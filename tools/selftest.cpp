// ============================================================
// RF.Guru MeshCore Pager - host self-test
//
//   make test
//
// This links the pager's ACTUAL protocol sources (src/meshcore.cpp,
// src/pagerclock.cpp, include/grid.h) against the meshcore-repeater's actual
// packet.c, advert.c and identity.c, and makes the two talk to each other. A
// framing or key-derivation mistake shows up here rather than as a pager that
// sits in a pocket hearing nothing.
//
// The two sides do not even share an Ed25519 implementation - the pager uses
// orlp/ed25519, the repeater TweetNaCl - so the advert tests below double as
// proof that the signatures really are interoperable RFC 8032 and not merely
// self-consistent.
//
// The companion protocol (src/companion.cpp) has no counterparty to link:
// the apps are not C. Its tests drive it the way an app does, byte for byte,
// and check every reply against the offsets in upstream's
// examples/companion_radio/MyMesh.cpp - and check what it puts on the air
// with the repeater's own code, as everything else here is.
// ============================================================

#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>

#include "companion.h"
#include "config_file.h"
#include "grid.h"
#include "meshcore.h"
#include "pagerclock.h"

extern "C" {
#include "packet.h"
#include "advert.h"
#include "identity.h"
#include "util.h"
#include "aes128.h"
#include "sha256.h"
#include "hmac_sha256.h"
}

HostRp2040  rp2040;
FatFSClass  FatFS;

// pub_key(32) + timestamp(4) + signature(64): where app_data starts.
#define ADV_DATA_OFF_TEST 100

static int failures = 0;
static int checks   = 0;

static void ck(bool cond, const char *what) {
    checks++;
    if (!cond) { failures++; printf("  FAIL  %s\n", what); }
    else       { printf("  ok    %s\n", what); }
}

// ============================================================
// The repeater's side of a public channel.
//
// channel_build_txt() and channel_decrypt() are static inside the repeater's
// src/mesh.c, so they cannot be linked; these are verbatim copies. If that
// file changes, re-copy them - the point of this test is that the pager
// agrees with the repeater, and a stale copy would agree with nothing.
// ============================================================

// The repeater's channel struct lives in its config.h, which drags in the
// whole daemon. Only these three fields matter on the wire.
struct rpt_channel_t {
    char    name[32];
    uint8_t secret[32];      // 16 significant bytes, zero-padded - as the repeater stores it
    uint8_t hash;            // on-air selector
};

// A channel from its 16-byte key, as the repeater's register_channel() keeps
// a public_channel PSK.
static void rpt_channel_from_key(rpt_channel_t *ch, const char *name, const uint8_t key[16]) {
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name);
    memcpy(ch->secret, key, 16);               // bytes [16..31] stay zero

    SHA256_CTX c;
    uint8_t digest[SHA256_BLOCK_SIZE];
    sha256_init(&c);
    sha256_update(&c, ch->secret, 16);         // EXACTLY the raw key bytes
    sha256_final(&c, digest);
    ch->hash = digest[0];
}

static void rpt_channel_from_name(rpt_channel_t *ch, const char *name) {
    SHA256_CTX c;
    uint8_t digest[SHA256_BLOCK_SIZE];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)name, strlen(name));
    sha256_final(&c, digest);
    rpt_channel_from_key(ch, name, digest);
}

static int rpt_build_txt(const rpt_channel_t *ch, const char *sender,
                         const char *text, uint32_t ts, uint8_t route_type,
                         mc_packet_t *pkt) {
    uint8_t plain[MC_MAX_PACKET_PAYLOAD];
    size_t i = 0;
    plain[0] = (uint8_t)ts; plain[1] = (uint8_t)(ts >> 8);
    plain[2] = (uint8_t)(ts >> 16); plain[3] = (uint8_t)(ts >> 24);
    i = 4;
    plain[i++] = 0;                                    /* txt_type = plain */
    int n = snprintf((char *)&plain[i], sizeof(plain) - i, "%s: %s",
                     sender ? sender : "", text);
    if (n < 0) return -1;
    i += (size_t)n;
    size_t ctlen = (i + 15) & ~(size_t)15;
    if (ctlen == 0) ctlen = 16;
    if (3 + ctlen > MC_MAX_PACKET_PAYLOAD) return -1;
    while (i < ctlen) plain[i++] = 0;

    aes128_ctx_t ac;
    aes128_init(&ac, ch->secret);
    aes128_ecb_encrypt(&ac, plain, ctlen);

    memset(pkt, 0, sizeof(*pkt));
    pkt->header   = pkt_make_header(PAYLOAD_VER_1, PAYLOAD_TYPE_GRP_TXT, route_type);
    pkt->path_len = 0;
    pkt->payload[0] = ch->hash;
    uint8_t mac[32];
    hmac_sha256(ch->secret, sizeof(ch->secret), plain, ctlen, mac);
    pkt->payload[1] = mac[0];
    pkt->payload[2] = mac[1];
    memcpy(&pkt->payload[3], plain, ctlen);
    pkt->payload_len = (uint16_t)(3 + ctlen);
    return 0;
}

static int rpt_decrypt(const rpt_channel_t *ch, const mc_packet_t *pkt,
                       uint8_t *out, size_t outsz) {
    if (pkt->payload_len < 3 + 16) return -1;
    if (pkt->payload[0] != ch->hash) return -1;
    size_t ctlen = (size_t)pkt->payload_len - 3;
    if (ctlen % 16 != 0 || ctlen + 1 > outsz) return -1;

    uint8_t mac[32];
    hmac_sha256(ch->secret, sizeof(ch->secret), &pkt->payload[3], ctlen, mac);
    if (mac[0] != pkt->payload[1] || mac[1] != pkt->payload[2]) return -2;   /* MAC */

    memcpy(out, &pkt->payload[3], ctlen);
    aes128_ctx_t ac;
    aes128_init(&ac, ch->secret);
    aes128_ecb_decrypt(&ac, out, ctlen);
    out[ctlen] = '\0';
    return (int)ctlen;
}

// ============================================================
// tests
// ============================================================

static void testGrid() {
    printf("\nMaidenhead\n");

    char loc[8];
    gridFromLatLon(50.8503, 4.3517, loc, 6);          // Brussels
    ck(strcmp(loc, "JO20EU") == 0, "50.85N 4.35E -> JO20EU");

    gridFromLatLon(50.8503, 4.3517, loc, 4);
    ck(strcmp(loc, "JO20") == 0, "same point, 4 characters -> JO20");

    double lat = 0, lon = 0;
    ck(gridCentre("JO20EU", &lat, &lon), "JO20EU parses");
    // The centre must be inside the square that produced it: half a 6-char
    // cell is 1/48 deg of latitude and 1/24 of longitude.
    ck(fabs(lat - 50.8503) < (1.0 / 48.0) && fabs(lon - 4.3517) < (2.0 / 48.0),
       "its centre is within half a cell of the original point");

    char back[8];
    gridFromLatLon(lat, lon, back, 6);
    ck(strcmp(back, "JO20EU") == 0, "the centre re-encodes to the same square");

    ck(gridCentre("JO20eu", &lat, &lon), "lowercase locators parse");
    ck(!gridCentre("JO2", &lat, &lon), "a 3-character locator is rejected");
    ck(!gridCentre("ZZ99XX", &lat, &lon), "an out-of-range field is rejected");

    gridFromLatLon(-33.86, 151.21, loc, 6);           // Sydney
    ck(strcmp(loc, "QF56OD") == 0, "southern/eastern hemisphere -> QF56OD");
}

