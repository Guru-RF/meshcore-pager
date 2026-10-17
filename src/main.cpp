// ============================================================
// RF.Guru MeshCore Pager
//
// A one-button MeshCore receiver. It listens on the public channels named in
// config.txt, shows everything it can decrypt, and makes a noise when
// something is addressed to it. It advertises itself, learns roughly where
// it is from the repeaters it hears, and gets its clock off #mesh.
//
// The interface is one button:
//
//   press  - silence the alarm, or step to the next message (and one step
//            past the oldest lands on the status page)
//   hold   - fetch mail from #mbox, or - within fetchCooldown of the last
//            fetch - delete the message on screen
//
// Everything the pager knows lives in the UiModel below and is drawn from
// there, so the screen cannot disagree with the state that drives the LED.
//
// A MeshCore app on the USB port can use it as its radio as well - see
// include/companion.h. While one is attached the pager is in app mode: the
// app is the interface, so nothing above happens - no store, no alarm, and
// the button only says so. It all comes back when the app lets go.
// ============================================================

#include <Arduino.h>
#include <FatFS.h>
#include <FatFSUSB.h>
#include <hardware/watchdog.h>

#include "alerts.h"
#include "companion.h"
#include "config_file.h"
#include "grid.h"
#include "messages.h"
#include "meshcore.h"
#include "pagerclock.h"
#include "pins.h"
#include "radio.h"
#include "ui.h"

#define VERSION      "1.1.0"
#define VERSION_DATE "15 Oct 2026"

static PagerConfig cfg;
static UiModel     ui;

static MeshChannel channels[PAGER_MAX_CHANNELS];
static MeshChannel mboxChan;
static MeshChannel timeChan;

// ---- state ----

static bool     everFetched    = false;
static uint32_t lastFetchMs    = 0;
static bool     fetchPending   = false;
static uint32_t fetchStartedMs = 0;
static const uint32_t FETCH_TIMEOUT_MS = 20000;

static uint32_t nextAdvertMs   = 0;
static bool     clockWasValid  = false;

static uint32_t nextTimeReqMs  = 20000;    // first request 20 s after boot
static int      timeReqTries   = 0;

// The repeater answers a #mesh time or date request at the next minute
// boundary, and the flood back takes a few seconds more. A request repeated
// sooner than this goes out again while its answer is still on the way.
static const uint32_t TIME_RETRY_MS = 90000;

static uint32_t lastInteractionMs = 0;
static uint32_t lastRenderMs      = 0;
static bool     uiDirty           = true;

static uint32_t nextBattMs = 0;
static float    battFilt   = 0.0f;

static bool     portWasOpen = false;
static bool     appWasActive = false;

// By route, for the app's statistics page. Radio.cpp counts the rest.
static uint32_t nRecvFlood = 0, nRecvDirect = 0, nSentFlood = 0, nSentDirect = 0;

static bool isFloodHeader(uint8_t header) {
    uint8_t rt = header & PH_ROUTE_MASK;
    return rt == ROUTE_TYPE_FLOOD || rt == ROUTE_TYPE_TRANSPORT_FLOOD;
}

// Repeaters heard, for the position estimate. Keyed by the first four bytes
// of the public key, which is what identifies a node on the mesh.
struct HeardRepeater {
    uint8_t  id[4];
    int16_t  rssi;
    uint32_t lastMs;
    double   lat, lon;
    bool     used;
};
static HeardRepeater repeaters[8];

// Where the pager says it is. Three sources, best first: an app that has
// been told, the locator in config.txt, and the strongest repeater heard.
// Each is kept apart, so withdrawing one falls back to the next rather than
// to nothing - or to a stale copy of the one withdrawn.
struct Fix {
    bool   valid;
    double lat, lon;
    char   loc[8];
};
static Fix appFix, cfgFix, heardFix;

static double myLat = 0.0, myLon = 0.0;     // the chosen one, as advertised
static bool   haveLocation = false;

// ---- small helpers ----

static void logf(const char *fmt, ...) {
    // An app on the port reads it as frames; a line of text in the middle
    // would knock its parser out of step.
    if (!cfg.debug || Companion::active()) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Serial.printf("%s\r\n", buf);
}

static void toast(const char *text) {
    snprintf(ui.toast, sizeof(ui.toast), "%s", text);
    ui.toastUntilMs = millis() + 1800;
    uiDirty = true;
}

