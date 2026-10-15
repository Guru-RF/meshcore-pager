// ============================================================
// RF.Guru MeshCore Pager - config.txt on the USB drive
//
// The pager enumerates as a small FAT drive. config.txt on it is the whole
// user interface for setup: edit it, EJECT the drive (an unmount is not
// enough - the sector never reaches flash), and the pager reboots with the
// new settings. Keys absent from the file keep their firmware default, so an
// old file on a deployed pager still boots.
//
// The Ed25519 identity lives beside it in /meshid.bin and is NOT
// regenerable: it is this pager's address on the mesh. Copy it off the
// drive before you do anything that reformats the filesystem.
// ============================================================

#pragma once

#include <Arduino.h>
#include <FatFS.h>

// ---- defaults ----
#define DEFAULT_CALL            "NOCALL"
#define DEFAULT_NODE_TYPE       "chat"

// A pager is a leaf and never forwards, so its advert has to reach the mesh
// on its own legs: FLOOD, unlike the tracker, whose position is gated to
// APRS-IS by the first repeater that hears it and needs no further travel.
#define DEFAULT_ROUTE           "flood"

#define DEFAULT_CHANNELS        "#mbox,#mesh"
#define DEFAULT_MBOX_CHANNEL    "#mbox"
#define DEFAULT_TIME_CHANNEL    "#mesh"

// "mention" alarms only for traffic addressed to this pager and gives
// everything else a short chirp. "all" alarms for every message on every
// subscribed channel, which on a busy #mesh is a lot of alarm.
#define DEFAULT_ALERT_ON        "mention"
#define DEFAULT_ALERT_SECONDS   30
// Scales every pattern frequency, in percent. The frequencies themselves are
// taken from LoRa433APRSpager, which drove this transducer from 330 Hz to
// 1165 Hz on real hardware, so 100 means "as proven".
#define DEFAULT_TONE_PITCH      100
#define DEFAULT_BUZZER          true
#define DEFAULT_LED_BRIGHTNESS  200

#define DEFAULT_FETCH_COOLDOWN  600     // the 10 minutes that switch what a hold does
#define DEFAULT_ADVERT_INTERVAL 3600
#define DEFAULT_TIMESYNC_INTERVAL 21600
#define DEFAULT_SCREEN_TIMEOUT  30
#define DEFAULT_GRID_PRECISION  6

// IARU R1 ham MeshCore channel - github.com/Guru-RF/meshcore-rfc-iaru-r1
#define DEFAULT_FREQ            434.890f
#define DEFAULT_BW              62.5f
#define DEFAULT_SF              8
#define DEFAULT_CR              8
#define DEFAULT_PREAMBLE        16
#define DEFAULT_TX_POWER        17

// Each subscribed channel costs one selector-byte compare per packet heard,
// so the limit is about the config line and the app's channel list, not
// about load.
#define PAGER_MAX_CHANNELS      8
#define PAGER_CHAN_LEN          20

struct PagerConfig {
    char  call[16];
    char  meshName[24];
    char  nodeType[12];
    char  route[8];

    char  channels[PAGER_MAX_CHANNELS][PAGER_CHAN_LEN];
    int   nChannels;
    char  mboxChannel[PAGER_CHAN_LEN];
    char  timeChannel[PAGER_CHAN_LEN];

    char  alertOn[12];
    int   alertSeconds;
    int   tonePitch;
    bool  buzzer;
    int   ledBrightness;

    int   fetchCooldown;
    int   advertInterval;
    int   timeSyncInterval;
    int   screenTimeout;
    int   gridPrecision;
    char  locator[8];               // manual override, e.g. JO11UF; empty = learn it

    float frequency;
    float bandwidth;
    int   sf;
    int   cr;
    int   preamble;
    int   txPower;

    bool  debug;
};

static const char *CONFIG_PATH = "/config.txt";

static void configSplitChannels(PagerConfig &cfg, const char *csv) {
    cfg.nChannels = 0;
    const char *p = csv;
    while (*p && cfg.nChannels < PAGER_MAX_CHANNELS) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        size_t n = 0;
        char  *dst = cfg.channels[cfg.nChannels];
        while (*p && *p != ',' && n + 1 < PAGER_CHAN_LEN) dst[n++] = *p++;
        while (n > 0 && dst[n - 1] == ' ') n--;      // trim "#mbox , #mesh"
        dst[n] = '\0';
        while (*p && *p != ',') p++;
        if (n) cfg.nChannels++;
    }
}

