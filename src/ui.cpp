#include "ui.h"

#include <U8g2lib.h>

#include "pins.h"
#include "messages.h"
#include "meshcore.h"

// A 1.3" panel is an SH1106; the 0.96" ones are SSD1306. Build with
// -DPAGER_OLED_SSD1306 for the other controller - nothing else changes.
// U8G2_R2, not R0: the panel is mounted upside down on this board. The
// previous firmware needed displayio's rotation=180 for the same reason
// (LoRa433APRSpager code.py). U8g2 applies the SH1106's 2-column offset below
// the rotation, in the tile layer, so it stays correct either way up.
#ifdef PAGER_OLED_SSD1306
static U8G2_SSD1306_128X64_NONAME_F_4W_SW_SPI oled(
    U8G2_R2, PIN_OLED_SCK, PIN_OLED_MOSI, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
#else
static U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI oled(
    U8G2_R2, PIN_OLED_SCK, PIN_OLED_MOSI, PIN_OLED_CS, PIN_OLED_DC, PIN_OLED_RST);
#endif

static bool started = false;
static bool screenOn = true;

#define W 128
#define H  64

#define FONT_SMALL  u8g2_font_5x7_tf
#define FONT_TINY   u8g2_font_4x6_tf
#define FONT_BODY   u8g2_font_6x10_tf
#define FONT_BIG    u8g2_font_7x13B_tf

bool Ui::begin() {
    started = oled.begin();
    if (started) {
        oled.setFontMode(1);          // transparent, so text over a filled box XORs
        oled.setDrawColor(1);
        oled.clearBuffer();
        oled.sendBuffer();
        screenOn = true;
    }
    return started;
}

bool Ui::present() { return started; }

void Ui::wake() {
    if (!started || screenOn) return;
    oled.setPowerSave(0);
    screenOn = true;
}

void Ui::sleep() {
    if (!started || !screenOn) return;
    oled.setPowerSave(1);
    screenOn = false;
}

bool Ui::awake() { return screenOn; }

// ---- small helpers ----

// "12s", "7m", "3h", "2d" - relative to reception, using millis() rather
// than the wall clock, because the pager often has no wall clock.
static void fmtAge(uint32_t rxMs, char *out, size_t sz) {
    if (!rxMs) { snprintf(out, sz, "--"); return; }
    uint32_t s = (millis() - rxMs) / 1000;
    if (s < 60)        snprintf(out, sz, "%lus", (unsigned long)s);
    else if (s < 3600) snprintf(out, sz, "%lum", (unsigned long)(s / 60));
    else if (s < 86400) snprintf(out, sz, "%luh", (unsigned long)(s / 3600));
    else               snprintf(out, sz, "%lud", (unsigned long)(s / 86400));
}

static void fmtClock(const UiModel &m, char *out, size_t sz) {
    if (!m.clockValid) { snprintf(out, sz, "--:--"); return; }
    uint32_t secOfDay = m.nowUnix % 86400u;
    snprintf(out, sz, "%02u:%02u", (unsigned)(secOfDay / 3600), (unsigned)((secOfDay / 60) % 60));
}

// Greedy word wrap against the font currently selected. Breaks a word that
// cannot fit on a line of its own rather than dropping it.
static int wrapText(const char *s, int maxPx, char lines[][40], int maxLines, int lineSz) {
    int   nLines = 0;
    char  cur[40];
    int   curLen = 0;
    cur[0] = '\0';

    while (*s && nLines < maxLines) {
        while (*s == ' ') s++;                    // collapse runs of spaces
        if (!*s) break;

        const char *wEnd = s;
        while (*wEnd && *wEnd != ' ') wEnd++;
        int wLen = (int)(wEnd - s);

        char word[40];
        int  wCopy = wLen < lineSz - 1 ? wLen : lineSz - 1;
        memcpy(word, s, wCopy);
        word[wCopy] = '\0';

        char probe[80];
        snprintf(probe, sizeof(probe), "%s%s%s", cur, curLen ? " " : "", word);
        if (oled.getStrWidth(probe) <= maxPx) {
            snprintf(cur, sizeof(cur), "%s", probe);
            curLen = (int)strlen(cur);
        } else if (curLen) {
            snprintf(lines[nLines++], lineSz, "%s", cur);
            cur[0] = '\0';
            curLen = 0;
            continue;                             // retry this word on the new line
        } else {
            // A single word wider than the line: chop it at the last
            // character that fits, so it wraps instead of vanishing.
            int fit = wCopy;
            while (fit > 1) {
                word[fit] = '\0';
                if (oled.getStrWidth(word) <= maxPx) break;
                fit--;
            }
            snprintf(lines[nLines++], lineSz, "%s", word);
            s += fit;
            continue;
        }
        s = wEnd;
    }
    if (curLen && nLines < maxLines) snprintf(lines[nLines++], lineSz, "%s", cur);
    return nLines;
}

// ---- chrome ----

static void drawBattery(int x, int y, int pct) {
    oled.drawFrame(x, y, 12, 7);
    oled.drawBox(x + 12, y + 2, 2, 3);            // the nub
    int fill = (pct * 10) / 100;
    if (fill > 10) fill = 10;
    if (fill < 0) fill = 0;
    if (fill) oled.drawBox(x + 1, y + 1, fill, 5);
}

// Right-aligned at the top right; returns the x it starts at. On battery, a
// cell filled to its level; on USB, the word and the percentage.
static int drawPower(const UiModel &m) {
    if (m.power == PWR_BATTERY) {
        drawBattery(W - 16, 1, m.battPct);
        return W - 16;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "USB %d%%", m.battPct);
    int x = W - oled.getStrWidth(buf);
    oled.drawStr(x, 7, buf);
    return x;
}

static void drawStatusBar(const UiModel &m) {
    char buf[32];
    oled.setFont(FONT_SMALL);

    fmtClock(m, buf, sizeof(buf));
    oled.drawStr(0, 7, buf);

    // Laid out from the right, because the power reading changes width: the
    // power, then the badge left of it, then the call in whatever is left.
    int right = drawPower(m);

    const char *badge = nullptr;
    bool boxed = false;
    if (!m.radioOk)         badge = "RF!";
    else if (!m.identityOk) badge = "ID!";
    else {
        int unread = Messages::unread();
        if (unread > 0) {
            snprintf(buf, sizeof(buf), "%d", unread > 99 ? 99 : unread);
            badge = buf;
            boxed = true;
        }
    }
    if (badge) {
        int wpx = oled.getStrWidth(badge);
        int x = right - 3 - wpx;
        if (boxed) {
            oled.drawBox(x - 1, 0, wpx + 3, 9);
            oled.setDrawColor(0);
            oled.drawStr(x + 1, 7, badge);
            oled.setDrawColor(1);
        } else {
            oled.drawStr(x, 7, badge);
        }
        right = x - 1;
    }

    // The call is the identity the alarm keys off, so it is always on screen -
    // clipped to the gap before the badge rather than allowed to run under it.
    char call[16];
    snprintf(call, sizeof(call), "%s", m.cfg->call);
    while (call[0] && oled.getStrWidth(call) > right - 2 - 32) call[strlen(call) - 1] = '\0';
    oled.drawStr(32, 7, call);

    oled.drawHLine(0, 9, W);
}

static void drawFooter(const UiModel &m) {
    char buf[32];
    oled.setFont(FONT_SMALL);
    oled.drawHLine(0, 53, W);

    // A toast owns the whole footer while it lasts: it is the answer to the
    // press that just happened, and that matters more than the hint.
    if (m.toast[0] && (int32_t)(millis() - m.toastUntilMs) < 0) {
        int wpx = oled.getStrWidth(m.toast);
        oled.drawBox(0, 54, W, 10);
        oled.setDrawColor(0);
        oled.drawStr((W - wpx) / 2, 62, m.toast);
        oled.setDrawColor(1);
        return;
    }

    // The pager's own controls mean nothing in app mode. What is worth a
    // glance is that the radio is still hearing things.
    if (m.view == VIEW_APP) {
        char age[8];
        snprintf(buf, sizeof(buf), "nodes %d", m.nodes);
        oled.drawStr(0, 62, buf);
        fmtAge(m.lastRxMs, age, sizeof(age));
        snprintf(buf, sizeof(buf), "rx %s", age);
        oled.drawStr(W - oled.getStrWidth(buf), 62, buf);
        return;
    }

    int count = Messages::count();
    if (count > 0 && m.sel < count) snprintf(buf, sizeof(buf), "%d/%d", m.sel + 1, count);
    else                            snprintf(buf, sizeof(buf), "INFO");
    oled.drawStr(0, 62, buf);

    const char *hint = m.holdIsFetch ? "HOLD:FETCH" : "HOLD:DEL";
    oled.drawStr(W - oled.getStrWidth(hint), 62, hint);
}

// ---- views ----

static void drawEmpty(const UiModel &m) {
    oled.setFont(FONT_BIG);
    const char *t = "NO MESSAGES";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 30, t);
    oled.setFont(FONT_SMALL);
    const char *h = m.holdIsFetch ? "hold to fetch mail" : "listening";
    oled.drawStr((W - oled.getStrWidth(h)) / 2, 44, h);
}