// The base call, up to any '-', which is how the repeater's mailbox indexes
// mentions - so @OR7F reaches OR7F-12 and OR7F-7 alike.
static void baseCall(const char *s, char *out, size_t sz) {
    size_t i = 0;
    while (s && s[i] && s[i] != '-' && i + 1 < sz) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

// MeshCore dedups on a hash of the payload, and inside a "fetch" the
// timestamp is the only field that can differ between two of them - the body
// has to be exactly "fetch" for the repeater to recognise it. Before the
// clock is set, hand over a monotonic seconds count instead: it displays as
// 1970 on other clients, but two fetches never hash alike and get silently
// dropped as duplicates by every repeater that heard the first.
static uint32_t mcTimestamp() {
    uint32_t t = PagerClock::now();
    return t ? t : (millis() / 1000) + 1;
}

// LiPo, loaded, discharge curve flattened to eight points. A percentage off
// a resting-voltage table is a guess under load; it is here to say "nearly
// flat" rather than to be accurate in the middle.
static int battPercent(float v) {
    static const struct { float v; int pct; } curve[] = {
        {4.15f, 100}, {4.05f, 90}, {3.95f, 75}, {3.85f, 60},
        {3.75f, 45},  {3.65f, 30}, {3.55f, 15}, {3.40f, 5}, {3.20f, 0},
    };
    if (v >= curve[0].v) return 100;
    for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); i++) {
        if (v >= curve[i].v) {
            float span = curve[i - 1].v - curve[i].v;
            float frac = span > 0 ? (v - curve[i].v) / span : 0;
            return curve[i].pct + (int)(frac * (curve[i - 1].pct - curve[i].pct));
        }
    }
    return 0;
}

// ---- transmit ----

static bool sendFrame(const uint8_t *frame, int len) {
    if (len <= 0) return false;
    // Record it before it goes out: every repeater in range re-floods it, and
    // without this the pager would receive its own fetch back and display it
    // as an incoming message.
    Mesh::rememberFrame(frame, (size_t)len);
    bool ok = Radio::send(frame, (size_t)len);
    if (!ok) logf("tx failed (%d bytes)", len);
    else if (isFloodHeader(frame[0])) nSentFlood++;
    else nSentDirect++;
    return ok;
}

static bool sendChannelText(const MeshChannel &ch, const char *text, uint8_t route) {
    if (!ch.valid) return false;
    uint8_t frame[MC_MAX_TRANS_UNIT];
    int n = Mesh::buildGroupText(frame, sizeof(frame), ch, cfg.meshName, text,
                                 mcTimestamp(), route);
    if (n < 0) return false;
    logf("tx %s: \"%s\" (%d bytes)", ch.name, text, n);
    return sendFrame(frame, n);
}

// `route` overrides config.txt's route= for this one advert, which is how
// the app asks for flood or zero-hop. Only the header carries the route -
// the signature covers the key, the timestamp and the app data - so it can
// be changed after signing.
static bool sendAdvert(int route = -1) {
    uint8_t frame[MC_MAX_TRANS_UNIT];
    int n = Mesh::buildAdvert(frame, sizeof(frame), cfg, haveLocation, myLat, myLon,
                              PagerClock::now());
    if (n < 0) return false;
    if (route >= 0) frame[0] = (uint8_t)((frame[0] & ~PH_ROUTE_MASK) | route);
    // Not in app mode: the advert still goes out, but a tick every hour from
    // a radio on a desk is noise, and the app shows the advert anyway.
    if (!Companion::active()) Alerts::play(ALERT_TX);
    if (!sendFrame(frame, n)) return false;
    ui.adverts++;
    logf("advert sent (%d bytes)%s", n, haveLocation ? " with position" : "");
    return true;
}

// ---- position ----

static void choosePosition() {
    const Fix *f   = nullptr;
    PosSource  src = POS_NONE;
    if (appFix.valid)        { f = &appFix;   src = POS_APP; }
    else if (cfgFix.valid)   { f = &cfgFix;   src = POS_CONFIG; }
    else if (heardFix.valid) { f = &heardFix; src = POS_HEARD; }

    haveLocation = (f != nullptr);
    if (f) { myLat = f->lat; myLon = f->lon; }

    const char *loc = f ? f->loc : "";
    if (src != ui.posSource || strcmp(loc, ui.locator) != 0) {
        snprintf(ui.locator, sizeof(ui.locator), "%s", loc);
        ui.posSource = src;
        static const char *const from[] = {"", "repeater", "config.txt", "app"};
        if (f) logf("position: %s (%.4f, %.4f) from %s", loc, myLat, myLon, from[src]);
        else   logf("position: none");
        uiDirty = true;
    }
}

// The strongest repeater heard in the last six hours, with the precision
// thrown away: what the pager actually knows is "within earshot of that
// repeater", and the centre of its Maidenhead square says exactly that.
// Copying the repeater's own coordinates would claim to be standing on its
// mast.
static void learnPosition() {
    int best = -1;
    uint32_t now = millis();
    for (int i = 0; i < (int)(sizeof(repeaters) / sizeof(repeaters[0])); i++) {
        if (!repeaters[i].used) continue;
        if (now - repeaters[i].lastMs > 6UL * 3600UL * 1000UL) continue;
        if (best < 0 || repeaters[i].rssi > repeaters[best].rssi) best = i;
    }
    if (best < 0) return;

    Fix f;
    if (!gridSnap(repeaters[best].lat, repeaters[best].lon, cfg.gridPrecision,
                  &f.lat, &f.lon, f.loc))
        return;
    f.valid  = true;
    heardFix = f;
    choosePosition();
}

