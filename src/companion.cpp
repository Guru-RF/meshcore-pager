#include "companion.h"

#include <math.h>

#include "pagerclock.h"

static CompanionHost host;

// ---- little-endian helpers ----

static inline uint32_t rdU32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void wrU32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

// Upstream's StrHelper::strzcpy: a fixed-width, zero-filled field that
// always ends in at least one NUL.
static void putStr(uint8_t *dst, const char *src, size_t width) {
    memset(dst, 0, width);
    if (src) strncpy((char *)dst, src, width - 1);
}

// ---- state ----

static bool    appActive = false;
static uint8_t appVer    = 0;           // protocol level the app asked for in DEVICE_QUERY

static uint8_t out[CMP_MAX_FRAME];

// ---- output ----

// One frame, header and payload in a single write. A push - something the
// app did not ask for - goes only if the whole frame fits right now: an app
// that has stopped reading would otherwise hold the main loop, and with it
// the radio poll, for a second per frame, and half a frame would leave the
// app's parser out of step for good.
static bool writeFrame(const uint8_t *payload, size_t len, bool push) {
    if (len == 0 || len > CMP_MAX_FRAME) return false;
    if (push && host.writable() < (int)(len + 3)) return false;

    uint8_t buf[3 + CMP_MAX_FRAME];
    buf[0] = CMP_FRAME_OUT;
    buf[1] = (uint8_t)(len & 0xFF);
    buf[2] = (uint8_t)(len >> 8);
    memcpy(&buf[3], payload, len);
    return host.write(buf, len + 3) == len + 3;
}

static void writeOk() {
    uint8_t f[1] = { RESP_CODE_OK };
    writeFrame(f, sizeof(f), false);
}

static void writeErr(uint8_t code) {
    uint8_t f[2] = { RESP_CODE_ERR, code };
    writeFrame(f, sizeof(f), false);
}

// ---- contacts ----
//
// Every node heard advertising, as upstream does with auto-add on. Nothing
// is ever sent to them - there are no direct messages - so this is a list of
// who is out there, which is what fills the app's contact list and map.

struct Contact {
    uint8_t  pub[MC_PUB_KEY_SIZE];
    char     name[32];
    uint32_t advertTs;      // the node's own clock, from its signed advert
    uint32_t lastmod;       // ours: when this entry last changed
    int32_t  lat, lon;      // 1e-6 degrees; 0 when no advert carried one
    uint8_t  type;
    bool     used;
};

static Contact book[CMP_MAX_CONTACTS];

#define CONTACT_FRAME_LEN (1 + MC_PUB_KEY_SIZE + 3 + MC_MAX_PATH_SIZE + 32 + 16)   // 148

// The app syncs contacts incrementally, asking only for entries changed
// since the newest lastmod it has seen. Before the clock is set, uptime is
// the only thing that still goes up.
static uint32_t stamp() {
    uint32_t t = PagerClock::now();
    return t ? t : millis() / 1000 + 1;
}

static int findContact(const uint8_t *pub) {
    for (int i = 0; i < CMP_MAX_CONTACTS; i++)
        if (book[i].used && memcmp(book[i].pub, pub, MC_PUB_KEY_SIZE) == 0) return i;
    return -1;
}

int Companion::contacts() {
    int n = 0;
    for (int i = 0; i < CMP_MAX_CONTACTS; i++) if (book[i].used) n++;
    return n;
}