static void drawMessage(const UiModel &m) {
    const PagerMessage *msg = Messages::get(m.sel);
    if (!msg) { drawEmpty(m); return; }

    char buf[40], age[8];
    oled.setFont(FONT_SMALL);

    // Header: who it is from and how old, with the channel behind it. A
    // message addressed to us is inverted so the one that matters is obvious
    // in a list of channel chatter.
    fmtAge(msg->rxMs, age, sizeof(age));
    snprintf(buf, sizeof(buf), "%s %s", msg->chan, msg->from);
    if (msg->mention) {
        oled.drawBox(0, 10, W, 9);
        oled.setDrawColor(0);
    }
    oled.drawStr(1, 17, buf);
    oled.drawStr(W - oled.getStrWidth(age) - 1, 17, age);
    if (msg->mention) oled.setDrawColor(1);

    char lines[4][40];
    int n = wrapText(msg->text, W - 2, lines, 4, 40);
    for (int i = 0; i < n; i++) oled.drawStr(1, 27 + i * 8, lines[i]);

    if (msg->unread) {
        // An unread marker in the corner, so scrolling past one is visible.
        oled.drawDisc(W - 4, 23, 2);
    }
}

static void drawStatus(const UiModel &m) {
    char buf[48];
    oled.setFont(FONT_SMALL);

    const uint8_t *pk = Mesh::publicKey();
    snprintf(buf, sizeof(buf), "id %02X%02X%02X%02X", pk[0], pk[1], pk[2], pk[3]);
    oled.drawStr(0, 18, buf);

    if (m.locator[0])
        snprintf(buf, sizeof(buf), "%s %s", m.locator,
                 m.posSource == POS_APP ? "(app)" : m.posSource == POS_CONFIG ? "(set)" : "(heard)");
    else
        snprintf(buf, sizeof(buf), "no position yet");
    oled.drawStr(0, 27, buf);

    snprintf(buf, sizeof(buf), "rptr %d  adv %lu", m.repeaters, (unsigned long)m.adverts);
    oled.drawStr(0, 36, buf);

    if (m.lastRxMs) {
        char age[8];
        fmtAge(m.lastRxMs, age, sizeof(age));
        snprintf(buf, sizeof(buf), "rx %ddBm %.1fdB %s", m.lastRssi, m.lastSnrQ / 4.0, age);
    } else {
        snprintf(buf, sizeof(buf), "nothing heard yet");
    }
    oled.drawStr(0, 45, buf);

    // On USB the charger lifts the voltage and the percentage reads high; it
    // is shown anyway, as a rough "how far along".
    if (m.power == PWR_CHARGING)
        snprintf(buf, sizeof(buf), "%.2fV %d%% charging", (double)m.battVolts, m.battPct);
    else if (m.power == PWR_FULL)
        snprintf(buf, sizeof(buf), "%.2fV %d%% charged", (double)m.battVolts, m.battPct);
    else
        snprintf(buf, sizeof(buf), "%.2fV %d%% battery", (double)m.battVolts, m.battPct);
    oled.drawStr(0, 52, buf);
}

