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
// ============================================================

#include <cstdio>
#include <cstring>
#include <cmath>

#include "config_file.h"
#include "grid.h"
#include "meshcore.h"
#include "pagerclock.h"

extern "C" {
#include "packet.h"
#include "advert.h"
#include "identity.h"
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

static void rpt_channel_from_name(rpt_channel_t *ch, const char *name) {
    memset(ch, 0, sizeof(*ch));
    snprintf(ch->name, sizeof(ch->name), "%s", name);

    SHA256_CTX c;
    uint8_t digest[SHA256_BLOCK_SIZE];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)name, strlen(name));
    sha256_final(&c, digest);
    memcpy(ch->secret, digest, 16);            // bytes [16..31] stay zero

    sha256_init(&c);
    sha256_update(&c, ch->secret, 16);         // EXACTLY the raw key bytes
    sha256_final(&c, digest);
    ch->hash = digest[0];
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
    ck(PagerClock::applyTimeLine("time 14:33 UTC"), "\"time 14:33 UTC\" parses");
    ck(!PagerClock::valid(), "but a time with no date cannot be an epoch yet");

    ck(PagerClock::applyDateLine("date 2026-08-31"), "\"date 2026-08-31\" parses");
    ck(PagerClock::valid(), "and the held time completes the clock");

    // 2026-08-31 14:33:00 UTC
    ck(PagerClock::now() >= 1788186780u && PagerClock::now() <= 1788186782u,
       "which is 2026-08-31 14:33:00 UTC");

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

int main() {
    FatFS.begin();                     // the shim's ./.selftest-fs, for /meshid.bin

    printf("RF.Guru MeshCore Pager - protocol self-test\n");
    printf("(pager sources vs meshcore-repeater sources)\n");

    testGrid();
    testChannelDerivation();
    testGroupTextRepeaterToPager();
    testGroupTextPagerToRepeater();
    testDedupHash();
    testPacketParse();
    testAdvertPagerToRepeater();
    testAdvertRepeaterToPager();
    testClock();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
