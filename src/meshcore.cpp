#include "meshcore.h"

#include <FatFS.h>
#include <hardware/watchdog.h>

extern "C" {
#include "ed_25519.h"
#include "aes128.h"
#include "sha256.h"
#include "hmac_sha256.h"
}

// ---- identity ----

static const char    *IDENTITY_PATH  = "/meshid.bin";
static const uint32_t IDENTITY_MAGIC = 0x4D435F31u;   // "MC_1", same as the tracker

struct IdentityFile {
    uint32_t magic;
    uint8_t  pub[MC_PUB_KEY_SIZE];
    uint8_t  prv[MC_PRV_KEY_SIZE];
};

static IdentityFile identity;
static bool         identityLoaded = false;

// Seed from the RP2040's ring oscillator, never from millis(). A predictable
// seed is a forgeable identity: the private key is the only thing stopping
// anyone advertising as this pager.
static void seedFromHardware(uint8_t seed[32]) {
    for (int i = 0; i < 8; i++) {
        uint32_t r = rp2040.hwrand32();
        memcpy(seed + i * 4, &r, 4);
    }
}

bool Mesh::begin(char *err, size_t errLen) {
    File f = FatFS.open(IDENTITY_PATH, "r");
    if (f) {
        size_t n = f.read((uint8_t *)&identity, sizeof(identity));
        f.close();
        if (n == sizeof(identity) && identity.magic == IDENTITY_MAGIC) {
            identityLoaded = true;
            return true;
        }
        // Present but unusable. Refuse rather than silently minting a new
        // identity over it - the owner may have restored a backup badly, and
        // a node that changes address on its own is worse than one that says
        // why it will not start.
        snprintf(err, errLen, "%s corrupt - delete it to mint a new identity", IDENTITY_PATH);
        return false;
    }

    uint8_t seed[32];
    seedFromHardware(seed);
    watchdog_update();
    ed25519_create_keypair(identity.pub, identity.prv, seed);
    watchdog_update();
    identity.magic = IDENTITY_MAGIC;

    f = FatFS.open(IDENTITY_PATH, "w");
    if (!f) {
        snprintf(err, errLen, "could not create %s", IDENTITY_PATH);
        return false;
    }
    size_t n = f.write((const uint8_t *)&identity, sizeof(identity));
    f.close();
    if (n != sizeof(identity)) {
        // Remove the stub rather than leaving it to be read back as a corrupt
        // file on every later boot, which would report an owner error for a
        // file this code wrote five lines ago and knows is bad.
        FatFS.remove(IDENTITY_PATH);
        snprintf(err, errLen, "short write creating %s - removed", IDENTITY_PATH);
        return false;
    }

    identityLoaded = true;
    return true;
}

bool Mesh::identityOk() { return identityLoaded; }
const uint8_t *Mesh::publicKey() { return identity.pub; }

// ---- little-endian helpers ----

static inline uint32_t rdU32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline int32_t rdI32le(const uint8_t *p) { return (int32_t)rdU32le(p); }
static inline void wrU32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void wrI32le(uint8_t *p, int32_t v) { wrU32le(p, (uint32_t)v); }

// ---- channels ----

// MeshCore's PUBLIC_GROUP_PSK, "izOH6cXN6mrJ5e26oRXNcg==" in base64.
static const uint8_t PUBLIC_CHANNEL_KEY[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72,
};

void Mesh::channelInit(MeshChannel &c, const char *name) {
    memset(&c, 0, sizeof(c));
    if (!name || !name[0]) return;

    SHA256_CTX ctx;
    uint8_t digest[SHA256_BLOCK_SIZE];

    if (!strcasecmp(name, MC_PUBLIC_CHANNEL)) {
        // Spelled the way upstream and the apps spell it, whatever case
        // config.txt used.
        snprintf(c.name, sizeof(c.name), "%s", MC_PUBLIC_CHANNEL);
        memcpy(c.key, PUBLIC_CHANNEL_KEY, 16);
    } else {
        snprintf(c.name, sizeof(c.name), "%s", name);
        sha256_init(&ctx);
        sha256_update(&ctx, (const uint8_t *)c.name, strlen(c.name));
        sha256_final(&ctx, digest);
        memcpy(c.key, digest, 16);              // channel key = SHA256(name)[0..15]
    }

    // The selector hashes the raw 16 bytes, not the 32-byte padded form the
    // HMAC uses. That asymmetry is real and easy to get wrong.
    sha256_init(&ctx);
    sha256_update(&ctx, c.key, 16);
    sha256_final(&ctx, digest);
    c.selector = digest[0];
    c.valid = true;
}

// ---- packet parse ----

static bool validPathLen(uint8_t pathLen) {
    uint8_t count = pathLen & 0x3F;
    uint8_t size  = (uint8_t)((pathLen >> 6) + 1);
    if (size == 4) return false;                // top bits 0b11 are reserved
    return (count * size) <= MC_MAX_PATH_SIZE;
}