static void drawAlert(const UiModel &m) {
    const PagerMessage *msg = Messages::get(m.sel);

    // Blink the banner: on a device meant to be noticed from across a room,
    // a static screen is the one thing the eye filters out.
    bool on = ((millis() / 400) & 1) != 0;
    if (on) oled.drawBox(0, 10, W, 20);

    oled.setDrawColor(on ? 0 : 1);
    oled.setFont(FONT_BIG);
    const char *t = "MESSAGE";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 24, t);
    oled.setDrawColor(1);

    oled.setFont(FONT_SMALL);
    if (msg) {
        char buf[40];
        snprintf(buf, sizeof(buf), "from %s", msg->from);
        oled.drawStr((W - oled.getStrWidth(buf)) / 2, 39, buf);

        char lines[1][40];
        int n = wrapText(msg->text, W - 2, lines, 1, 40);
        if (n) oled.drawStr((W - oled.getStrWidth(lines[0])) / 2, 49, lines[0]);
    }
}

static void drawFetch(const UiModel &m) {
    oled.setFont(FONT_BIG);
    const char *t = "FETCHING";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 26, t);

    // A bar that fills over the wait, so a fetch that is going to time out
    // looks different from one that just started.
    uint32_t elapsed = millis() - m.fetchStartedMs;
    int wpx = (int)((elapsed * (W - 20)) / 20000);
    if (wpx > W - 20) wpx = W - 20;
    oled.drawFrame(10, 34, W - 20, 7);
    if (wpx > 0) oled.drawBox(10, 34, wpx, 7);

    oled.setFont(FONT_SMALL);
    char buf[32];
    snprintf(buf, sizeof(buf), "%s  %lus", m.cfg->mboxChannel, (unsigned long)(elapsed / 1000));
    oled.drawStr((W - oled.getStrWidth(buf)) / 2, 50, buf);
}