static void configSetDefaults(PagerConfig &cfg) {
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.call, DEFAULT_CALL, sizeof(cfg.call) - 1);
    cfg.meshName[0] = '\0';                          // resolved from call below
    strncpy(cfg.nodeType, DEFAULT_NODE_TYPE, sizeof(cfg.nodeType) - 1);
    strncpy(cfg.route, DEFAULT_ROUTE, sizeof(cfg.route) - 1);

    configSplitChannels(cfg, DEFAULT_CHANNELS);
    strncpy(cfg.mboxChannel, DEFAULT_MBOX_CHANNEL, sizeof(cfg.mboxChannel) - 1);
    strncpy(cfg.timeChannel, DEFAULT_TIME_CHANNEL, sizeof(cfg.timeChannel) - 1);

    strncpy(cfg.alertOn, DEFAULT_ALERT_ON, sizeof(cfg.alertOn) - 1);
    cfg.alertSeconds     = DEFAULT_ALERT_SECONDS;
    cfg.tonePitch        = DEFAULT_TONE_PITCH;
    cfg.buzzer           = DEFAULT_BUZZER;
    cfg.ledBrightness    = DEFAULT_LED_BRIGHTNESS;

    cfg.fetchCooldown    = DEFAULT_FETCH_COOLDOWN;
    cfg.advertInterval   = DEFAULT_ADVERT_INTERVAL;
    cfg.timeSyncInterval = DEFAULT_TIMESYNC_INTERVAL;
    cfg.screenTimeout    = DEFAULT_SCREEN_TIMEOUT;
    cfg.gridPrecision    = DEFAULT_GRID_PRECISION;
    cfg.locator[0]       = '\0';

    cfg.frequency = DEFAULT_FREQ;
    cfg.bandwidth = DEFAULT_BW;
    cfg.sf        = DEFAULT_SF;
    cfg.cr        = DEFAULT_CR;
    cfg.preamble  = DEFAULT_PREAMBLE;
    cfg.txPower   = DEFAULT_TX_POWER;
    cfg.debug     = false;
}

static bool configTruthy(const char *v) {
    return strcasecmp(v, "true") == 0 || strcmp(v, "1") == 0 ||
           strcasecmp(v, "yes") == 0 || strcasecmp(v, "on") == 0;
}