bool Mesh::parse(MeshPacket &p, const uint8_t *raw, size_t len) {
    if (len < 2 || len > MC_MAX_TRANS_UNIT) return false;

    memset(&p, 0, sizeof(p));
    size_t i = 0;
    p.header = raw[i++];

    uint8_t rt = p.header & PH_ROUTE_MASK;
    if (rt == ROUTE_TYPE_TRANSPORT_FLOOD || rt == ROUTE_TYPE_TRANSPORT_DIRECT) {
        if (i + 4 > len) return false;
        p.transportCodes[0] = (uint16_t)(raw[i] | (raw[i + 1] << 8)); i += 2;
        p.transportCodes[1] = (uint16_t)(raw[i] | (raw[i + 1] << 8)); i += 2;
    }

    if (i >= len) return false;
    p.pathLen = raw[i++];
    if (!validPathLen(p.pathLen)) return false;

    uint16_t pathBytes = (uint16_t)(Mesh::pathHashCount(p) * Mesh::pathHashSize(p));
    if (i + pathBytes > len) return false;
    memcpy(p.path, &raw[i], pathBytes);
    i += pathBytes;

    size_t payloadLen = len - i;
    if (payloadLen > MC_MAX_PACKET_PAYLOAD) return false;
    p.payloadLen = (uint16_t)payloadLen;
    memcpy(p.payload, &raw[i], payloadLen);
    return true;
}

// ---- dedup ----

#define SEEN_SIZE 128

static uint8_t seen[SEEN_SIZE][MC_HASH_SIZE];
static int     seenIdx   = 0;
static int     seenCount = 0;

void Mesh::calcHash(const MeshPacket &p, uint8_t out[MC_HASH_SIZE]) {
    SHA256_CTX ctx;
    uint8_t digest[SHA256_BLOCK_SIZE];
    uint8_t t = Mesh::payloadType(p);

    sha256_init(&ctx);
    sha256_update(&ctx, &t, 1);
    if (t == PAYLOAD_TYPE_TRACE) {
        // MeshCore folds the (uint16) path_len into a TRACE's hash so it may
        // revisit nodes on the return path.
        uint8_t pl[2] = { p.pathLen, 0 };
        sha256_update(&ctx, pl, 2);
    }
    sha256_update(&ctx, p.payload, p.payloadLen);
    sha256_final(&ctx, digest);
    memcpy(out, digest, MC_HASH_SIZE);
}

static bool seenCheckAndAdd(const uint8_t *hash) {
    for (int i = 0; i < seenCount; i++)
        if (memcmp(seen[i], hash, MC_HASH_SIZE) == 0) return true;
    memcpy(seen[seenIdx], hash, MC_HASH_SIZE);
    seenIdx = (seenIdx + 1) % SEEN_SIZE;
    if (seenCount < SEEN_SIZE) seenCount++;
    return false;
}

bool Mesh::seenBefore(const MeshPacket &p) {
    uint8_t hash[MC_HASH_SIZE];
    Mesh::calcHash(p, hash);
    return seenCheckAndAdd(hash);
}

void Mesh::rememberFrame(const uint8_t *frame, size_t len) {
    MeshPacket p;
    if (!Mesh::parse(p, frame, len)) return;
    uint8_t hash[MC_HASH_SIZE];
    Mesh::calcHash(p, hash);
    seenCheckAndAdd(hash);
}

// ---- group text ----
//
//   key      = SHA256("#name")[0..15]
//   selector = SHA256(key, 16)[0]
//   plain    = [ts u32 LE][txt_type 0]["sender: text"], zero-padded to 16
//   cipher   = AES-128-ECB(key, plain)
//   payload  = [selector][HMAC-SHA256(key32, CIPHER)[0..1]][cipher]
//
// It is encrypt-then-MAC. The repeater's channel_build_txt() encrypts in
// place and only then calls hmac_sha256() on the same buffer, which reads as
// though the MAC covers the plaintext; it covers the ciphertext. Getting it
// backwards yields a packet the repeater receives, matches to the right
// channel, and drops with "failed public-channel MAC".