static void testChannelDerivation() {
    printf("\nChannel key derivation\n");

    const char *names[] = {"#mbox", "#mesh", "#public"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        MeshChannel pager;
        Mesh::channelInit(pager, names[i]);
        rpt_channel_t rpt;
        rpt_channel_from_name(&rpt, names[i]);

        char what[64];
        snprintf(what, sizeof(what), "%s: key matches the repeater's", names[i]);
        ck(memcmp(pager.key, rpt.secret, 32) == 0, what);
        snprintf(what, sizeof(what), "%s: on-air selector matches (0x%02X)", names[i], rpt.hash);
        ck(pager.selector == rpt.hash, what);
    }
}

static void testPublicChannel() {
    printf("\nMeshCore's built-in Public channel\n");

    // The PSK exactly as the repeater's config takes it, decoded by the
    // repeater's own base64_decode().
    uint8_t psk[32];
    int plen = base64_decode("izOH6cXN6mrJ5e26oRXNcg==", psk, sizeof(psk));
    ck(plen == 16, "the repeater decodes the published PSK to 16 bytes");

    MeshChannel pager;
    Mesh::channelInit(pager, "Public");
    ck(memcmp(pager.key, psk, 16) == 0, "the pager's Public key is that PSK, not SHA256 of the name");
    rpt_channel_t rpt;
    rpt_channel_from_key(&rpt, "Public", psk);
    ck(memcmp(pager.key, rpt.secret, 32) == 0, "zero-padded to 32 for the HMAC, as the repeater keeps it");
    ck(pager.selector == rpt.hash, "and the on-air selector matches");

    MeshChannel lower;
    Mesh::channelInit(lower, "public");
    ck(memcmp(lower.key, pager.key, 32) == 0 && strcmp(lower.name, "Public") == 0,
       "\"public\" in config.txt is the same channel, shown as \"Public\"");
    MeshChannel tag;
    Mesh::channelInit(tag, "#public");
    ck(memcmp(tag.key, pager.key, 16) != 0 && strcmp(tag.name, "#public") == 0,
       "\"#public\" is a different channel, keyed from its name");

    mc_packet_t tx;
    rpt_build_txt(&rpt, "ON4ABC", "hello Public", 1756600500u, ROUTE_TYPE_FLOOD, &tx);
    uint8_t wire[MC_MAX_TRANS_UNIT];
    int wlen = pkt_serialize(&tx, wire, sizeof(wire));
    MeshPacket rx;
    uint32_t ts = 0;
    char sender[32], body[160];
    ck(wlen > 0 && Mesh::parse(rx, wire, (size_t)wlen) &&
       Mesh::decryptGroupText(pager, rx, &ts, sender, sizeof(sender), body, sizeof(body)) &&
       strcmp(sender, "ON4ABC") == 0 && strcmp(body, "hello Public") == 0,
       "the pager reads the repeater's message on Public");
    ck(!Mesh::decryptGroupText(tag, rx, &ts, sender, sizeof(sender), body, sizeof(body)),
       "and #public does not");

    uint8_t frame[MC_MAX_TRANS_UNIT];
    int flen = Mesh::buildGroupText(frame, sizeof(frame), pager, "OR7F-12", "hello back",
                                    1756600501u, ROUTE_TYPE_FLOOD);
    mc_packet_t back;
    uint8_t plain[MC_MAX_PACKET_PAYLOAD + 1];
    ck(flen > 0 && pkt_parse(&back, frame, (size_t)flen) == 0 &&
       rpt_decrypt(&rpt, &back, plain, sizeof(plain)) > 0 &&
       strcmp((const char *)&plain[5], "OR7F-12: hello back") == 0,
       "the repeater reads the pager's message on Public");
}

static void testGroupTextRepeaterToPager() {
    printf("\nGroup text: repeater -> pager\n");

    MeshChannel pager;
    Mesh::channelInit(pager, "#mbox");
    rpt_channel_t rpt;
    rpt_channel_from_name(&rpt, "#mbox");

    mc_packet_t tx;
    ck(rpt_build_txt(&rpt, "ON0RFG", "mbox 1: [OR7X] see you at 19z",
                     1756600000u, ROUTE_TYPE_DIRECT, &tx) == 0,
       "the repeater builds a reply");

    uint8_t wire[MC_MAX_TRANS_UNIT];
    int wlen = pkt_serialize(&tx, wire, sizeof(wire));
    ck(wlen > 0, "it serialises");

    MeshPacket rx;
    ck(Mesh::parse(rx, wire, (size_t)wlen), "the pager parses it");
    ck(Mesh::payloadType(rx) == PAYLOAD_TYPE_GRP_TXT, "as a group text");
    ck(Mesh::routeType(rx) == ROUTE_TYPE_DIRECT, "routed DIRECT, as a fetch reply is");

    uint32_t ts = 0;
    char sender[32], body[160];
    ck(Mesh::decryptGroupText(pager, rx, &ts, sender, sizeof(sender), body, sizeof(body)),
       "the pager decrypts it");
    ck(ts == 1756600000u, "the timestamp survives");
    ck(strcmp(sender, "ON0RFG") == 0, "the sender is split off correctly");
    ck(strcmp(body, "mbox 1: [OR7X] see you at 19z") == 0, "the body is intact");

    // A message on a channel we do not hold the key for must be silently
    // ignored, not half-decoded.
    MeshChannel other;
    Mesh::channelInit(other, "#mesh");
    ck(!Mesh::decryptGroupText(other, rx, &ts, sender, sizeof(sender), body, sizeof(body)),
       "the wrong channel does not decrypt it");

    // A single flipped ciphertext byte must fail the MAC rather than produce
    // garbage on screen.
    MeshPacket tampered = rx;
    tampered.payload[7] ^= 0x01;
    ck(!Mesh::decryptGroupText(pager, tampered, &ts, sender, sizeof(sender), body, sizeof(body)),
       "a tampered ciphertext fails the MAC");
}

static void testGroupTextPagerToRepeater() {
    printf("\nGroup text: pager -> repeater\n");

    MeshChannel pager;
    Mesh::channelInit(pager, "#mbox");
    rpt_channel_t rpt;
    rpt_channel_from_name(&rpt, "#mbox");

    uint8_t frame[MC_MAX_TRANS_UNIT];
    int flen = Mesh::buildGroupText(frame, sizeof(frame), pager, "OR7F-12", "fetch",
                                    1756600123u, ROUTE_TYPE_FLOOD);
    ck(flen > 0, "the pager builds a fetch");

    mc_packet_t rx;
    ck(pkt_parse(&rx, frame, (size_t)flen) == 0, "the repeater parses it");
    ck(pkt_payload_type(&rx) == PAYLOAD_TYPE_GRP_TXT, "as a group text");
    ck(pkt_is_route_flood(&rx), "routed FLOOD - the repeater only treats FLOOD as a command");
    ck(rx.payload[0] == rpt.hash, "with the right channel selector");

    uint8_t plain[MC_MAX_PACKET_PAYLOAD + 1];
    int plen = rpt_decrypt(&rpt, &rx, plain, sizeof(plain));
    ck(plen > 0, "the repeater decrypts it (MAC over the ciphertext, as it expects)");
    if (plen > 5) {
        ck(strcmp((const char *)&plain[5], "OR7F-12: fetch") == 0,
           "the plaintext is \"OR7F-12: fetch\"");
        // mbox_classify() needs the body after ": " to be exactly "fetch".
        const char *sep = strstr((const char *)&plain[5], ": ");
        ck(sep && strcmp(sep + 2, "fetch") == 0,
           "the body is exactly \"fetch\", which is what mbox_classify() requires");
    }

    // A long message must pad to a whole AES block and still round-trip.
    flen = Mesh::buildGroupText(frame, sizeof(frame), pager, "OR7F-12",
                                "the quick brown fox jumps over the lazy dog, and again",
                                1756600124u, ROUTE_TYPE_FLOOD);
    ck(flen > 0 && ((flen - 5) % 16) == 0, "a long message pads to a whole AES block");
    ck(pkt_parse(&rx, frame, (size_t)flen) == 0 && rpt_decrypt(&rpt, &rx, plain, sizeof(plain)) > 0,
       "and still decrypts");
}