static void recordRepeater(const uint8_t *pub, int16_t rssi, double lat, double lon) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < (int)(sizeof(repeaters) / sizeof(repeaters[0])); i++) {
        if (repeaters[i].used && memcmp(repeaters[i].id, pub, 4) == 0) { slot = i; break; }
        if (!repeaters[i].used && slot < 0) slot = i;
        if (repeaters[i].used && repeaters[i].lastMs < repeaters[oldest].lastMs) oldest = i;
    }
    if (slot < 0) slot = oldest;

    memcpy(repeaters[slot].id, pub, 4);
    repeaters[slot].used   = true;
    repeaters[slot].rssi   = rssi;
    repeaters[slot].lat    = lat;
    repeaters[slot].lon    = lon;
    repeaters[slot].lastMs = millis();

    // Counted from the table, not incremented here: the oldest slot gets
    // recycled when a ninth repeater turns up, and a counter would have gone
    // on climbing past the number actually being tracked.
    int used = 0;
    for (unsigned i = 0; i < sizeof(repeaters) / sizeof(repeaters[0]); i++)
        if (repeaters[i].used) used++;
    ui.repeaters = used;
}

// ---- receive ----

static bool bodyMentionsUs(const char *body) {
    char base[16];
    baseCall(cfg.call, base, sizeof(base));
    if (!base[0]) return false;
    size_t bl = strlen(base);

    for (const char *p = strchr(body, '@'); p; p = strchr(p + 1, '@')) {
        if (strncasecmp(p + 1, base, bl) != 0) continue;
        // "@OR7F", "@OR7F-12" and "@OR7F:" all match; "@OR7FX" is somebody
        // else. Anything that is not another call character ends the match.
        char after = p[1 + bl];
        if (!isalnum((unsigned char)after)) return true;
    }
    return false;
}

static void storeMessage(const MeshChannel &ch, const MeshPacket &p, uint32_t ts,
                         const char *sender, const char *body, bool mention) {
    PagerMessage m;
    memset(&m, 0, sizeof(m));
    snprintf(m.chan, sizeof(m.chan), "%s", ch.name);
    snprintf(m.from, sizeof(m.from), "%s", (sender && sender[0]) ? sender : "?");
    snprintf(m.text, sizeof(m.text), "%s", body);
    m.ts      = ts;
    m.rxMs    = millis();
    m.rssi    = p.rssi;
    m.snrQ    = p.snrQ;
    m.mention = mention;
    m.unread  = true;

    Messages::add(m);
    ui.sel = 0;                         // the newest message is what you want to see
    uiDirty = true;
}

static void handleChannelText(int chan, const MeshPacket &p, uint32_t ts,
                              const char *sender, const char *body) {
    const MeshChannel &ch = channels[chan];

    // Our own message, re-flooded back at us by a repeater whose copy has a
    // different path. The dedup hash catches this already; the name check is
    // the backstop for the case where it has aged out of the ring.
    if (sender[0] && strcasecmp(sender, cfg.meshName) == 0) return;

    logf("rx %s <%s> %s  (%d dBm)", ch.name, sender, body, p.rssi);

    // Queued for the app whether or not one is attached, so one that
    // attaches later still gets it.
    Companion::onChannelText(chan, p, ts, sender, body);

    // The clock. Both lines are also stored and shown, because "display
    // everything you hear" is the whole point of the device - they simply
    // never alarm.
    if (timeChan.valid && strcmp(ch.name, timeChan.name) == 0) {
        bool wasValid = PagerClock::valid();
        if (PagerClock::applyDateLine(body) || PagerClock::applyTimeLine(body)) {
            if (!wasValid && PagerClock::valid()) toast("CLOCK SET");
            uiDirty = true;
        }
    }

    // App mode. The app has the message and shows it; a copy in the store
    // would be a second inbox to clear, and an alarm would be about
    // something already on the phone.
    if (Companion::active()) return;

    bool mention = bodyMentionsUs(body);

    // A mailbox reply to the fetch we sent. It arrives as an ordinary #mbox
    // message routed back to us, so the only thing that marks it is that we
    // asked and it starts the way the repeater's replies start.
    bool fetchReply = false;
    if (fetchPending && mboxChan.valid && strcmp(ch.name, mboxChan.name) == 0 &&
        strncasecmp(body, "mbox", 4) == 0) {
        fetchReply   = true;
        mention      = true;                  // it is addressed to us by construction
        fetchPending = false;
        ui.fetchStartedMs = 0;
        if (strncasecmp(body, "mbox: no messages", 17) == 0) {
            Alerts::play(ALERT_FETCH_NONE);
            toast("MAILBOX EMPTY");
            mention = false;                  // nothing to alarm about
        } else {
            Alerts::play(ALERT_FETCH_OK);
            toast("MAIL");
        }
    }

    storeMessage(ch, p, ts, sender, body, mention);

    bool alarm = !fetchReply && (mention || !strcasecmp(cfg.alertOn, "all"));
    Ui::wake();
    lastInteractionMs = millis();

    if (alarm) {
        ui.view = VIEW_ALERT;
        Alerts::play(ALERT_MESSAGE, cfg.alertSeconds ? (uint32_t)cfg.alertSeconds * 1000 : 0);
    } else if (!fetchReply) {
        Alerts::play(ALERT_CHIRP);
        if (ui.view == VIEW_STATUS) ui.view = VIEW_MESSAGE;
    } else {
        ui.view = VIEW_MESSAGE;
    }
}