bool Mesh::decryptGroupText(const MeshChannel &c, const MeshPacket &p,
                            uint32_t *ts, char *sender, size_t senderSz,
                            char *body, size_t bodySz) {
    if (sender && senderSz) sender[0] = '\0';
    if (body && bodySz) body[0] = '\0';
    if (!c.valid || p.payloadLen < 3 + 16) return false;
    if (p.payload[0] != c.selector) return false;

    size_t ctlen = (size_t)p.payloadLen - 3;
    if (ctlen % 16 != 0) return false;

    uint8_t mac[32];
    hmac_sha256(c.key, sizeof(c.key), &p.payload[3], ctlen, mac);
    if (mac[0] != p.payload[1] || mac[1] != p.payload[2]) return false;

    uint8_t plain[MC_MAX_PACKET_PAYLOAD + 1];
    memcpy(plain, &p.payload[3], ctlen);
    aes128_ctx_t ac;
    aes128_init(&ac, c.key);                    // AES takes the 16 significant bytes
    aes128_ecb_decrypt(&ac, plain, ctlen);
    plain[ctlen] = '\0';                        // terminate at the zero padding

    if (ctlen < 5) return false;
    if (ts) *ts = rdU32le(plain);

    // [ts:4][txt_type:1]["sender: message"]
    const char *text = (const char *)&plain[5];
    const char *sep  = strstr(text, ": ");
    if (sep) {
        size_t n = (size_t)(sep - text);
        if (sender && senderSz) {
            if (n > senderSz - 1) n = senderSz - 1;
            memcpy(sender, text, n);
            sender[n] = '\0';
        }
        if (body && bodySz) snprintf(body, bodySz, "%s", sep + 2);
    } else {
        // No separator: the whole thing is the message and nobody signed it.
        if (body && bodySz) snprintf(body, bodySz, "%s", text);
    }
    return true;
}

// ---- adverts ----

#define ADV_PUB_OFF   0
#define ADV_TS_OFF    (MC_PUB_KEY_SIZE)                             // 32
#define ADV_SIG_OFF   (MC_PUB_KEY_SIZE + 4)                         // 36
#define ADV_DATA_OFF  (MC_PUB_KEY_SIZE + 4 + MC_SIGNATURE_SIZE)     // 100

bool Mesh::advertVerify(const MeshPacket &p, uint8_t outPub[MC_PUB_KEY_SIZE]) {
    if (Mesh::payloadType(p) != PAYLOAD_TYPE_ADVERT) return false;
    if (p.payloadLen < ADV_DATA_OFF) return false;

    const uint8_t *pub  = &p.payload[ADV_PUB_OFF];
    uint32_t       tstamp = rdU32le(&p.payload[ADV_TS_OFF]);
    const uint8_t *sig  = &p.payload[ADV_SIG_OFF];
    const uint8_t *app  = &p.payload[ADV_DATA_OFF];
    size_t appLen = p.payloadLen - ADV_DATA_OFF;
    if (appLen > MC_MAX_ADVERT_DATA) appLen = MC_MAX_ADVERT_DATA;   // ignore an overflow tail

    uint8_t msg[MC_PUB_KEY_SIZE + 4 + MC_MAX_ADVERT_DATA];
    size_t mlen = 0;
    memcpy(&msg[mlen], pub, MC_PUB_KEY_SIZE); mlen += MC_PUB_KEY_SIZE;
    wrU32le(&msg[mlen], tstamp);              mlen += 4;
    memcpy(&msg[mlen], app, appLen);          mlen += appLen;

    // Ed25519 verification is the most expensive thing this firmware does -
    // roughly a tenth of a second - so the watchdog is fed either side of it.
    watchdog_update();
    bool okSig = ed25519_verify(sig, msg, mlen, pub) != 0;
    watchdog_update();
    if (!okSig) return false;

    if (outPub) memcpy(outPub, pub, MC_PUB_KEY_SIZE);
    return true;
}

bool Mesh::advertInfo(const MeshPacket &p, uint8_t *type, char *name, size_t nameSz,
                      bool *hasLoc, double *lat, double *lon, uint32_t *ts) {
    if (name && nameSz) name[0] = '\0';
    if (hasLoc) *hasLoc = false;
    if (p.payloadLen < ADV_DATA_OFF) return false;
    if (ts) *ts = rdU32le(&p.payload[ADV_TS_OFF]);

    const uint8_t *app = &p.payload[ADV_DATA_OFF];
    size_t appLen = p.payloadLen - ADV_DATA_OFF;
    if (appLen == 0) return false;

    uint8_t flags = app[0];
    if (type) *type = (uint8_t)(flags & 0x0F);

    size_t i = 1;
    if (flags & ADV_LATLON_MASK) {
        if (appLen >= 9) {
            if (hasLoc) *hasLoc = true;
            if (lat) *lat = (double)rdI32le(&app[1]) / 1e6;
            if (lon) *lon = (double)rdI32le(&app[5]) / 1e6;
        }
        i += 8;
    }
    if (flags & ADV_FEAT1_MASK) i += 2;
    if (flags & ADV_FEAT2_MASK) i += 2;

    if ((flags & ADV_NAME_MASK) && name && nameSz && i < appLen) {
        size_t n = appLen - i;
        if (n > nameSz - 1) n = nameSz - 1;
        memcpy(name, &app[i], n);
        name[n] = '\0';
    }
    return true;
}

