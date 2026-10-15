// ============================================================
// RF.Guru MeshCore Pager - MeshCore wire protocol
//
// Wire formats verified against meshcore-repeater (src/packet.h,
// src/advert.h, src/mesh.c), which is itself verified against upstream
// MeshCore. The crypto in lib/meshcrypto and lib/ed25519 is vendored from
// that same tree rather than reimplemented, so the two cannot drift.
//
// The pager is a LEAF. It receives, decrypts what it holds keys for, and
// originates its own adverts and channel messages - but it never forwards
// anything. That is deliberate: a battery-powered device in a pocket is a
// bad repeater, and re-flooding is what would flatten it.
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"

#define MC_MAX_PACKET_PAYLOAD 184
#define MC_MAX_PATH_SIZE       64
#define MC_MAX_TRANS_UNIT     255
#define MC_PUB_KEY_SIZE        32
#define MC_PRV_KEY_SIZE        64
#define MC_SIGNATURE_SIZE      64
#define MC_MAX_ADVERT_DATA     32
#define MC_HASH_SIZE            8

// header 0bVVPPPPRR: payload version, payload type, route type
#define PH_ROUTE_MASK  0x03
#define PH_TYPE_SHIFT  2
#define PH_TYPE_MASK   0x0F
#define PH_VER_SHIFT   6

#define ROUTE_TYPE_TRANSPORT_FLOOD  0x00
#define ROUTE_TYPE_FLOOD            0x01
#define ROUTE_TYPE_DIRECT           0x02
#define ROUTE_TYPE_TRANSPORT_DIRECT 0x03

#define PAYLOAD_TYPE_REQ        0x00
#define PAYLOAD_TYPE_RESPONSE   0x01
#define PAYLOAD_TYPE_TXT_MSG    0x02
#define PAYLOAD_TYPE_ACK        0x03
#define PAYLOAD_TYPE_ADVERT     0x04
#define PAYLOAD_TYPE_GRP_TXT    0x05
#define PAYLOAD_TYPE_GRP_DATA   0x06
#define PAYLOAD_TYPE_TRACE      0x09

#define ADV_TYPE_NONE     0
#define ADV_TYPE_CHAT     1
#define ADV_TYPE_REPEATER 2
#define ADV_TYPE_ROOM     3
#define ADV_TYPE_SENSOR   4

#define ADV_LATLON_MASK 0x10
#define ADV_FEAT1_MASK  0x20
#define ADV_FEAT2_MASK  0x40
#define ADV_NAME_MASK   0x80

struct MeshPacket {
    uint8_t  header;
    uint16_t transportCodes[2];
    uint8_t  pathLen;                       // encoded: bits0-5 count, bits6-7 size-1
    uint8_t  path[MC_MAX_PATH_SIZE];
    uint8_t  payload[MC_MAX_PACKET_PAYLOAD];
    uint16_t payloadLen;
    int16_t  rssi;                          // receive metadata, not transmitted
    int8_t   snrQ;                          // SNR in quarter-dB
};

// A public channel. The key is derived from the name alone - SHA256("#mbox")
// - so there is no key exchange: anyone who knows the name can read it,
// which is the point on an amateur band where obscuring meaning is not
// allowed anyway.
//
// The one exception is MeshCore's built-in channel, MC_PUBLIC_CHANNEL - the
// one every app starts with. Its key is not derived from anything: it is a
// fixed key published in upstream's source and in every app, and being
// published is what keeps it public. "#public" is a different channel.
#define MC_PUBLIC_CHANNEL "Public"

struct MeshChannel {
    char    name[PAGER_CHAN_LEN];
    uint8_t key[32];                        // 16 significant bytes, zero-padded
    uint8_t selector;                       // on-air channel byte
    bool    valid;
};

namespace Mesh {

// Load /meshid.bin, minting and persisting a keypair on first run. Must be
// called while the filesystem is still ours - before FatFSUSB.begin().
bool begin(char *err, size_t errLen);
bool identityOk();
const uint8_t *publicKey();

void channelInit(MeshChannel &c, const char *name);

bool parse(MeshPacket &p, const uint8_t *raw, size_t len);

static inline uint8_t payloadType(const MeshPacket &p) {
    return (uint8_t)((p.header >> PH_TYPE_SHIFT) & PH_TYPE_MASK);
}
static inline uint8_t routeType(const MeshPacket &p) { return p.header & PH_ROUTE_MASK; }
static inline uint8_t pathHashCount(const MeshPacket &p) { return p.pathLen & 0x3F; }
static inline uint8_t pathHashSize(const MeshPacket &p) { return (uint8_t)((p.pathLen >> 6) + 1); }

void calcHash(const MeshPacket &p, uint8_t out[MC_HASH_SIZE]);

// Dedup. Returns true if this packet has been seen before; records it on
// first sight. A flooded message reaches a pager from several repeaters and
// would otherwise alarm once per copy.
bool seenBefore(const MeshPacket &p);

// Record a frame we are about to transmit, so the repeaters' re-flood of it
// comes back as a duplicate instead of as an incoming message from
// ourselves.
void rememberFrame(const uint8_t *frame, size_t len);

// Decrypt a GRP_TXT on `c`. Returns false when the channel selector does not
// match, the MAC fails, or the plaintext is malformed. `sender` gets the
// self-reported name before the first ": " and `body` the text after it.
bool decryptGroupText(const MeshChannel &c, const MeshPacket &p,
                      uint32_t *ts, char *sender, size_t senderSz,
                      char *body, size_t bodySz);

// Ed25519 signature check over pub_key || timestamp || app_data. Costs about
// a tenth of a second on this chip, so it runs only for ADVERTs.
bool advertVerify(const MeshPacket &p, uint8_t outPub[MC_PUB_KEY_SIZE]);

bool advertInfo(const MeshPacket &p, uint8_t *type, char *name, size_t nameSz,
                bool *hasLoc, double *lat, double *lon, uint32_t *ts);

// The advert node type config.txt's nodeType names: chat, repeater, room or
// sensor, and chat for anything else.
uint8_t advertType(const PagerConfig &cfg);

// Build a signed self-advert. `unixTime` must be real: MeshCore uses the
// advert timestamp for freshness, so a fabricated one poisons the mesh.
// Returns the frame length, or -1.
int buildAdvert(uint8_t *frame, size_t frameSz, const PagerConfig &cfg,
                bool hasLoc, double lat, double lon, uint32_t unixTime);

// Build an encrypted public-channel message carrying "sender: text".
// Returns the frame length, or -1.
int buildGroupText(uint8_t *frame, size_t frameSz, const MeshChannel &c,
                   const char *sender, const char *text, uint32_t unixTime,
                   uint8_t route);

}  // namespace Mesh