static void testDedupHash() {
    printf("\nDedup hash\n");

    MeshChannel pager;
    Mesh::channelInit(pager, "#mesh");

    uint8_t frame[MC_MAX_TRANS_UNIT];
    int flen = Mesh::buildGroupText(frame, sizeof(frame), pager, "OR7F-12", "time",
                                    1756600200u, ROUTE_TYPE_FLOOD);

    MeshPacket a;
    mc_packet_t b;
    Mesh::parse(a, frame, (size_t)flen);
    pkt_parse(&b, frame, (size_t)flen);

    uint8_t ha[MC_HASH_SIZE], hb[MC_MAX_HASH_SIZE];
    Mesh::calcHash(a, ha);
    pkt_calc_hash(&b, hb);
    ck(memcmp(ha, hb, MC_HASH_SIZE) == 0, "matches the repeater's pkt_calc_hash");

    // The hash covers the payload and not the path, which is what makes a
    // re-flooded copy - same payload, one more hop recorded - a duplicate.
    MeshPacket hopped = a;
    hopped.pathLen = 1;
    hopped.path[0] = 0xAB;
    uint8_t hc[MC_HASH_SIZE];
    Mesh::calcHash(hopped, hc);
    ck(memcmp(ha, hc, MC_HASH_SIZE) == 0, "a re-flooded copy hashes the same");

    ck(Mesh::seenBefore(a) == false, "first sight is not a duplicate");
    ck(Mesh::seenBefore(hopped) == true, "the re-flooded copy is");

    // Two fetches a second apart must NOT collide, or the second is dropped
    // by every repeater that heard the first.
    uint8_t f2[MC_MAX_TRANS_UNIT];
    int l2 = Mesh::buildGroupText(f2, sizeof(f2), pager, "OR7F-12", "time",
                                  1756600201u, ROUTE_TYPE_FLOOD);
    MeshPacket c;
    Mesh::parse(c, f2, (size_t)l2);
    ck(Mesh::seenBefore(c) == false, "the same text one second later is a new packet");
}

static void testPacketParse() {
    printf("\nPacket parsing\n");

    MeshPacket p;
    uint8_t tooShort[1] = {0x15};
    ck(!Mesh::parse(p, tooShort, 1), "a 1-byte frame is rejected");

    // path_len 0xC0: hash_size 4, which MeshCore reserves.
    uint8_t badPath[3] = {0x15, 0xC0, 0x00};
    ck(!Mesh::parse(p, badPath, 3), "a reserved path hash size is rejected");

    // A path that claims more hops than the frame carries.
    uint8_t shortPath[3] = {0x15, 0x08, 0x00};
    ck(!Mesh::parse(p, shortPath, 3), "a truncated path is rejected");

    // TRANSPORT_FLOOD carries four transport-code bytes before the path.
    uint8_t transport[8] = {0x14, 0x01, 0x02, 0x03, 0x04, 0x00, 0xAA, 0xBB};
    ck(Mesh::parse(p, transport, 8), "a TRANSPORT_FLOOD frame parses");
    ck(p.transportCodes[0] == 0x0201 && p.transportCodes[1] == 0x0403,
       "its transport codes are little-endian");
    ck(p.payloadLen == 2, "and the payload starts after them");

    // Three recorded hops, one byte each.
    uint8_t hops[7] = {0x15, 0x03, 0x11, 0x22, 0x33, 0xAA, 0xBB};
    ck(Mesh::parse(p, hops, 7), "a 3-hop path parses");
    ck(Mesh::pathHashCount(p) == 3 && Mesh::pathHashSize(p) == 1, "3 hops of 1 byte");
    ck(p.path[0] == 0x11 && p.path[2] == 0x33, "the hops are in order");
    ck(p.payloadLen == 2, "and the payload follows them");
}

static void testAdvertPagerToRepeater() {
    printf("\nAdvert: pager -> repeater (orlp signs, TweetNaCl verifies)\n");

    char err[128] = "";
    ck(Mesh::begin(err, sizeof(err)), err[0] ? err : "the pager has an identity");

    PagerConfig cfg;
    configSetDefaults(cfg);
    strncpy(cfg.call, "OR7F-12", sizeof(cfg.call) - 1);
    strncpy(cfg.meshName, "OR7F-12", sizeof(cfg.meshName) - 1);
    strncpy(cfg.nodeType, "chat", sizeof(cfg.nodeType) - 1);
    strncpy(cfg.route, "flood", sizeof(cfg.route) - 1);

    // The centre of JO20EU, which is what the pager actually advertises.
    double lat = 0, lon = 0;
    gridCentre("JO20EU", &lat, &lon);

    uint8_t frame[MC_MAX_TRANS_UNIT];
    int flen = Mesh::buildAdvert(frame, sizeof(frame), cfg, true, lat, lon, 1756600300u);
    ck(flen > 0, "the pager builds a signed advert");

    mc_packet_t rx;
    ck(pkt_parse(&rx, frame, (size_t)flen) == 0, "the repeater parses it");
    ck(pkt_payload_type(&rx) == PAYLOAD_TYPE_ADVERT, "as an ADVERT");
    ck(pkt_is_route_flood(&rx), "routed FLOOD so neighbours re-flood it");

    uint8_t pub[MC_PUB_KEY_SIZE];
    ck(mc_advert_verify(&rx, pub), "TweetNaCl accepts the orlp/ed25519 signature");
    ck(memcmp(pub, Mesh::publicKey(), MC_PUB_KEY_SIZE) == 0, "and the key is ours");

    uint8_t type = 0;
    char name[32];
    ck(mc_advert_extract(&rx, &type, name, sizeof(name)) == 0, "app_data extracts");
    ck(type == ADV_TYPE_CHAT, "node type is chat");
    ck(strcmp(name, "OR7F-12") == 0, "the advertised name is OR7F-12");

    double rlat = 0, rlon = 0;
    ck(mc_advert_extract_location(&rx, &rlat, &rlon), "it carries a position");
    ck(fabs(rlat - lat) < 1e-5 && fabs(rlon - lon) < 1e-5, "which is the grid centre we set");

    // A tampered advert must fail, or anyone could advertise as this pager.
    mc_packet_t forged = rx;
    forged.payload[ADV_DATA_OFF_TEST] ^= 0x01;
    ck(!mc_advert_verify(&forged, pub), "a tampered advert fails verification");

    // Without a position (no repeater heard yet) the advert must still be
    // valid - the pager announces itself before it knows where it is.
    flen = Mesh::buildAdvert(frame, sizeof(frame), cfg, false, 0, 0, 1756600301u);
    ck(flen > 0 && pkt_parse(&rx, frame, (size_t)flen) == 0 && mc_advert_verify(&rx, pub),
       "a positionless advert is still valid");
    ck(!mc_advert_extract_location(&rx, &rlat, &rlon), "and carries no position");
}