static void handleAdvert(const MeshPacket &p) {
    uint8_t pub[MC_PUB_KEY_SIZE];
    if (!Mesh::advertVerify(p, pub)) {
        logf("advert with a bad signature, dropped");
        return;
    }

    uint8_t  type = 0;
    char     name[32];
    bool     hasLoc = false;
    double   lat = 0, lon = 0;
    uint32_t ts = 0;
    if (!Mesh::advertInfo(p, &type, name, sizeof(name), &hasLoc, &lat, &lon, &ts)) return;

    logf("advert <%s> type %u %s (%d dBm)", name, type,
         hasLoc ? "with position" : "no position", p.rssi);

    Companion::onAdvert(pub, type, name, hasLoc, lat, lon, ts);

    // Only a repeater's clock is worth borrowing: it is the class of node
    // that has one. And only until #mesh answers - see pagerclock.h. A
    // hotspot advertises as a repeater too, so it counts for both.
    if (type == ADV_TYPE_REPEATER) {
        bool wasValid = PagerClock::valid();
        PagerClock::seedFromAdvert(ts);
        if (!wasValid && PagerClock::valid()) { toast("CLOCK SET"); uiDirty = true; }

        // Only an advert heard straight from the repeater says how strong
        // that repeater is here. A re-flooded copy arrives at the strength
        // of whichever repeater passed it on - perhaps the one next door,
        // carrying the advert of one 100 km away.
        if (hasLoc && Mesh::pathHashCount(p) == 0) {
            recordRepeater(pub, p.rssi, lat, lon);
            learnPosition();
        }
    }
}

static void handlePacket(const uint8_t *raw, int len, int16_t rssi, int8_t snrQ) {
    MeshPacket p;
    if (!Mesh::parse(p, raw, (size_t)len)) return;
    p.rssi = rssi;
    p.snrQ = snrQ;

    // A flooded message reaches a pager from every repeater in range. Without
    // this it would alarm once per copy.
    if (Mesh::seenBefore(p)) return;

    ui.lastRssi = rssi;
    ui.lastSnrQ = snrQ;
    ui.lastRxMs = millis();

    uint8_t type = Mesh::payloadType(p);
    if (type == PAYLOAD_TYPE_ADVERT) { handleAdvert(p); return; }

    // Private unicast is not readable without a key exchange this pager does
    // not do, and a leaf does not forward, so there is nothing to be done
    // with anything else.
    if (type != PAYLOAD_TYPE_GRP_TXT) return;

    for (int i = 0; i < cfg.nChannels; i++) {
        // Sized for the whole plaintext rather than for the message store:
        // the app gets the text uncut, and storeMessage() cuts its own copy.
        uint32_t ts = 0;
        char sender[32];
        char body[MC_MAX_PACKET_PAYLOAD];
        if (!Mesh::decryptGroupText(channels[i], p, &ts, sender, sizeof(sender),
                                    body, sizeof(body)))
            continue;
        handleChannelText(i, p, ts, sender, body);
        return;
    }
}

// ---- the button ----

#define BTN_DEBOUNCE_MS  25
#define BTN_LONG_MS     700

static bool     btnRaw        = false;
static bool     btnStable     = false;
static uint32_t btnChangeMs   = 0;
static uint32_t btnDownMs     = 0;
static bool     btnLongFired  = false;
static bool     silencedByThisPress = false;
static bool     wokeByThisPress     = false;

static bool holdWouldFetch() {
    if (!everFetched) return true;
    return (int32_t)(millis() - lastFetchMs) >= (int32_t)((uint32_t)cfg.fetchCooldown * 1000);
}

static void doFetch() {
    if (!mboxChan.valid) {
        toast("NO MBOX CHANNEL");
        Alerts::play(ALERT_ERROR);
        return;
    }
    // The repeater only treats FLOOD packets on #mbox as commands - its
    // routed reply comes back DIRECT and must pass other mailboxes
    // untouched - so this is FLOOD whatever config.txt says about adverts.
    Alerts::play(ALERT_FETCH);
    if (sendChannelText(mboxChan, "fetch", ROUTE_TYPE_FLOOD)) {
        everFetched    = true;
        lastFetchMs    = millis();
        fetchPending   = true;
        fetchStartedMs = millis();
        ui.fetchStartedMs = fetchStartedMs;
        ui.view = VIEW_FETCH;
        toast("FETCH SENT");
    } else {
        Alerts::play(ALERT_ERROR);
        toast("TX FAILED");
    }
    uiDirty = true;
}