static void configApplyKey(PagerConfig &cfg, const char *key, const char *val) {
    if      (!strcasecmp(key, "call"))         strncpy(cfg.call, val, sizeof(cfg.call) - 1);
    else if (!strcasecmp(key, "meshName"))     strncpy(cfg.meshName, val, sizeof(cfg.meshName) - 1);
    else if (!strcasecmp(key, "nodeType"))     strncpy(cfg.nodeType, val, sizeof(cfg.nodeType) - 1);
    else if (!strcasecmp(key, "route"))        strncpy(cfg.route, val, sizeof(cfg.route) - 1);
    else if (!strcasecmp(key, "channels"))     configSplitChannels(cfg, val);
    else if (!strcasecmp(key, "mboxChannel"))  strncpy(cfg.mboxChannel, val, sizeof(cfg.mboxChannel) - 1);
    else if (!strcasecmp(key, "timeChannel"))  strncpy(cfg.timeChannel, val, sizeof(cfg.timeChannel) - 1);
    else if (!strcasecmp(key, "alertOn"))      strncpy(cfg.alertOn, val, sizeof(cfg.alertOn) - 1);
    else if (!strcasecmp(key, "alertSeconds")) cfg.alertSeconds = constrain(atoi(val), 0, 600);
    else if (!strcasecmp(key, "tonePitch"))    cfg.tonePitch = constrain(atoi(val), 50, 200);
    else if (!strcasecmp(key, "buzzer"))       cfg.buzzer = configTruthy(val);
    else if (!strcasecmp(key, "ledBrightness")) cfg.ledBrightness = constrain(atoi(val), 0, 255);
    else if (!strcasecmp(key, "fetchCooldown")) cfg.fetchCooldown = constrain(atoi(val), 60, 86400);
    else if (!strcasecmp(key, "advertInterval")) cfg.advertInterval = constrain(atoi(val), 300, 86400);
    else if (!strcasecmp(key, "timeSyncInterval")) cfg.timeSyncInterval = constrain(atoi(val), 600, 604800);
    else if (!strcasecmp(key, "screenTimeout")) cfg.screenTimeout = constrain(atoi(val), 0, 600);
    else if (!strcasecmp(key, "gridPrecision")) cfg.gridPrecision = (atoi(val) >= 6) ? 6 : 4;
    else if (!strcasecmp(key, "locator"))      strncpy(cfg.locator, val, sizeof(cfg.locator) - 1);
    else if (!strcasecmp(key, "frequency"))    cfg.frequency = constrain((float)atof(val), 430.0f, 434.960f);
    else if (!strcasecmp(key, "bandwidth")) {
        // Only the bandwidths the SX1276 can actually be programmed to; an
        // unrecognised number keeps the default rather than failing to tune.
        static const float ok[] = {7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f,
                                   62.5f, 125.0f, 250.0f, 500.0f};
        float bw = (float)atof(val);
        for (unsigned i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
            if (fabsf(bw - ok[i]) < 0.01f) { cfg.bandwidth = bw; break; }
    }
    else if (!strcasecmp(key, "sf"))           cfg.sf = constrain(atoi(val), 6, 12);
    else if (!strcasecmp(key, "cr"))           cfg.cr = constrain(atoi(val), 5, 8);
    else if (!strcasecmp(key, "preamble"))     cfg.preamble = constrain(atoi(val), 6, 64);
    else if (!strcasecmp(key, "txPower"))      cfg.txPower = constrain(atoi(val), 2, 20);
    else if (!strcasecmp(key, "debug"))        cfg.debug = configTruthy(val);
}

static bool configCreateDefault() {
    File f = FatFS.open(CONFIG_PATH, "w");
    if (!f) return false;
    f.println("# RF.Guru MeshCore Pager");
    f.println("#");
    f.println("# Edit, then EJECT this drive - the pager reboots and applies it.");
    f.println("# Unmounting without ejecting loses the write.");
    f.println("");
    f.println("# Your call. Messages that mention it on a subscribed channel");
    f.println("# ('@OR7F hello') set off the alarm; the SSID is ignored when");
    f.println("# matching, so @OR7F reaches OR7F-12.");
    f.println("call=NOCALL");
    f.println("# Advertised name and the prefix on anything this pager sends.");
    f.println("# Empty = same as call.");
    f.println("meshName=");
    f.println("nodeType=chat");
    f.println("# flood: the advert travels the mesh. direct: neighbours only.");
    f.println("route=flood");
    f.println("");
    f.println("# Public channels to listen on. The key is derived from the name");
    f.println("# (SHA256 of it), so no key exchange - anyone with the name reads it.");
    f.println("# Public, without a #, is MeshCore's built-in channel: its key is");
    f.println("# published rather than derived.");
    f.println("channels=#mbox,#mesh");
    f.println("# Where 'fetch' is sent and mail comes back.");
    f.println("mboxChannel=#mbox");
    f.println("# Where the repeaters answer 'time' and 'date'. The pager has no");
    f.println("# RTC and no GPS, so this is the only clock it has.");
    f.println("timeChannel=#mesh");
    f.println("");
    f.println("# mention = alarm only for traffic addressed to you (chirp for the");
    f.println("# rest). all = alarm for every message on every channel above.");
    f.println("alertOn=mention");
    f.println("# How long the alarm repeats if nobody presses the button. 0 = one shot.");
    f.println("alertSeconds=30");
    f.println("# Shifts every tone, in percent. The pitches are the ones the");
    f.println("# previous firmware for this board proved audible; 100 keeps them.");
    f.println("tonePitch=100");
    f.println("buzzer=true");
    f.println("ledBrightness=200");
    f.println("");
    f.println("# A hold fetches mail; within this many seconds of the last fetch a");
    f.println("# hold deletes the message on screen instead.");
    f.println("fetchCooldown=600");
    f.println("advertInterval=3600");
    f.println("timeSyncInterval=21600");
    f.println("# Seconds of no button before the display sleeps. 0 = never.");
    f.println("screenTimeout=30");
    f.println("");
    f.println("# The pager has no GPS. It advertises the CENTRE of the Maidenhead");
    f.println("# square of the strongest repeater it has heard, which is honest");
    f.println("# about the precision it actually has. Set locator to pin it down");
    f.println("# yourself and stop it learning.");
    f.println("locator=");
    f.println("gridPrecision=6");
    f.println("");
    f.println("# IARU R1 ham MeshCore channel.");
    f.println("frequency=434.890");
    f.println("bandwidth=62.5");
    f.println("sf=8");
    f.println("cr=8");
    f.println("preamble=16");
    f.println("txPower=17");
    f.println("");
    f.println("debug=false");
    f.close();
    return true;
}

// Resolve the keys that default to other keys. Called after parsing, and
// after configSetDefaults() when there is no file at all.
static void configResolve(PagerConfig &cfg) {
    if (!cfg.meshName[0])
        strncpy(cfg.meshName, cfg.call, sizeof(cfg.meshName) - 1);
    if (cfg.nChannels == 0)
        configSplitChannels(cfg, DEFAULT_CHANNELS);
    // An uppercase locator is what every other tool prints, and the parser
    // in grid.h expects it.
    for (char *p = cfg.locator; *p; p++) *p = toupper(*p);
}

static bool configLoad(PagerConfig &cfg) {
    configSetDefaults(cfg);

    File f = FatFS.open(CONFIG_PATH, "r");
    if (!f) {
        configCreateDefault();
        configResolve(cfg);
        return false;                     // ran on defaults; caller says so on screen
    }

    char line[160];
    while (f.available()) {
        size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == '\0' || *s == '\r') continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        for (char *e = key + strlen(key); e > key && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';
        while (*val == ' ' || *val == '\t') val++;
        for (char *e = val + strlen(val); e > val &&
             (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = '\0';

        configApplyKey(cfg, key, val);
    }
    f.close();
    configResolve(cfg);
    return true;
}