static void testAdvertRepeaterToPager() {
    printf("\nAdvert: repeater -> pager (TweetNaCl signs, orlp verifies)\n");

    mc_identity_t id;
    mc_identity_generate(&id);

    mc_advert_info_t info;
    memset(&info, 0, sizeof(info));
    info.type         = ADV_TYPE_REPEATER;
    info.has_location = true;
    info.latitude     = 50.9012;
    info.longitude    = 4.4123;
    info.name         = "ON0RFG";

    mc_packet_t tx;
    ck(mc_advert_build(&tx, &id, &info, 1756600400u) == 0, "the repeater builds an advert");

    uint8_t wire[MC_MAX_TRANS_UNIT];
    int wlen = pkt_serialize(&tx, wire, sizeof(wire));
    ck(wlen > 0, "it serialises");

    MeshPacket rx;
    ck(Mesh::parse(rx, wire, (size_t)wlen), "the pager parses it");

    uint8_t pub[MC_PUB_KEY_SIZE];
    ck(Mesh::advertVerify(rx, pub), "orlp/ed25519 accepts the TweetNaCl signature");
    ck(memcmp(pub, id.pub, MC_PUB_KEY_SIZE) == 0, "and reports the repeater's key");

    uint8_t type = 0;
    char name[32];
    bool hasLoc = false;
    double lat = 0, lon = 0;
    uint32_t ts = 0;
    ck(Mesh::advertInfo(rx, &type, name, sizeof(name), &hasLoc, &lat, &lon, &ts),
       "the pager reads its app_data");
    ck(type == ADV_TYPE_REPEATER, "and sees a REPEATER - the only clock it will borrow");
    ck(strcmp(name, "ON0RFG") == 0, "with the right name");
    ck(hasLoc && fabs(lat - 50.9012) < 1e-5 && fabs(lon - 4.4123) < 1e-5, "and position");
    ck(ts == 1756600400u, "and timestamp, which is what seeds the clock");

    // This is the whole position story: the pager must not advertise the
    // repeater's own coordinates, only the centre of its square.
    char loc[8];
    double myLat = 0, myLon = 0;
    ck(gridSnap(lat, lon, 6, &myLat, &myLon, loc), "the heard position snaps to a grid centre");
    ck(strcmp(loc, "JO20EV") == 0, "which is JO20EV");
    ck(myLat != lat || myLon != lon, "and is NOT the repeater's own position");

    MeshPacket forged = rx;
    forged.payload[8] ^= 0x01;                       // inside the public key
    ck(!Mesh::advertVerify(forged, pub), "a tampered advert is rejected");
}

static void testClock() {
    printf("\nClock from #mesh\n");

    ck(!PagerClock::valid(), "starts unset - no RTC, no GPS");
    ck(!PagerClock::haveDate() && !PagerClock::synced(), "with no date, and never synced");
    ck(PagerClock::applyTimeLine("time 14:33 UTC"), "\"time 14:33 UTC\" parses");
    ck(!PagerClock::valid(), "but a time with no date cannot be an epoch yet");
    ck(!PagerClock::haveDate(), "so the date is what is still missing");

    // The date turns up a second later. The held time was 14:33:00 when it
    // was heard, so the clock must now read 14:33:01, not 14:33:00.
    usleep(1100 * 1000);
    ck(PagerClock::applyDateLine("date 2026-08-31"), "\"date 2026-08-31\" parses");
    ck(PagerClock::valid(), "and the held time completes the clock");

    // 2026-08-31 14:33:00 UTC
    ck(PagerClock::now() >= 1788186780u && PagerClock::now() <= 1788186782u,
       "which is 2026-08-31 14:33 UTC");
    ck(PagerClock::now() >= 1788186781u,
       "dated from when the time was heard, not from when the date came");
    ck(PagerClock::synced() && (millis() - PagerClock::syncedAtMs()) < 1000,
       "and that counts as a sync, just now");

    ck(!PagerClock::applyTimeLine("time 25:00 UTC"), "an impossible hour is rejected");
    ck(!PagerClock::applyDateLine("date 2026-13-01"), "an impossible month is rejected");
    ck(!PagerClock::applyTimeLine("pong rssi -90dBm"), "ordinary chatter is not a time");

    ck(PagerClock::applyTimeLine("time 15:00 UTC"), "a later resync applies");
    ck(PagerClock::now() >= 1788188400u && PagerClock::now() <= 1788188402u,
       "and moves the clock to 15:00");

    // seedFromAdvert must never override a clock that #mesh has set.
    PagerClock::seedFromAdvert(1700000001u);
    ck(PagerClock::now() > 1788100000u, "an advert cannot drag a set clock backwards");
}

// ============================================================
// The companion protocol, driven as an app drives it
// ============================================================

// The fake hardware behind the companion: a serial port that keeps
// everything written to it, and a radio that keeps the last frame.
static uint8_t portOut[16384];
static size_t  portOutLen   = 0;
static size_t  portReadPos  = 0;
static int     portRoom     = 4096;
static uint8_t lastTx[MC_MAX_TRANS_UNIT];
static size_t  lastTxLen    = 0;
static int     advertCalls  = 0;
static bool    advertFlood  = false;
static bool    rebooted     = false;
static double  fakeLat = 0, fakeLon = 0;

static int    fakeWritable() { return portRoom; }
static size_t fakeWrite(const uint8_t *d, size_t n) {
    if (portOutLen + n > sizeof(portOut)) return 0;
    memcpy(&portOut[portOutLen], d, n);
    portOutLen += n;
    return n;
}
static bool fakeTransmit(const uint8_t *f, size_t n) {
    memcpy(lastTx, f, n);
    lastTxLen = n;
    return true;
}
static bool     fakeAdvert(bool flood) { advertCalls++; advertFlood = flood; return true; }
static bool     fakePosition(double *lat, double *lon) { *lat = fakeLat; *lon = fakeLon; return true; }
static uint16_t fakeBattery() { return 3912; }
static void     fakeRadioStats(CompanionRadioStats *s) {
    s->noiseFloor = -121;
    s->txAirMs = 12500; s->rxAirMs = 345600;
    s->recv = 410; s->recvErrors = 7; s->sent = 12;
    s->recvFlood = 380; s->recvDirect = 30; s->sentFlood = 11; s->sentDirect = 1;
}
static void     fakeReboot() { rebooted = true; }
static int      setPosCalls = 0;
static bool     setPosSet   = false;
static double   setPosLat = 0, setPosLon = 0;
static void     fakeSetPosition(bool set, double lat, double lon) {
    setPosCalls++; setPosSet = set; setPosLat = lat; setPosLon = lon;
}
static float    fakeTemp = 24.5f;
static float    fakeMcuTemp() { return fakeTemp; }

static void portClear() { portOutLen = portReadPos = 0; }

// What an app writes: '<', length little-endian, payload.
static void appSend(const uint8_t *payload, size_t len) {
    Companion::feed('<');
    Companion::feed((uint8_t)(len & 0xFF));
    Companion::feed((uint8_t)(len >> 8));
    for (size_t i = 0; i < len; i++) Companion::feed(payload[i]);
}

static void appSendText(const char *bytes) {
    for (const char *p = bytes; *p; p++) Companion::feed((uint8_t)*p);
}