static void doDelete() {
    int count = Messages::count();
    if (count == 0 || ui.sel >= count) {
        Alerts::play(ALERT_ERROR);
        toast("NOTHING TO DELETE");
        return;
    }
    Messages::remove(ui.sel);

    // Stay on the message that has taken this one's place, so deleting a run
    // of messages is one repeated gesture. Deleting the oldest steps back
    // rather than falling through onto the status page.
    int left = Messages::count();
    if (ui.sel >= left) ui.sel = left > 0 ? left - 1 : 0;
    ui.view = VIEW_MESSAGE;

    Alerts::play(ALERT_DELETE);
    toast("DELETED");
    uiDirty = true;
}

static void onPressStart() {
    lastInteractionMs = millis();

    // A press on a sleeping screen only wakes it. Otherwise the press that
    // means "what does it say?" scrolls past the thing you wanted to read.
    wokeByThisPress = !Ui::awake();
    Ui::wake();
    uiDirty = true;

    // In app mode the button has nothing to do but say so - a press that
    // seemed to do nothing would read as a broken pager.
    if (Companion::active()) {
        ui.view = VIEW_APP;
        toast("APP MODE - USE THE APP");
        Alerts::play(ALERT_ACK);
        return;
    }

    // "until one presses on the button": any press stops the alarm, and it
    // stops on the way DOWN rather than on release, so holding the button to
    // fetch does not mean holding it through another second of siren.
    if (Alerts::alarming()) {
        Alerts::stop();
        Messages::markRead(0);
        silencedByThisPress = true;
        ui.view = VIEW_MESSAGE;
    }
}

static void onShortPress() {
    if (Companion::active()) return;          // answered on the way down

    // The press that silenced the alarm leaves you looking at the message
    // that caused it. Stepping away from it would be the one thing you did
    // not ask for.
    if (silencedByThisPress || wokeByThisPress) {
        silencedByThisPress = false;
        wokeByThisPress     = false;
        Alerts::play(ALERT_ACK);
        return;
    }

    Alerts::play(ALERT_ACK);
    int count = Messages::count();
    ui.sel = (ui.sel + 1) % (count + 1);          // one past the oldest = status page
    if (ui.sel < count) {
        Messages::markRead(ui.sel);
        ui.view = VIEW_MESSAGE;
    } else {
        ui.view = VIEW_STATUS;
    }
    uiDirty = true;
}

static void onLongPress() {
    if (Companion::active()) return;

    // A hold is deliberate: it does what it says whether or not it also
    // silenced an alarm or woke the screen on the way down.
    silencedByThisPress = false;
    wokeByThisPress     = false;
    if (holdWouldFetch()) doFetch();
    else                  doDelete();
}

static void serviceButton() {
    bool raw = (digitalRead(PIN_BUTTON) == LOW);
    uint32_t now = millis();

    if (raw != btnRaw) { btnRaw = raw; btnChangeMs = now; }
    if ((now - btnChangeMs) >= BTN_DEBOUNCE_MS && raw != btnStable) {
        btnStable = raw;
        if (raw) {
            btnDownMs    = now;
            btnLongFired = false;
            onPressStart();
        } else if (!btnLongFired) {
            onShortPress();
        }
    }

    // Fire the hold at the threshold rather than on release, so the pager
    // answers while your thumb is still down and you know it registered.
    if (btnStable && !btnLongFired && (now - btnDownMs) >= BTN_LONG_MS) {
        btnLongFired = true;
        onLongPress();
    }
}

// ---- schedulers ----

static void serviceTimeSync() {
    if (!timeChan.valid) return;

    // An answer on #mesh - to this pager or to anyone else who asked - or an
    // app setting the clock is a resync, and the next one is due an interval
    // after it. Without this, a request scheduled before the answer came in
    // still goes out, asking for what it has just been told.
    uint32_t interval = (uint32_t)cfg.timeSyncInterval * 1000;
    if (PagerClock::synced()) {
        uint32_t since = millis() - PagerClock::syncedAtMs();
        if (since < interval) {
            uint32_t due = millis() + (interval - since);
            if ((int32_t)(due - nextTimeReqMs) > 0) nextTimeReqMs = due;
        }
    }

    if ((int32_t)(millis() - nextTimeReqMs) < 0) return;

    if (PagerClock::valid()) {
        // The periodic resync - there is no RTC here, only the RP2040's
        // crystal - or, for a clock only seeded from an advert, the request
        // for the exact minute.
        sendChannelText(timeChan, "time", ROUTE_TYPE_FLOOD);
        nextTimeReqMs = millis() + interval;
        return;
    }

    // Not set yet. Ask for what is missing and nothing else: the date first,
    // since a time on its own cannot be turned into an epoch, then the time.
    sendChannelText(timeChan, PagerClock::haveDate() ? "time" : "date", ROUTE_TYPE_FLOOD);
    timeReqTries++;
    // Back off hard once it is clear nothing is answering: a pager that keys
    // up every 90 seconds forever is worse than a pager with no clock.
    nextTimeReqMs = millis() + (timeReqTries < 6 ? TIME_RETRY_MS : 1800000);
}