uint8_t Mesh::advertType(const PagerConfig &cfg) {
    if (!strcasecmp(cfg.nodeType, "repeater")) return ADV_TYPE_REPEATER;
    if (!strcasecmp(cfg.nodeType, "room"))     return ADV_TYPE_ROOM;
    if (!strcasecmp(cfg.nodeType, "sensor"))   return ADV_TYPE_SENSOR;
    return ADV_TYPE_CHAT;
}

int Mesh::buildAdvert(uint8_t *frame, size_t frameSz, const PagerConfig &cfg,
                      bool hasLoc, double lat, double lon, uint32_t unixTime) {
    if (!identityLoaded) return -1;
    if (frameSz < 2 + ADV_DATA_OFF + MC_MAX_ADVERT_DATA) return -1;

    uint8_t appData[MC_MAX_ADVERT_DATA];
    size_t  appLen = 1;
    uint8_t flags  = Mesh::advertType(cfg);

    if (hasLoc) {
        flags |= ADV_LATLON_MASK;
        wrI32le(&appData[appLen], (int32_t)lround(lat * 1e6)); appLen += 4;
        wrI32le(&appData[appLen], (int32_t)lround(lon * 1e6)); appLen += 4;
    }

    size_t nameLen = strlen(cfg.meshName);
    size_t room    = MC_MAX_ADVERT_DATA - appLen;
    if (nameLen > room) nameLen = room;
    if (nameLen) {
        flags |= ADV_NAME_MASK;
        memcpy(&appData[appLen], cfg.meshName, nameLen);
        appLen += nameLen;
    }
    appData[0] = flags;

    uint8_t msg[MC_PUB_KEY_SIZE + 4 + MC_MAX_ADVERT_DATA];
    size_t mlen = 0;
    memcpy(&msg[mlen], identity.pub, MC_PUB_KEY_SIZE); mlen += MC_PUB_KEY_SIZE;
    wrU32le(&msg[mlen], unixTime);                     mlen += 4;
    memcpy(&msg[mlen], appData, appLen);               mlen += appLen;

    uint8_t sig[MC_SIGNATURE_SIZE];
    watchdog_update();
    ed25519_sign(sig, msg, mlen, identity.pub, identity.prv);
    watchdog_update();

    uint8_t route = (!strcasecmp(cfg.route, "direct")) ? ROUTE_TYPE_DIRECT : ROUTE_TYPE_FLOOD;

    // path_len 0: no hops recorded, hash size 1. On FLOOD that is the normal
    // starting state and each hop appends its own hash.
    frame[0] = (uint8_t)(route | (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT));
    frame[1] = 0x00;
    memcpy(&frame[2 + ADV_PUB_OFF], identity.pub, MC_PUB_KEY_SIZE);
    wrU32le(&frame[2 + ADV_TS_OFF], unixTime);
    memcpy(&frame[2 + ADV_SIG_OFF], sig, MC_SIGNATURE_SIZE);
    memcpy(&frame[2 + ADV_DATA_OFF], appData, appLen);
    return (int)(2 + ADV_DATA_OFF + appLen);
}

int Mesh::buildGroupText(uint8_t *frame, size_t frameSz, const MeshChannel &c,
                         const char *sender, const char *text, uint32_t unixTime,
                         uint8_t route) {
    if (!c.valid || !text || !text[0]) return -1;

    uint8_t plain[MC_MAX_PACKET_PAYLOAD];
    size_t i = 0;
    wrU32le(plain, unixTime); i = 4;
    plain[i++] = 0;                              // txt_type = plain

    int n = snprintf((char *)&plain[i], sizeof(plain) - i, "%s: %s",
                     sender ? sender : "", text);
    if (n < 0) return -1;
    if ((size_t)n >= sizeof(plain) - i) return -1;   // would have been truncated
    i += (size_t)n;                              // strlen, no NUL - as MeshCore does

    size_t ctlen = (i + 15) & ~(size_t)15;       // zero-pad to a whole AES block
    if (ctlen == 0) ctlen = 16;
    if (3 + ctlen > MC_MAX_PACKET_PAYLOAD) return -1;
    if (2 + 3 + ctlen > frameSz) return -1;
    while (i < ctlen) plain[i++] = 0;

    aes128_ctx_t ac;
    aes128_init(&ac, c.key);
    aes128_ecb_encrypt(&ac, plain, ctlen);

    uint8_t mac[32];
    hmac_sha256(c.key, sizeof(c.key), plain, ctlen, mac);   // over the CIPHERTEXT

    frame[0] = (uint8_t)(route | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT));
    frame[1] = 0x00;                             // empty path
    frame[2] = c.selector;
    frame[3] = mac[0];
    frame[4] = mac[1];
    memcpy(&frame[5], plain, ctlen);
    return (int)(5 + ctlen);
}