static void drawApp(const UiModel &) {
    oled.setFont(FONT_BIG);
    const char *t = "APP MODE";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 27, t);

    oled.setFont(FONT_SMALL);
    t = "messages go to the app";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 39, t);
    t = "close it to resume";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 48, t);
}

static void drawBoot(const UiModel &m) {
    oled.setFont(FONT_BIG);
    const char *t = "MeshCore";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 22, t);
    oled.setFont(FONT_BODY);
    t = "PAGER";
    oled.drawStr((W - oled.getStrWidth(t)) / 2, 36, t);

    oled.setFont(FONT_SMALL);
    oled.drawStr((W - oled.getStrWidth("RF.Guru")) / 2, 46, "RF.Guru");
    if (m.line[0]) {
        char lines[1][40];
        int n = wrapText(m.line, W, lines, 1, 40);
        if (n) oled.drawStr((W - oled.getStrWidth(lines[0])) / 2, 62, lines[0]);
    }
}

static void drawFatal(const UiModel &m) {
    oled.drawBox(0, 0, W, 12);
    oled.setDrawColor(0);
    oled.setFont(FONT_BODY);
    oled.drawStr(2, 10, "SETUP FAILED");
    oled.setDrawColor(1);

    oled.setFont(FONT_SMALL);
    char lines[4][40];
    int n = wrapText(m.line, W - 2, lines, 4, 40);
    for (int i = 0; i < n; i++) oled.drawStr(1, 24 + i * 9, lines[i]);
}

void Ui::render(const UiModel &m) {
    if (!started || !screenOn) return;
    oled.clearBuffer();

    switch (m.view) {
        case VIEW_BOOT:
            drawBoot(m);
            break;
        case VIEW_FATAL:
            drawFatal(m);
            break;
        default:
            drawStatusBar(m);
            if (m.view == VIEW_ALERT)        drawAlert(m);
            else if (m.view == VIEW_FETCH)   drawFetch(m);
            else if (m.view == VIEW_STATUS)  drawStatus(m);
            else if (m.view == VIEW_APP)     drawApp(m);
            else if (Messages::count() == 0) drawEmpty(m);
            else                             drawMessage(m);
            drawFooter(m);
            break;
    }

    oled.sendBuffer();
}