static void serviceAdvert() {
    // No advert until the clock is real. MeshCore uses the advert timestamp
    // for freshness, so a fabricated one poisons the mesh - and this board
    // has neither an RTC nor a GPS, so silence indoors and out of range is
    // deliberate, not a gap.
    if (!PagerClock::valid()) return;
    if ((int32_t)(millis() - nextAdvertMs) < 0) return;
    sendAdvert();
    nextAdvertMs = millis() + (uint32_t)cfg.advertInterval * 1000;
}

static void serviceFetchTimeout() {
    if (!fetchPending) return;
    if ((millis() - fetchStartedMs) < FETCH_TIMEOUT_MS) return;
    fetchPending = false;
    ui.fetchStartedMs = 0;
    ui.view = VIEW_MESSAGE;
    Alerts::play(ALERT_ERROR);
    toast("NO REPLY");
    logf("fetch timed out");
}

// The app's side of the USB port. DTR dropping is the only sign a serial
// client gives that it has gone; until then, once it has spoken, the port
// is its. A pulled cable ends it too, which matters for a pager that leaves
// the desk in app mode: the RP2040 port forces VBUS detect on, so TinyUSB
// never sees an unplug - but the idle bus reads as a suspend, and
// tud_ready() turns that into `Serial` going false.
static void serviceCompanion() {
    bool open = (bool)Serial;
    if (portWasOpen && !open) Companion::portClosed();
    portWasOpen = open;

    // Bounded, so a host pouring bytes in cannot starve the radio poll.
    for (int i = 0; i < 256 && Serial.available() > 0; i++)
        Companion::feed((uint8_t)Serial.read());
    Companion::service();

    if (Companion::active() != appWasActive) {
        appWasActive = Companion::active();
        if (appWasActive) {
            // Whatever the pager was in the middle of is the app's business
            // now. A running alarm is about a message the app is about to be
            // handed, and a fetch reply will reach the app as #mbox anyway.
            if (Alerts::alarming()) Alerts::stop();
            fetchPending = false;
            ui.view = VIEW_APP;
            toast("APP MODE");
        } else {
            // The store is as the app found it: nothing was added or deleted
            // while it was attached.
            ui.sel  = 0;
            ui.view = VIEW_MESSAGE;
            toast("PAGER MODE");
        }
        Ui::wake();
        lastInteractionMs = millis();
        uiDirty = true;
    }
}

static void serviceBattery() {
    if ((int32_t)(millis() - nextBattMs) < 0) return;
    nextBattMs = millis() + 5000;

    float v = analogRead(PIN_VBAT_ADC) * (3.3f / 4095.0f) * VBAT_DIVIDER;
    battFilt = (battFilt == 0.0f) ? v : (battFilt * 0.8f + v * 0.2f);

    ui.battVolts = battFilt;
    ui.battPct   = battPercent(battFilt);
    ui.charging  = (digitalRead(PIN_CHARGE) == HIGH);
    uiDirty = true;
}

// The LED is the only thing still reporting once the screen has slept, which
// is what makes a screen timeout acceptable on a device you glance at.
static void serviceIdleLed() {
    // In app mode the pager is a radio on a desk, and a blinking one in a
    // dark room is a nuisance. Dark whenever the screen is.
    Alerts::setLedDark(Companion::active() && !Ui::awake());

    if (!Radio::ok())                  Alerts::setIdle(IDLE_BREATHE, 255,   0,   0);
    else if (Companion::active())      Alerts::setIdle(IDLE_PULSE,   160,   0, 255);
    else if (fetchPending)             Alerts::setIdle(IDLE_BREATHE,   0,  80, 255);
    else if (Messages::unread() > 0)   Alerts::setIdle(IDLE_PULSE,   255,  40,   0);
    else if (ui.charging)              Alerts::setIdle(IDLE_BREATHE, 255, 120,   0);
    else if (!PagerClock::valid())     Alerts::setIdle(IDLE_PULSE,   120, 120,   0);
    else                               Alerts::setIdle(IDLE_PULSE,     0, 255,   0);
}