// What an app reads: the next '>' frame, or false if there is none.
static bool appRecv(const uint8_t **frame, size_t *len) {
    if (portReadPos + 3 > portOutLen || portOut[portReadPos] != '>') return false;
    size_t n = portOut[portReadPos + 1] | (portOut[portReadPos + 2] << 8);
    if (portReadPos + 3 + n > portOutLen) return false;
    *frame = &portOut[portReadPos + 3];
    *len   = n;
    portReadPos += 3 + n;
    return true;
}

// Send one command and take exactly one reply.
static bool appCall(const uint8_t *cmd, size_t len, const uint8_t **reply, size_t *rlen) {
    portClear();
    appSend(cmd, len);
    return appRecv(reply, rlen) && portReadPos == portOutLen;
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static bool isErr(const uint8_t *r, size_t n, uint8_t code) {
    return n == 2 && r[0] == RESP_CODE_ERR && r[1] == code;
}

// A group text from the repeater, as the pager would hand it on: parsed and
// decrypted by the pager's own code.
static bool repeaterSays(const MeshChannel &pagerCh, const char *chName, const char *sender,
                         const char *text, uint32_t ts, uint8_t route, uint8_t hops,
                         MeshPacket *out, char *snd, size_t sndSz, char *body, size_t bodySz) {
    rpt_channel_t rpt;
    rpt_channel_from_name(&rpt, chName);
    mc_packet_t tx;
    if (rpt_build_txt(&rpt, sender, text, ts, route, &tx) != 0) return false;
    tx.path_len = hops;
    for (uint8_t i = 0; i < hops; i++) tx.path[i] = (uint8_t)(0xA0 + i);
    uint8_t wire[MC_MAX_TRANS_UNIT];
    int wlen = pkt_serialize(&tx, wire, sizeof(wire));
    if (wlen <= 0 || !Mesh::parse(*out, wire, (size_t)wlen)) return false;
    out->snrQ = 26;                                      // 6.5 dB
    uint32_t got = 0;
    return Mesh::decryptGroupText(pagerCh, *out, &got, snd, sndSz, body, bodySz) && got == ts;
}

static void testCompanion() {
    printf("\nCompanion protocol: app <-> pager over USB\n");

    PagerConfig cfg;
    configSetDefaults(cfg);
    strncpy(cfg.call, "OR7F-12", sizeof(cfg.call) - 1);
    configResolve(cfg);                                  // meshName = call, #mbox,#mesh

    MeshChannel chans[PAGER_MAX_CHANNELS];
    for (int i = 0; i < cfg.nChannels; i++) Mesh::channelInit(chans[i], cfg.channels[i]);

    gridCentre("JO20EU", &fakeLat, &fakeLon);

    CompanionHost host;
    host.cfg       = &cfg;
    host.channels  = chans;
    host.nChannels = cfg.nChannels;
    host.version   = "v1.1.0";
    host.buildDate = "15 Oct 2026";
    host.writable  = fakeWritable;
    host.write     = fakeWrite;
    host.transmit  = fakeTransmit;
    host.advert    = fakeAdvert;
    host.position  = fakePosition;
    host.setPosition = fakeSetPosition;
    host.mcuTempC  = fakeMcuTemp;
    host.batteryMv = fakeBattery;
    host.radioStats = fakeRadioStats;
    host.reboot    = fakeReboot;
    Companion::begin(host);

    const uint8_t *r;
    size_t rn;

    // ---- framing ----

    ck(!Companion::active(), "nothing is attached at boot");

    // A terminal on the same port. Its '<' is followed by CR LF, which reads
    // as a 2573-byte frame; waiting for that would eat the next real one.
    portClear();
    appSendText("debug> hello <\r\n");
    ck(portOutLen == 0 && !Companion::active(), "terminal text is ignored, stray '<' included");

    uint8_t q[] = {CMD_DEVICE_QUERY, 3};
    ck(appCall(q, sizeof(q), &r, &rn), "DEVICE_QUERY gets exactly one '>' frame back");
    ck(Companion::active(), "and from then on the port belongs to the app");

    // A frame that stops half way is dropped after a pause, not glued onto
    // the start of the next one.
    portClear();
    Companion::feed('<'); Companion::feed(5); Companion::feed(0); Companion::feed(CMD_DEVICE_QUERY);
    usleep(600 * 1000);
    appSend(q, sizeof(q));
    ck(appRecv(&r, &rn) && r[0] == RESP_CODE_DEVICE_INFO, "a stalled frame is abandoned after 500 ms");

    // ---- DEVICE_INFO: upstream handleCmdFrame(), CMD_DEVICE_QUERY ----

    ck(appCall(q, sizeof(q), &r, &rn) && rn == 80, "DEVICE_INFO is 80 bytes");
    ck(r[0] == RESP_CODE_DEVICE_INFO && r[1] == CMP_FIRMWARE_VER, "code 13, firmware level 8");
    ck(r[2] == CMP_MAX_CONTACTS / 2 && r[3] == PAGER_MAX_CHANNELS,
       "max contacts / 2 at [2], channel slots at [3]");
    ck(strcmp((const char *)&r[8], "15 Oct 2026") == 0, "build date at [8]");
    ck(strcmp((const char *)&r[20], "RF.Guru MeshCore Pager") == 0, "manufacturer at [20]");
    ck(strcmp((const char *)&r[60], "v1.1.0") == 0, "version at [60]");

    // ---- SELF_INFO: upstream CMD_APP_START ----

    uint8_t start[] = {CMD_APP_START, 3, 0, 0, 0, 0, 0, 0, 't', 'e', 's', 't'};
    ck(appCall(start, sizeof(start), &r, &rn) && r[0] == RESP_CODE_SELF_INFO, "APP_START gets SELF_INFO");
    ck(rn == 58 + strlen("OR7F-12"), "58 fixed bytes, then the name");
    ck(r[1] == ADV_TYPE_CHAT && r[2] == 17 && r[3] == 20, "node type, tx power, max tx power");
    ck(memcmp(&r[4], Mesh::publicKey(), MC_PUB_KEY_SIZE) == 0, "our public key at [4]");
    ck((int32_t)rd32(&r[36]) == (int32_t)lround(fakeLat * 1e6) &&
       (int32_t)rd32(&r[40]) == (int32_t)lround(fakeLon * 1e6),
       "position at [36] and [40], micro-degrees");
    ck(rd32(&r[48]) == 434890 && rd32(&r[52]) == 62500, "434.890 MHz in kHz, 62.5 kHz in Hz");
    ck(r[56] == 8 && r[57] == 8, "SF8, CR 4/8");
    ck(memcmp(&r[58], "OR7F-12", 7) == 0, "and the mesh name");

    // ---- channels ----

    rpt_channel_t rptMbox;
    rpt_channel_from_name(&rptMbox, "#mbox");
    uint8_t gc[] = {CMD_GET_CHANNEL, 0};
    ck(appCall(gc, sizeof(gc), &r, &rn) && rn == 50 && r[0] == RESP_CODE_CHANNEL_INFO && r[1] == 0,
       "GET_CHANNEL 0 is 50 bytes");
    ck(strcmp((const char *)&r[2], "#mbox") == 0, "slot 0 is #mbox");
    ck(memcmp(&r[34], rptMbox.secret, 16) == 0, "with the key the repeater derives");

    gc[1] = 2;
    static const uint8_t zero[48] = {0};
    ck(appCall(gc, sizeof(gc), &r, &rn) && rn == 50 && memcmp(&r[2], zero, 48) == 0,
       "an unused slot is empty, not missing");
    gc[1] = 9;
    ck(appCall(gc, sizeof(gc), &r, &rn) && isErr(r, rn, ERR_CODE_NOT_FOUND),
       "a slot past the last is NOT_FOUND");

    // ---- sending: what goes on the air, read by the repeater ----

    rpt_channel_t rptMesh;
    rpt_channel_from_name(&rptMesh, "#mesh");
    const char *say = "hello from the app";
    uint8_t snd[64] = {CMD_SEND_CHANNEL_TXT_MSG, 0, 1};
    wr32(&snd[3], 1788190000u);
    memcpy(&snd[7], say, strlen(say));
    lastTxLen = 0;
    ck(appCall(snd, 7 + strlen(say), &r, &rn) && rn == 1 && r[0] == RESP_CODE_OK,
       "SEND_CHANNEL_TXT_MSG on slot 1 is OK");

    mc_packet_t air;
    uint8_t plain[MC_MAX_PACKET_PAYLOAD + 1];
    ck(lastTxLen > 0 && pkt_parse(&air, lastTx, lastTxLen) == 0, "it went on the air and the repeater parses it");
    ck(pkt_is_route_flood(&air), "routed FLOOD, as upstream sends channel text");
    int plen = rpt_decrypt(&rptMesh, &air, plain, sizeof(plain));
    ck(plen > 0, "the repeater decrypts it on #mesh");
    ck(plen > 5 && strcmp((const char *)&plain[5], "OR7F-12: hello from the app") == 0,
       "signed with the mesh name, as \"OR7F-12: hello from the app\"");
    ck(rd32(plain) == 1788190000u, "carrying the app's timestamp");

    // 84 two-byte characters and a letter: past the 160-byte limit, with the
    // cut landing inside a character.
    uint8_t lng[CMP_MAX_FRAME] = {CMD_SEND_CHANNEL_TXT_MSG, 0, 1};
    wr32(&lng[3], 1788190001u);
    size_t ln = 7;
    for (int i = 0; i < 84; i++) { lng[ln++] = 0xC3; lng[ln++] = 0xA9; }   // e-acute
    lng[ln++] = 'a';
    ck(appCall(lng, ln, &r, &rn) && r[0] == RESP_CODE_OK, "an over-long message is accepted");
    plen = (pkt_parse(&air, lastTx, lastTxLen) == 0) ? rpt_decrypt(&rptMesh, &air, plain, sizeof(plain)) : -1;
    const char *lt = (const char *)&plain[5];
    ck(plen > 0 && strlen(lt) <= CMP_MAX_TEXT, "cut to upstream's 160 bytes of \"sender: text\"");
    bool whole = plen > 0 && (strlen(lt) - 9) % 2 == 0;
    for (size_t i = 9; whole && i < strlen(lt); i += 2)
        whole = (uint8_t)lt[i] == 0xC3 && (uint8_t)lt[i + 1] == 0xA9;
    ck(whole, "on a character boundary, not through one");

    snd[1] = 1;                                          // TXT_TYPE_CLI_DATA
    ck(appCall(snd, 7 + strlen(say), &r, &rn) && isErr(r, rn, ERR_CODE_UNSUPPORTED_CMD),
       "a non-plain text type is refused");
    snd[1] = 0; snd[2] = 3;
    ck(appCall(snd, 7 + strlen(say), &r, &rn) && isErr(r, rn, ERR_CODE_NOT_FOUND),
       "an unused slot cannot be sent on");
    snd[2] = 1;
    ck(appCall(snd, 7, &r, &rn) && isErr(r, rn, ERR_CODE_ILLEGAL_ARG), "an empty message is refused");

    // ---- receiving: queue, tickle, sync ----

    MeshPacket pk;
    char from[32], body[MC_MAX_PACKET_PAYLOAD];
    ck(repeaterSays(chans[0], "#mbox", "ON0RFG", "mbox 1: [OR7X] see you at 19z", 1788190100u,
                    ROUTE_TYPE_DIRECT, 0, &pk, from, sizeof(from), body, sizeof(body)),
       "a fetch reply from the repeater decrypts");
    portClear();
    Companion::onChannelText(0, pk, 1788190100u, from, body);
    ck(appRecv(&r, &rn) && rn == 1 && r[0] == PUSH_CODE_MSG_WAITING, "the app is tickled with MSG_WAITING");
    ck(Companion::queued() == 1, "and the message waits in the queue");

    uint8_t sync[] = {CMD_SYNC_NEXT_MESSAGE};
    ck(appCall(sync, 1, &r, &rn) && r[0] == RESP_CODE_CHANNEL_MSG_RECV_V3, "SYNC_NEXT_MESSAGE hands it over as V3");
    ck((int8_t)r[1] == 26 && r[4] == 0, "SNR in quarter-dB at [1], slot at [4]");
    ck(r[5] == 0xFF, "path 0xFF: it came DIRECT");
    ck(r[6] == 0 && rd32(&r[7]) == 1788190100u, "plain text, sender's timestamp at [7]");
    ck(rn == 11 + strlen("ON0RFG: mbox 1: [OR7X] see you at 19z") &&
       memcmp(&r[11], "ON0RFG: mbox 1: [OR7X] see you at 19z", rn - 11) == 0,
       "and the text as sent, \"sender: text\"");
    ck(appCall(sync, 1, &r, &rn) && rn == 1 && r[0] == RESP_CODE_NO_MORE_MESSAGES, "then NO_MORE_MESSAGES");

    ck(repeaterSays(chans[1], "#mesh", "ON4ABC", "@OR7F lunch?", 1788190200u,
                    ROUTE_TYPE_FLOOD, 3, &pk, from, sizeof(from), body, sizeof(body)),
       "a flooded #mesh message, three hops out, decrypts");
    Companion::onChannelText(1, pk, 1788190200u, from, body);
    ck(appCall(sync, 1, &r, &rn) && r[4] == 1 && r[5] == 3, "slot 1, and the hop count as path length");

    // An app that has stopped reading: the tickle is dropped whole rather
    // than blocking or half-written, and the message still waits.
    portRoom = 2;
    portClear();
    Companion::onChannelText(1, pk, 1788190200u, from, body);
    ck(portOutLen == 0 && Companion::queued() == 1, "no room on the port: no tickle, message kept");
    portRoom = 4096;

    // An app that predates V3 gets the old layout from the same queue.
    Companion::portClosed();
    ck(!Companion::active(), "DTR dropping detaches the app");
    uint8_t q2[] = {CMD_DEVICE_QUERY, 2};
    appCall(q2, sizeof(q2), &r, &rn);
    ck(appCall(sync, 1, &r, &rn) && r[0] == RESP_CODE_CHANNEL_MSG_RECV, "a level-2 app gets CHANNEL_MSG_RECV");
    ck(r[1] == 1 && r[2] == 3 && r[3] == 0 && rd32(&r[4]) == 1788190200u &&
       memcmp(&r[8], "ON4ABC: @OR7F lunch?", rn - 8) == 0,
       "slot, path, type, timestamp and text, without the SNR bytes");
    appCall(q, sizeof(q), &r, &rn);                      // back to level 3

    for (int i = 0; i < CMP_QUEUE_SIZE + 1; i++) {
        char t[16];
        snprintf(t, sizeof(t), "msg %d", i);
        Companion::onChannelText(1, pk, 1788190300u + i, "ON4ABC", t);
    }
    ck(Companion::queued() == CMP_QUEUE_SIZE, "a full queue stays full");
    ck(appCall(sync, 1, &r, &rn) && memcmp(&r[11], "ON4ABC: msg 1", rn - 11) == 0,
       "by dropping its oldest message");
    while (Companion::queued()) appCall(sync, 1, &r, &rn);

    // ---- adverts become contacts ----

    mc_identity_t rid;
    mc_identity_generate(&rid);
    mc_advert_info_t info;
    memset(&info, 0, sizeof(info));
    info.type = ADV_TYPE_REPEATER;
    info.has_location = true;
    info.latitude = 50.9012;
    info.longitude = 4.4123;
    info.name = "ON0RFG";
    mc_packet_t atx;
    mc_advert_build(&atx, &rid, &info, 1788190400u);
    uint8_t awire[MC_MAX_TRANS_UNIT];
    int awlen = pkt_serialize(&atx, awire, sizeof(awire));
    MeshPacket apk;
    uint8_t apub[MC_PUB_KEY_SIZE];
    uint8_t atype = 0;
    char aname[32];
    bool aloc = false;
    double alat = 0, alon = 0;
    uint32_t ats = 0;
    ck(awlen > 0 && Mesh::parse(apk, awire, (size_t)awlen) && Mesh::advertVerify(apk, apub) &&
       Mesh::advertInfo(apk, &atype, aname, sizeof(aname), &aloc, &alat, &alon, &ats),
       "a repeater advert verifies and reads");

    portClear();
    Companion::onAdvert(apub, atype, aname, aloc, alat, alon, ats);
    ck(appRecv(&r, &rn) && rn == 33 && r[0] == PUSH_CODE_ADVERT && memcmp(&r[1], rid.pub, 32) == 0,
       "the app is told with PUSH_CODE_ADVERT and the key");
    ck(Companion::contacts() == 1, "and it is a contact");

    uint8_t gcs[] = {CMD_GET_CONTACTS};
    ck(appCall(gcs, 1, &r, &rn) && rn == 5 && r[0] == RESP_CODE_CONTACTS_START && rd32(&r[1]) == 1,
       "GET_CONTACTS starts with a count of 1");
    portClear();
    Companion::service();
    ck(appRecv(&r, &rn) && rn == 148 && r[0] == RESP_CODE_CONTACT, "then one 148-byte CONTACT");
    ck(memcmp(&r[1], rid.pub, 32) == 0 && r[33] == ADV_TYPE_REPEATER, "key at [1], type at [33]");
    ck(r[34] == 0 && r[35] == 0xFF, "no flags, path length 0xFF: no route is ever learned");
    ck(strcmp((const char *)&r[100], "ON0RFG") == 0, "name at [100]");
    ck(rd32(&r[132]) == 1788190400u, "its advert timestamp at [132]");
    ck((int32_t)rd32(&r[136]) == 50901200 && (int32_t)rd32(&r[140]) == 4412300,
       "its own position at [136] and [140], not a grid centre");
    uint32_t lastmod = rd32(&r[144]);
    ck(lastmod > 0, "lastmod at [144]");
    portClear();
    Companion::service();
    ck(appRecv(&r, &rn) && rn == 5 && r[0] == RESP_CODE_END_OF_CONTACTS && rd32(&r[1]) == lastmod,
       "END_OF_CONTACTS carries the newest lastmod");

    uint8_t gcs2[5] = {CMD_GET_CONTACTS};
    wr32(&gcs2[1], lastmod);
    appCall(gcs2, sizeof(gcs2), &r, &rn);
    portClear();
    Companion::service();
    ck(appRecv(&r, &rn) && r[0] == RESP_CODE_END_OF_CONTACTS, "asking since that lastmod sends nothing new");

    portClear();
    Companion::onAdvert(apub, atype, aname, aloc, alat, alon, ats);
    ck(portOutLen == 0, "a replay of the same advert changes nothing");
    Companion::onAdvert(Mesh::publicKey(), ADV_TYPE_CHAT, "OR7F-12", false, 0, 0, ats + 1);
    ck(Companion::contacts() == 1, "our own advert, re-flooded back, is not a contact");

    uint8_t rm[1 + MC_PUB_KEY_SIZE] = {CMD_REMOVE_CONTACT};
    memcpy(&rm[1], rid.pub, 32);
    ck(appCall(rm, sizeof(rm), &r, &rn) && r[0] == RESP_CODE_OK && Companion::contacts() == 0,
       "REMOVE_CONTACT removes it");
    ck(appCall(rm, sizeof(rm), &r, &rn) && isErr(r, rn, ERR_CODE_NOT_FOUND), "and a second time is NOT_FOUND");

    // ---- the clock ----

    uint8_t gt[] = {CMD_GET_DEVICE_TIME};
    ck(appCall(gt, 1, &r, &rn) && rn == 5 && r[0] == RESP_CODE_CURR_TIME && rd32(&r[1]) == PagerClock::now(),
       "GET_DEVICE_TIME reads the pager's clock");
    uint8_t st[5] = {CMD_SET_DEVICE_TIME};
    wr32(&st[1], 1790000000u);
    ck(appCall(st, sizeof(st), &r, &rn) && r[0] == RESP_CODE_OK, "SET_DEVICE_TIME from the phone is OK");
    ck(PagerClock::now() >= 1790000000u && PagerClock::now() <= 1790000001u, "and sets the clock");
    wr32(&st[1], 1000);
    ck(appCall(st, sizeof(st), &r, &rn) && isErr(r, rn, ERR_CODE_ILLEGAL_ARG) &&
       PagerClock::now() >= 1790000000u,
       "a phone that has lost its clock is refused, and changes nothing");

    // ---- the rest ----

    uint8_t adv[] = {CMD_SEND_SELF_ADVERT, 1};
    advertCalls = 0;
    ck(appCall(adv, sizeof(adv), &r, &rn) && r[0] == RESP_CODE_OK && advertCalls == 1 && advertFlood,
       "SEND_SELF_ADVERT 1 floods");
    ck(appCall(adv, 1, &r, &rn) && advertCalls == 2 && !advertFlood, "and with no argument is zero-hop");

    uint8_t bat[] = {CMD_GET_BATT_AND_STORAGE};
    ck(appCall(bat, 1, &r, &rn) && rn == 11 && r[0] == RESP_CODE_BATT_AND_STORAGE &&
       (r[1] | (r[2] << 8)) == 3912,
       "battery in millivolts");

    portClear();
    uint8_t raw[] = {0x15, 0x00, 0xAA, 0xBB, 0xCC};
    Companion::onRawPacket(raw, sizeof(raw), -97, -22);
    ck(appRecv(&r, &rn) && rn == 8 && r[0] == PUSH_CODE_LOG_RX_DATA && (int8_t)r[1] == -22 &&
       (int8_t)r[2] == -97 && memcmp(&r[3], raw, sizeof(raw)) == 0,
       "every packet heard is pushed raw to the app's log");

    // GET_STATS: upstream's three layouts, which Meshy asks for on connect.
    uint8_t gs[] = {CMD_GET_STATS, STATS_TYPE_CORE};
    ck(appCall(gs, sizeof(gs), &r, &rn) && rn == 11 && r[0] == RESP_CODE_STATS && r[1] == STATS_TYPE_CORE,
       "core stats are 11 bytes");
    ck((r[2] | (r[3] << 8)) == 3912 && rd32(&r[4]) == millis() / 1000 && r[8] == 0 && r[9] == 0 && r[10] == 0,
       "battery, uptime, no error flags, nothing queued to send");
    gs[1] = STATS_TYPE_RADIO;
    ck(appCall(gs, sizeof(gs), &r, &rn) && rn == 14 && r[1] == STATS_TYPE_RADIO, "radio stats are 14 bytes");
    ck((int16_t)(r[2] | (r[3] << 8)) == -121 && (int8_t)r[4] == -97 && (int8_t)r[5] == -22,
       "noise floor, and the last packet's RSSI and quarter-dB SNR");
    ck(rd32(&r[6]) == 12 && rd32(&r[10]) == 345, "airtime sent and received, in seconds");
    gs[1] = STATS_TYPE_PACKETS;
    ck(appCall(gs, sizeof(gs), &r, &rn) && rn == 30 && r[1] == STATS_TYPE_PACKETS, "packet stats are 30 bytes");
    ck(rd32(&r[2]) == 410 && rd32(&r[6]) == 12 && rd32(&r[10]) == 11 && rd32(&r[14]) == 1 &&
       rd32(&r[18]) == 380 && rd32(&r[22]) == 30 && rd32(&r[26]) == 7,
       "received, sent, sent flood/direct, received flood/direct, errors");
    gs[1] = 9;
    ck(appCall(gs, sizeof(gs), &r, &rn) && isErr(r, rn, ERR_CODE_ILLEGAL_ARG), "an unknown stats type is refused");

    uint8_t aa[] = {CMD_GET_AUTOADD_CONFIG};
    ck(appCall(aa, 1, &r, &rn) && rn == 3 && r[0] == RESP_CODE_AUTOADD_CONFIG && r[1] == 0x1F && r[2] == 0,
       "auto-add: every type, oldest overwritten, no hop limit - what the pager does");

    uint8_t dm[] = {CMD_SEND_TXT_MSG, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 'h', 'i'};
    ck(appCall(dm, sizeof(dm), &r, &rn) && isErr(r, rn, ERR_CODE_UNSUPPORTED_CMD),
       "a direct message is refused");
    uint8_t setch[2 + 32 + 16] = {CMD_SET_CHANNEL, 2, '#', 'x'};
    ck(appCall(setch, sizeof(setch), &r, &rn) && isErr(r, rn, ERR_CODE_UNSUPPORTED_CMD),
       "so is changing a channel - config.txt owns those");
    uint8_t setname[] = {CMD_SET_ADVERT_NAME, 'X'};
    ck(appCall(setname, sizeof(setname), &r, &rn) && isErr(r, rn, ERR_CODE_UNSUPPORTED_CMD),
       "and the name");

    // SET_ADVERT_LATLON: upstream's layout, micro-degrees, as Meshy sends it
    // from its settings page - with the reserved altitude on the end.
    uint8_t ll[13] = {CMD_SET_ADVERT_LATLON};
    wr32(&ll[1], (uint32_t)50850000);
    wr32(&ll[5], (uint32_t)(int32_t)4351000);
    wr32(&ll[9], 0);
    ck(appCall(ll, sizeof(ll), &r, &rn) && rn == 1 && r[0] == RESP_CODE_OK,
       "SET_ADVERT_LATLON is OK - a position is not a setting");
    ck(setPosCalls == 1 && setPosSet && fabs(setPosLat - 50.85) < 1e-9 && fabs(setPosLon - 4.351) < 1e-9,
       "and hands the pager 50.850000, 4.351000");
    wr32(&ll[5], (uint32_t)(int32_t)-4351000);
    ck(appCall(ll, 9, &r, &rn) && r[0] == RESP_CODE_OK && fabs(setPosLon + 4.351) < 1e-9,
       "west is negative, and the altitude is optional");
    wr32(&ll[1], 0); wr32(&ll[5], 0);
    ck(appCall(ll, 9, &r, &rn) && r[0] == RESP_CODE_OK && setPosCalls == 3 && !setPosSet,
       "0,0 withdraws it, as upstream reads 0,0 as no position");
    wr32(&ll[1], (uint32_t)91000000);
    ck(appCall(ll, 9, &r, &rn) && isErr(r, rn, ERR_CODE_ILLEGAL_ARG) && setPosCalls == 3,
       "a latitude past the pole is refused and changes nothing");
    ck(appCall(ll, 5, &r, &rn) && isErr(r, rn, ERR_CODE_ILLEGAL_ARG), "so is a short frame");

    // Self telemetry, as Meshy's device page asks for it: four bytes, no key.
    // The reply is a push code, and Cayenne LPP after an 8-byte header.
    uint8_t tr[4] = {CMD_SEND_TELEMETRY_REQ, 0, 0, 0};
    ck(appCall(tr, sizeof(tr), &r, &rn) && r[0] == PUSH_CODE_TELEMETRY_RESPONSE && r[1] == 0,
       "SEND_TELEMETRY_REQ for ourselves gets TELEMETRY_RESPONSE");
    ck(memcmp(&r[2], Mesh::publicKey(), 6) == 0, "our key prefix at [2]");
    ck(rn == 16 && r[8] == 1 && r[9] == 116 && ((r[10] << 8) | r[11]) == 391,
       "channel 1 voltage: 3912 mV as 391 hundredths, big-endian");
    ck(r[12] == 1 && r[13] == 103 && (int16_t)((r[14] << 8) | r[15]) == 245,
       "channel 1 temperature: 24.5 C as 245 tenths");
    fakeTemp = -3.2f;
    ck(appCall(tr, sizeof(tr), &r, &rn) && rn == 16 && (int16_t)((r[14] << 8) | r[15]) == -32,
       "below zero is signed");
    fakeTemp = NAN;
    ck(appCall(tr, sizeof(tr), &r, &rn) && rn == 12, "no sensor, no temperature - just the battery");
    uint8_t trk[4 + MC_PUB_KEY_SIZE] = {CMD_SEND_TELEMETRY_REQ, 0, 0, 0};
    ck(appCall(trk, sizeof(trk), &r, &rn) && isErr(r, rn, ERR_CODE_UNSUPPORTED_CMD),
       "telemetry from another node needs direct messaging, and is refused");

    uint8_t key[] = {CMD_EXPORT_PRIVATE_KEY};
    ck(appCall(key, 1, &r, &rn) && rn == 1 && r[0] == RESP_CODE_DISABLED, "the private key does not leave");

    uint8_t rb[] = {CMD_REBOOT, 'r', 'e', 'b', 'o', 'o', 't'};
    portClear();
    appSend(rb, sizeof(rb));
    ck(rebooted, "REBOOT \"reboot\" reboots");

    Companion::portClosed();
    portClear();
    Companion::onRawPacket(raw, sizeof(raw), -97, -22);
    Companion::onChannelText(1, pk, 1788190500u, from, body);
    ck(portOutLen == 0, "with the app gone, nothing is pushed");
    ck(Companion::queued() == 1, "but messages still queue for the next one");
}

int main() {
    FatFS.begin();                     // the shim's ./.selftest-fs, for /meshid.bin

    printf("RF.Guru MeshCore Pager - protocol self-test\n");
    printf("(pager sources vs meshcore-repeater sources)\n");

    testGrid();
    testChannelDerivation();
    testPublicChannel();
    testGroupTextRepeaterToPager();
    testGroupTextPagerToRepeater();
    testDedupHash();
    testPacketParse();
    testAdvertPagerToRepeater();
    testAdvertRepeaterToPager();
    testClock();
    testCompanion();                   // after testClock: the app moves the clock

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