// Upstream's writeContactRespFrame().
static size_t contactFrame(uint8_t *f, uint8_t code, const Contact &c) {
    size_t i = 0;
    f[i++] = code;
    memcpy(&f[i], c.pub, MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    f[i++] = c.type;
    f[i++] = 0;                                     // flags
    f[i++] = 0xFF;                                  // out_path_len: unknown, and never learned
    memset(&f[i], 0, MC_MAX_PATH_SIZE); i += MC_MAX_PATH_SIZE;
    putStr(&f[i], c.name, 32); i += 32;
    wrU32le(&f[i], c.advertTs); i += 4;
    wrU32le(&f[i], (uint32_t)c.lat); i += 4;
    wrU32le(&f[i], (uint32_t)c.lon); i += 4;
    wrU32le(&f[i], c.lastmod); i += 4;
    return i;
}

void Companion::onAdvert(const uint8_t pub[MC_PUB_KEY_SIZE], uint8_t type, const char *name,
                         bool hasLoc, double lat, double lon, uint32_t ts) {
    // Upstream drops a nameless advert as invalid; it could not be shown in
    // a contact list anyway.
    if (!name || !name[0]) return;
    if (memcmp(pub, Mesh::publicKey(), MC_PUB_KEY_SIZE) == 0) return;

    int slot = findContact(pub);
    if (slot >= 0) {
        // Signed is not the same as fresh: a replayed advert verifies too.
        if (ts <= book[slot].advertTs) return;
    } else {
        int oldest = 0;
        for (int i = 0; i < CMP_MAX_CONTACTS; i++) {
            if (!book[i].used) { slot = i; break; }
            if (book[i].lastmod < book[oldest].lastmod) oldest = i;
        }
        if (slot < 0) slot = oldest;
        memset(&book[slot], 0, sizeof(book[slot]));
        memcpy(book[slot].pub, pub, MC_PUB_KEY_SIZE);
        book[slot].used = true;
    }

    Contact &c = book[slot];
    snprintf(c.name, sizeof(c.name), "%s", name);
    c.type = type;
    if (hasLoc) {
        c.lat = (int32_t)lround(lat * 1e6);
        c.lon = (int32_t)lround(lon * 1e6);
    }
    c.advertTs = ts;
    c.lastmod  = stamp();

    // As upstream with auto-add on: the contact is already stored, so the
    // app is only told whose advert it was, and asks for the rest.
    if (appActive) {
        uint8_t f[1 + MC_PUB_KEY_SIZE];
        f[0] = PUSH_CODE_ADVERT;
        memcpy(&f[1], pub, MC_PUB_KEY_SIZE);
        writeFrame(f, sizeof(f), true);
    }
}

// GET_CONTACTS streams one contact per service() pass, so a full list never
// sits on the radio poll.
static bool     iterOn     = false;
static int      iterIdx    = 0;
static uint32_t iterSince  = 0;
static uint32_t iterNewest = 0;

void Companion::service() {
    if (!iterOn) return;

    while (iterIdx < CMP_MAX_CONTACTS &&
           !(book[iterIdx].used && book[iterIdx].lastmod > iterSince))
        iterIdx++;

    if (iterIdx < CMP_MAX_CONTACTS) {
        if (host.writable() < 3 + CONTACT_FRAME_LEN) return;    // the app is still reading
        size_t n = contactFrame(out, RESP_CODE_CONTACT, book[iterIdx]);
        writeFrame(out, n, false);
        if (book[iterIdx].lastmod > iterNewest) iterNewest = book[iterIdx].lastmod;
        iterIdx++;
        return;
    }

    // The newest lastmod sent is the app's 'since' for next time.
    uint8_t f[5];
    f[0] = RESP_CODE_END_OF_CONTACTS;
    wrU32le(&f[1], iterNewest);
    writeFrame(f, sizeof(f), false);
    iterOn = false;
}

// ---- the message queue ----
//
// Every channel message the pager decrypts is queued here, app or no app,
// and handed over one per SYNC_NEXT_MESSAGE - so an app that attaches later
// still gets what arrived while it was away. Stored in the V3 layout and
// cut down on the way out for an app that asked for less.

struct QueuedFrame {
    uint8_t len;
    uint8_t buf[CMP_MAX_FRAME];
};

static QueuedFrame queue[CMP_QUEUE_SIZE];
static int         nQueued = 0;

int Companion::queued() { return nQueued; }

void Companion::onChannelText(int chan, const MeshPacket &p, uint32_t ts,
                              const char *sender, const char *body) {
    uint8_t f[CMP_MAX_FRAME];
    size_t i = 0;
    f[i++] = RESP_CODE_CHANNEL_MSG_RECV_V3;
    f[i++] = (uint8_t)p.snrQ;
    f[i++] = 0;                                     // reserved
    f[i++] = 0;                                     // reserved
    f[i++] = (uint8_t)chan;
    // Hops travelled for a flood; 0xFF for a routed packet, which is how a
    // fetch reply arrives.
    uint8_t rt = Mesh::routeType(p);
    f[i++] = (rt == ROUTE_TYPE_FLOOD || rt == ROUTE_TYPE_TRANSPORT_FLOOD) ? p.pathLen : 0xFF;
    f[i++] = 0;                                     // TXT_TYPE_PLAIN
    wrU32le(&f[i], ts); i += 4;

    // The app wants the plaintext as it was sent - "sender: text" - and
    // splits it itself. The pager split it to display it; put it back.
    char text[CMP_MAX_FRAME];
    if (sender && sender[0]) snprintf(text, sizeof(text), "%s: %s", sender, body);
    else                     snprintf(text, sizeof(text), "%s", body);
    size_t tlen = strlen(text);
    if (tlen > sizeof(f) - i) tlen = sizeof(f) - i;
    memcpy(&f[i], text, tlen);
    i += tlen;

    // Full: the oldest message gives way, as upstream's does.
    if (nQueued == CMP_QUEUE_SIZE) {
        memmove(&queue[0], &queue[1], (CMP_QUEUE_SIZE - 1) * sizeof(QueuedFrame));
        nQueued--;
    }
    queue[nQueued].len = (uint8_t)i;
    memcpy(queue[nQueued].buf, f, i);
    nQueued++;

    if (appActive) {
        uint8_t w[1] = { PUSH_CODE_MSG_WAITING };
        writeFrame(w, sizeof(w), true);
    }
}

static void syncNextMessage() {
    if (nQueued == 0) {
        out[0] = RESP_CODE_NO_MORE_MESSAGES;
        writeFrame(out, 1, false);
        return;
    }

    const QueuedFrame &q = queue[0];
    if (appVer >= 3) {
        writeFrame(q.buf, q.len, false);
    } else {
        // The pre-V3 layout: no SNR and no reserved bytes.
        out[0] = RESP_CODE_CHANNEL_MSG_RECV;
        memcpy(&out[1], &q.buf[4], q.len - 4);
        writeFrame(out, q.len - 3, false);
    }

    nQueued--;
    memmove(&queue[0], &queue[1], (size_t)nQueued * sizeof(QueuedFrame));
}

// ---- raw receive log ----

static int16_t lastRssi = 0;
static int8_t  lastSnrQ = 0;

void Companion::onRawPacket(const uint8_t *raw, size_t len, int16_t rssi, int8_t snrQ) {
    lastRssi = rssi;                            // for GET_STATS, app or no app
    lastSnrQ = snrQ;

    // The app's packet log. Not queued: it is only worth anything live.
    if (!appActive || len + 3 > CMP_MAX_FRAME) return;
    uint8_t f[CMP_MAX_FRAME];
    f[0] = PUSH_CODE_LOG_RX_DATA;
    f[1] = (uint8_t)snrQ;
    f[2] = (uint8_t)(int8_t)constrain(rssi, -128, 127);
    memcpy(&f[3], raw, len);
    writeFrame(f, len + 3, true);
}

// ---- sending ----

static uint8_t sendChannelText(const MeshChannel &ch, char *text, uint32_t ts) {
    // Upstream's limit on "sender: text", so nothing the pager sends is
    // longer than any other client would send. Cut on a character boundary:
    // half a UTF-8 sequence shows up as garbage on every screen downstream.
    size_t room = CMP_MAX_TEXT - (strlen(host.cfg->meshName) + 2);
    size_t n = strlen(text);
    if (n > room) {
        n = room;
        while (n > 0 && ((uint8_t)text[n] & 0xC0) == 0x80) n--;
        text[n] = '\0';
    }

    // The app stamps its messages with the phone's clock, which is the
    // better clock of the two. Only an app that sends none gets ours.
    if (!ts) ts = PagerClock::now();

    uint8_t frame[MC_MAX_TRANS_UNIT];
    int flen = Mesh::buildGroupText(frame, sizeof(frame), ch, host.cfg->meshName, text,
                                    ts, ROUTE_TYPE_FLOOD);
    if (flen < 0) return ERR_CODE_ILLEGAL_ARG;
    return host.transmit(frame, (size_t)flen) ? 0 : ERR_CODE_BAD_STATE;
}

// ---- commands ----

static uint8_t cmd[CMP_MAX_FRAME + 1];      // +1: room to terminate a trailing string

static void deviceInfo() {
    size_t i = 0;
    out[i++] = RESP_CODE_DEVICE_INFO;
    out[i++] = CMP_FIRMWARE_VER;
    out[i++] = CMP_MAX_CONTACTS / 2;
    out[i++] = PAGER_MAX_CHANNELS;
    memset(&out[i], 0, 4); i += 4;                  // BLE PIN: there is no BLE
    putStr(&out[i], host.buildDate, 12); i += 12;
    putStr(&out[i], "RF.Guru MeshCore Pager", 40); i += 40;
    putStr(&out[i], host.version, 20); i += 20;
    writeFrame(out, i, false);
}

static void selfInfo() {
    const PagerConfig &cfg = *host.cfg;
    double lat = 0, lon = 0;
    if (!host.position(&lat, &lon)) lat = lon = 0;

    size_t i = 0;
    out[i++] = RESP_CODE_SELF_INFO;
    out[i++] = Mesh::advertType(cfg);
    out[i++] = (uint8_t)cfg.txPower;
    out[i++] = 20;                                  // the SX1276's top step
    memcpy(&out[i], Mesh::publicKey(), MC_PUB_KEY_SIZE); i += MC_PUB_KEY_SIZE;
    wrU32le(&out[i], (uint32_t)(int32_t)lround(lat * 1e6)); i += 4;
    wrU32le(&out[i], (uint32_t)(int32_t)lround(lon * 1e6)); i += 4;
    out[i++] = 0;                                   // multi_acks: no direct messages, no acks
    out[i++] = 1;                                   // advert_loc_policy: share whatever position there is
    out[i++] = 0;                                   // telemetry modes
    out[i++] = 0;                                   // manual_add_contacts: every advert is a contact
    wrU32le(&out[i], (uint32_t)lround(cfg.frequency * 1000.0)); i += 4;   // kHz
    wrU32le(&out[i], (uint32_t)lround(cfg.bandwidth * 1000.0)); i += 4;   // Hz
    out[i++] = (uint8_t)cfg.sf;
    out[i++] = (uint8_t)cfg.cr;
    size_t nlen = strlen(cfg.meshName);             // no terminator, as upstream
    if (nlen > sizeof(out) - i) nlen = sizeof(out) - i;
    memcpy(&out[i], cfg.meshName, nlen); i += nlen;
    writeFrame(out, i, false);
}

static void channelInfo(uint8_t idx) {
    if (idx >= PAGER_MAX_CHANNELS) { writeErr(ERR_CODE_NOT_FOUND); return; }

    // A slot with nothing in it answers with an empty name and a zero key,
    // as upstream's does: the app reads that as free, not as missing.
    out[0] = RESP_CODE_CHANNEL_INFO;
    out[1] = idx;
    memset(&out[2], 0, 32 + 16);
    if (idx < host.nChannels && host.channels[idx].valid) {
        putStr(&out[2], host.channels[idx].name, 32);
        memcpy(&out[34], host.channels[idx].key, 16);   // the 16 significant bytes
    }
    writeFrame(out, 2 + 32 + 16, false);
}

// ---- telemetry ----
//
// Cayenne LPP, as upstream's CayenneLPP writes it: channel, type, then the
// value big-endian. Upstream answers a request for its own telemetry with
// the battery and the MCU's temperature, and so does the pager. A position
// would go here too on a board with a GPS; this one's is in SELF_INFO.

#define TELEM_CHANNEL_SELF   1
#define LPP_TEMPERATURE    103          // 0.1 degC, signed 16
#define LPP_VOLTAGE        116          // 0.01 V, unsigned 16

static size_t putBE16(uint8_t *p, int32_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return 2;
}

static void selfTelemetry() {
    size_t i = 0;
    out[i++] = PUSH_CODE_TELEMETRY_RESPONSE;
    out[i++] = 0;                                   // reserved
    memcpy(&out[i], Mesh::publicKey(), 6); i += 6;  // whose, as a key prefix

    out[i++] = TELEM_CHANNEL_SELF;
    out[i++] = LPP_VOLTAGE;
    i += putBE16(&out[i], (host.batteryMv() + 5) / 10);

    float t = host.mcuTempC();
    if (!isnan(t)) {
        out[i++] = TELEM_CHANNEL_SELF;
        out[i++] = LPP_TEMPERATURE;
        i += putBE16(&out[i], (int32_t)lroundf(t * 10.0f));
    }
    writeFrame(out, i, false);
}

// Upstream's three CMD_GET_STATS layouts.
static void stats(uint8_t type) {
    CompanionRadioStats rs;
    memset(&rs, 0, sizeof(rs));
    host.radioStats(&rs);

    size_t i = 0;
    out[i++] = RESP_CODE_STATS;
    out[i++] = type;
    if (type == STATS_TYPE_CORE) {
        uint16_t mv = host.batteryMv();
        out[i++] = (uint8_t)mv;
        out[i++] = (uint8_t)(mv >> 8);
        wrU32le(&out[i], millis() / 1000); i += 4;      // uptime
        out[i++] = 0;                                   // error flags: none are kept
        out[i++] = 0;
        out[i++] = 0;                                   // outbound queue: sends are never queued
    } else if (type == STATS_TYPE_RADIO) {
        out[i++] = (uint8_t)rs.noiseFloor;
        out[i++] = (uint8_t)((uint16_t)rs.noiseFloor >> 8);
        out[i++] = (uint8_t)(int8_t)constrain(lastRssi, -128, 127);
        out[i++] = (uint8_t)lastSnrQ;
        wrU32le(&out[i], rs.txAirMs / 1000); i += 4;
        wrU32le(&out[i], rs.rxAirMs / 1000); i += 4;
    } else if (type == STATS_TYPE_PACKETS) {
        const uint32_t v[] = {rs.recv, rs.sent, rs.sentFlood, rs.sentDirect,
                              rs.recvFlood, rs.recvDirect, rs.recvErrors};
        for (uint32_t x : v) { wrU32le(&out[i], x); i += 4; }
    } else {
        writeErr(ERR_CODE_ILLEGAL_ARG);
        return;
    }
    writeFrame(out, i, false);
}

static void handle(size_t len) {
    // Whatever was on the port before, it is a client speaking the protocol
    // now, and the debug log has to keep off it.
    appActive = true;
    cmd[len] = 0;

    switch (cmd[0]) {
    case CMD_DEVICE_QUERY:
        if (len < 2) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        appVer = cmd[1];
        deviceInfo();
        break;

    case CMD_APP_START:
        if (len < 8) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        iterOn = false;                             // a reconnecting app starts afresh
        selfInfo();
        break;

    case CMD_GET_DEVICE_TIME:
        out[0] = RESP_CODE_CURR_TIME;
        wrU32le(&out[1], PagerClock::now());
        writeFrame(out, 5, false);
        break;

    case CMD_SET_DEVICE_TIME:
        if (len >= 5 && PagerClock::setFromApp(rdU32le(&cmd[1]))) writeOk();
        else writeErr(ERR_CODE_ILLEGAL_ARG);
        break;

    case CMD_SEND_SELF_ADVERT:
        // 1 = flood; anything else, or nothing, is zero-hop.
        if (host.advert(len >= 2 && cmd[1] == 1)) writeOk();
        else writeErr(ERR_CODE_BAD_STATE);
        break;

    case CMD_SEND_CHANNEL_TXT_MSG: {
        if (len < 8) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        uint8_t txtType = cmd[1];
        uint8_t chan    = cmd[2];
        if (txtType != 0) { writeErr(ERR_CODE_UNSUPPORTED_CMD); break; }
        if (chan >= host.nChannels || !host.channels[chan].valid) { writeErr(ERR_CODE_NOT_FOUND); break; }
        uint8_t err = sendChannelText(host.channels[chan], (char *)&cmd[7], rdU32le(&cmd[3]));
        if (err) writeErr(err);
        else writeOk();
        break;
    }

    case CMD_SYNC_NEXT_MESSAGE:
        syncNextMessage();
        break;

    case CMD_GET_CHANNEL:
        if (len < 2) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        channelInfo(cmd[1]);
        break;

    case CMD_GET_CONTACTS:
        if (iterOn) { writeErr(ERR_CODE_BAD_STATE); break; }
        iterSince  = (len >= 5) ? rdU32le(&cmd[1]) : 0;
        iterIdx    = 0;
        iterNewest = 0;
        iterOn     = true;
        out[0] = RESP_CODE_CONTACTS_START;
        wrU32le(&out[1], (uint32_t)Companion::contacts());     // all of them, not just the changed
        writeFrame(out, 5, false);
        break;

    case CMD_GET_CONTACT_BY_KEY: {
        int slot = (len >= 1 + MC_PUB_KEY_SIZE) ? findContact(&cmd[1]) : -1;
        if (slot < 0) { writeErr(ERR_CODE_NOT_FOUND); break; }
        writeFrame(out, contactFrame(out, RESP_CODE_CONTACT, book[slot]), false);
        break;
    }

    case CMD_REMOVE_CONTACT: {
        int slot = (len >= 1 + MC_PUB_KEY_SIZE) ? findContact(&cmd[1]) : -1;
        if (slot < 0) { writeErr(ERR_CODE_NOT_FOUND); break; }
        book[slot].used = false;
        writeOk();
        break;
    }

    case CMD_RESET_PATH:
        // There is never a path to reset, but the contact has to exist.
        if (len >= 1 + MC_PUB_KEY_SIZE && findContact(&cmd[1]) >= 0) writeOk();
        else writeErr(ERR_CODE_NOT_FOUND);
        break;

    case CMD_SET_ADVERT_LATLON: {
        // Upstream's layout; the altitude that may follow at [9] is unused
        // there too.
        if (len < 9) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        int32_t lat = (int32_t)rdU32le(&cmd[1]);
        int32_t lon = (int32_t)rdU32le(&cmd[5]);
        if (lat > 90000000 || lat < -90000000 || lon > 180000000 || lon < -180000000) {
            writeErr(ERR_CODE_ILLEGAL_ARG);
            break;
        }
        // 0,0 is how upstream spells "no position", and what an app sends
        // when the fields are cleared: hand the position back to the pager.
        host.setPosition(lat != 0 || lon != 0, lat / 1e6, lon / 1e6);
        writeOk();
        break;
    }

    case CMD_SEND_TELEMETRY_REQ:
        // Exactly four bytes asks for the pager's own. With a key after them
        // it is a request to another node, which takes the direct messaging
        // this firmware does not do - that falls through to the refusal.
        if (len == 4) { selfTelemetry(); break; }
        writeErr(ERR_CODE_UNSUPPORTED_CMD);
        break;

    case CMD_HAS_CONNECTION:
        writeErr(ERR_CODE_NOT_FOUND);               // no logins, so never connected
        break;

    case CMD_LOGOUT:
        writeOk();
        break;

    case CMD_GET_BATT_AND_STORAGE: {
        uint16_t mv = host.batteryMv();
        out[0] = RESP_CODE_BATT_AND_STORAGE;
        out[1] = (uint8_t)mv;
        out[2] = (uint8_t)(mv >> 8);
        wrU32le(&out[3], 0);                        // storage used, KB: nothing is stored
        wrU32le(&out[7], 0);                        // storage total, KB
        writeFrame(out, 11, false);
        break;
    }

    case CMD_GET_CUSTOM_VARS:
        out[0] = RESP_CODE_CUSTOM_VARS;             // none
        writeFrame(out, 1, false);
        break;

    case CMD_GET_STATS:
        if (len < 2) { writeErr(ERR_CODE_ILLEGAL_ARG); break; }
        stats(cmd[1]);
        break;

    case CMD_GET_AUTOADD_CONFIG:
        // What the pager does, in upstream's bits: every advert heard becomes
        // a contact - chat, repeater, room and sensor alike - from any
        // distance, and the oldest gives way when the list is full.
        out[0] = RESP_CODE_AUTOADD_CONFIG;
        out[1] = 0x1F;                              // overwrite oldest | all four types
        out[2] = 0;                                 // max hops: no limit
        writeFrame(out, 3, false);
        break;

    case CMD_EXPORT_PRIVATE_KEY:
        // /meshid.bin is on the drive for anyone with the device in hand;
        // there is no reason to also hand it to whatever is on the port.
        out[0] = RESP_CODE_DISABLED;
        writeFrame(out, 1, false);
        break;

    case CMD_REBOOT:
        if (len >= 7 && memcmp(&cmd[1], "reboot", 6) == 0) host.reboot();
        else writeErr(ERR_CODE_ILLEGAL_ARG);
        break;

    default:
        // Direct messages, settings and everything else this pager does not
        // do. See the header for why each is refused rather than faked.
        writeErr(ERR_CODE_UNSUPPORTED_CMD);
        break;
    }
}

// ---- input ----

enum RxState { RX_IDLE, RX_LEN_LO, RX_LEN_HI, RX_BODY };

static RxState  rxState  = RX_IDLE;
static uint16_t rxLen    = 0;
static uint16_t rxGot    = 0;
static uint32_t rxLastMs = 0;

// A frame that stops arriving half way - the app killed, the cable pulled -
// is abandoned after this long, rather than swallowing the start of the
// next one.
static const uint32_t RX_FRAME_TIMEOUT_MS = 500;

void Companion::feed(uint8_t c) {
    uint32_t now = millis();
    if (rxState != RX_IDLE && (now - rxLastMs) > RX_FRAME_TIMEOUT_MS) rxState = RX_IDLE;
    rxLastMs = now;

    switch (rxState) {
    case RX_IDLE:
        // Anything between frames - keystrokes from a terminal - is ignored.
        if (c == CMP_FRAME_IN) rxState = RX_LEN_LO;
        break;
    case RX_LEN_LO:
        rxLen   = c;
        rxState = RX_LEN_HI;
        break;
    case RX_LEN_HI:
        rxLen |= (uint16_t)(c << 8);
        rxGot  = 0;
        // Upstream truncates an oversize frame. On a port someone may also
        // be typing into, a length no app would send means a stray '<', not
        // a frame, and waiting for it would swallow the next real one.
        rxState = (rxLen > 0 && rxLen <= CMP_MAX_FRAME) ? RX_BODY : RX_IDLE;
        break;
    case RX_BODY:
        cmd[rxGot++] = c;
        if (rxGot == rxLen) {
            rxState = RX_IDLE;
            handle(rxLen);
        }
        break;
    }
}

bool Companion::active() { return appActive; }

void Companion::portClosed() {
    appActive = false;
    appVer    = 0;
    iterOn    = false;
    rxState   = RX_IDLE;
}

void Companion::begin(const CompanionHost &h) {
    host = h;
    memset(book, 0, sizeof(book));
    nQueued = 0;
    Companion::portClosed();
}