static void serviceScreen() {
    uint32_t now = millis();

    if (cfg.screenTimeout > 0 && Ui::awake() && !Alerts::alarming() && !fetchPending &&
        (now - lastInteractionMs) > (uint32_t)cfg.screenTimeout * 1000) {
        Ui::sleep();
        return;
    }

    // Alert and fetch views animate; everything else only has to keep the
    // clock and the message age honest.
    uint32_t period = (ui.view == VIEW_ALERT || ui.view == VIEW_FETCH) ? 150 : 1000;
    if (!uiDirty && (now - lastRenderMs) < period) return;
    if (uiDirty && (now - lastRenderMs) < 60) return;      // coalesce a burst

    ui.nowUnix    = PagerClock::now();
    ui.clockValid = PagerClock::valid();
    ui.radioOk    = Radio::ok();
    ui.identityOk = Mesh::identityOk();
    ui.holdIsFetch = holdWouldFetch();
    ui.fetchStartedMs = fetchPending ? fetchStartedMs : 0;
    ui.nodes      = Companion::contacts();

    Ui::render(ui);
    lastRenderMs = now;
    uiDirty = false;
}

// ---- setup ----

// Make sure `name` is one of the channels the pager decrypts. Returns false
// only when the list is full and it is not already there.
static bool ensureSubscribed(const char *name) {
    if (!name || !name[0]) return true;
    for (int i = 0; i < cfg.nChannels; i++)
        if (strcmp(cfg.channels[i], name) == 0) return true;
    if (cfg.nChannels >= PAGER_MAX_CHANNELS) {
        Serial.printf("channels list is full - '%s' will not be readable\r\n", name);
        return false;
    }
    snprintf(cfg.channels[cfg.nChannels], PAGER_CHAN_LEN, "%s", name);
    cfg.nChannels++;
    return true;
}

static bool usbDriveReady(uint32_t) { return true; }

static void fatal(const char *msg) {
    ui.view = VIEW_FATAL;
    snprintf(ui.line, sizeof(ui.line), "%s", msg);
    Ui::render(ui);
    Serial.printf("SETUP FAILED: %s\r\n", msg);
    Alerts::play(ALERT_ERROR, 4000);
}

void setup() {
    Serial.begin(115200);

    pinMode(PIN_BUTTON, INPUT_PULLUP);      // no external pull-up on the board
    // GP13 needs the internal pull-down: the charger's CHG# drives it HIGH
    // through R21 while charging and leaves it floating otherwise, so with no
    // pull-down "not charging" reads as whatever the pad last saw.
    pinMode(PIN_CHARGE, INPUT_PULLDOWN);
    analogReadResolution(12);

    watchdog_enable(8000, true);

    memset(&ui, 0, sizeof(ui));
    ui.cfg  = &cfg;
    ui.view = VIEW_BOOT;

    configSetDefaults(cfg);                 // so Alerts::begin has real numbers
    Alerts::begin(cfg);

    if (!Ui::begin()) Serial.println("OLED did not start");
    Ui::render(ui);

    randomSeed(rp2040.hwrand32());

    bool haveConfig = false;
    if (!FatFS.begin()) {
        FatFS.format();
        FatFS.begin();
    }
    haveConfig = configLoad(cfg);
    Alerts::begin(cfg);                     // re-apply, now with the real settings

    snprintf(ui.line, sizeof(ui.line), haveConfig ? "config loaded" : "defaults - edit config.txt");
    Ui::render(ui);
    watchdog_update();

    // The identity has to be minted while the filesystem is still ours, so
    // this must run before FatFSUSB hands the drive to the host.
    char err[96] = "";
    if (!Mesh::begin(err, sizeof(err))) {
        fatal(err);
        // Keep going: without an identity the pager cannot advertise, but it
        // can still receive and alert, which is most of what it is for.
    }

    // The mailbox and clock channels have to be in the subscription list or
    // their replies arrive as undecryptable noise. Add them rather than
    // reporting a mistake nobody would see: naming mboxChannel is already a
    // statement that you want to hear it.
    ensureSubscribed(cfg.mboxChannel);
    ensureSubscribed(cfg.timeChannel);

    for (int i = 0; i < cfg.nChannels; i++) Mesh::channelInit(channels[i], cfg.channels[i]);
    Mesh::channelInit(mboxChan, cfg.mboxChannel);
    Mesh::channelInit(timeChan, cfg.timeChannel);

    snprintf(ui.line, sizeof(ui.line), "starting radio");
    Ui::render(ui);
    if (!Radio::begin(cfg, err, sizeof(err))) fatal(err);
    watchdog_update();

    // A locator in config.txt outranks the repeaters the pager hears; only
    // an app that says otherwise outranks it.
    if (cfg.locator[0] && gridCentre(cfg.locator, &cfgFix.lat, &cfgFix.lon)) {
        cfgFix.valid = true;
        snprintf(cfgFix.loc, sizeof(cfgFix.loc), "%s", cfg.locator);
    }
    choosePosition();

    CompanionHost host;
    host.cfg       = &cfg;
    host.channels  = channels;
    host.nChannels = cfg.nChannels;
    host.version   = "v" VERSION;
    host.buildDate = VERSION_DATE;
    host.writable  = []() { return Serial.availableForWrite(); };
    host.write     = [](const uint8_t *d, size_t n) { return Serial.write(d, n); };
    host.transmit  = [](const uint8_t *f, size_t n) { return sendFrame(f, (int)n); };
    // The app's advert button keeps the schedule's rule: no clock, no advert.
    host.advert    = [](bool flood) {
        return PagerClock::valid() && sendAdvert(flood ? ROUTE_TYPE_FLOOD : ROUTE_TYPE_DIRECT);
    };
    host.position  = [](double *lat, double *lon) {
        if (!haveLocation) return false;
        *lat = myLat;
        *lon = myLon;
        return true;
    };
    // Typed into the app, so taken as given rather than snapped to a grid
    // square: the person holding the pager knows where it is.
    host.setPosition = [](bool set, double lat, double lon) {
        appFix.valid = set;
        if (set) {
            appFix.lat = lat;
            appFix.lon = lon;
            gridFromLatLon(lat, lon, appFix.loc, 6);
        }
        choosePosition();
    };
    host.mcuTempC  = []() { return analogReadTemp(); };
    host.batteryMv = []() { return (uint16_t)lroundf(battFilt * 1000.0f); };
    host.radioStats = [](CompanionRadioStats *s) {
        s->noiseFloor = Radio::noiseFloor();
        s->rxAirMs    = Radio::rxAirMs();
        s->txAirMs    = Radio::txAirMs();
        s->recv       = Radio::rxCount();
        s->recvErrors = Radio::rxErrors();
        s->sent       = Radio::txCount();
        s->recvFlood  = nRecvFlood;
        s->recvDirect = nRecvDirect;
        s->sentFlood  = nSentFlood;
        s->sentDirect = nSentDirect;
    };
    host.reboot    = []() { rp2040.reboot(); };
    Companion::begin(host);

    FatFSUSB.onUnplug([](uint32_t) { rp2040.reboot(); });
    FatFSUSB.driveReady(usbDriveReady);
    FatFSUSB.begin();
    watchdog_update();

    // Starting mass storage re-enumerates the device, which drops the host's
    // serial connection for about a second. Wait for it, bounded, because on
    // a charger there is no host to wait for.
    uint32_t usbWait = millis();
    while (!Serial && (millis() - usbWait) < 2500) { delay(10); watchdog_update(); }

    char radioLine[96];
    Radio::describe(radioLine, sizeof(radioLine));
    Serial.printf("\r\nRF.Guru MeshCore Pager %s\r\n", VERSION);
    Serial.printf("call %s  name %s  node %s  route %s\r\n",
                  cfg.call, cfg.meshName, cfg.nodeType, cfg.route);
    Serial.printf("radio: %s\r\n", radioLine);
    if (Mesh::identityOk()) {
        const uint8_t *pk = Mesh::publicKey();
        Serial.printf("identity: %02X%02X%02X%02X...\r\n", pk[0], pk[1], pk[2], pk[3]);
    }
    Serial.print("channels:");
    for (int i = 0; i < cfg.nChannels; i++) Serial.printf(" %s", cfg.channels[i]);
    Serial.printf("\r\nalert: %s for %ds, pitch %d%%\r\n", cfg.alertOn, cfg.alertSeconds, cfg.tonePitch);

    if (ui.view != VIEW_FATAL) {
        ui.view = VIEW_MESSAGE;
        Alerts::play(ALERT_BOOT);
    }
    lastInteractionMs = millis();
    uiDirty = true;
}

// ---- loop ----

void loop() {
    watchdog_update();

    // Receive first and unconditionally: everything else in this loop is
    // either slow or optional, and the radio has no interrupt to fall back on.
    uint8_t  raw[MC_MAX_TRANS_UNIT];
    int16_t  rssi = 0;
    int8_t   snrQ = 0;
    int n = Radio::poll(raw, sizeof(raw), &rssi, &snrQ);
    if (n > 0) {
        if (isFloodHeader(raw[0])) nRecvFlood++;
        else nRecvDirect++;
        Companion::onRawPacket(raw, (size_t)n, rssi, snrQ);
        handlePacket(raw, n, rssi, snrQ);
    }

    serviceButton();
    serviceCompanion();
    Alerts::service();

    // The alarm view outlives the alarm by exactly one pass, whether it was
    // silenced or simply ran out of alertSeconds.
    if (ui.view == VIEW_ALERT && !Alerts::alarming()) {
        ui.view = VIEW_MESSAGE;
        uiDirty = true;
    }

    if (PagerClock::valid() && !clockWasValid) {
        clockWasValid = true;
        // Do not advertise into the same instant the advert that set our
        // clock is still being re-flooded through.
        nextAdvertMs = millis() + 5000 + (uint32_t)random(5000);
        uiDirty = true;
    }

    serviceFetchTimeout();
    serviceTimeSync();
    serviceAdvert();
    serviceBattery();
    serviceIdleLed();
    serviceScreen();
}
