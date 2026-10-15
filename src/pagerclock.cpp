#include "pagerclock.h"

static uint32_t baseUnix   = 0;      // 0 = never set
static uint32_t baseMillis = 0;

// Days since the epoch for the date last heard, or -1. Kept separately from
// the clock so a "time" line that arrives before its "date" is not lost.
static int32_t  knownDay   = -1;
static bool     havePendingTime = false;
static int      pendingHH = 0, pendingMM = 0;
static uint32_t pendingMs = 0;       // millis() when the held time line was heard

static bool     everSynced = false;
static uint32_t syncedAt   = 0;

// Howard Hinnant's days_from_civil: proleptic Gregorian, no table, no libc.
static int32_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int32_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + (int32_t)doe - 719468;
}

bool PagerClock::valid() { return baseUnix != 0; }
bool PagerClock::haveDate() { return knownDay >= 0; }
bool PagerClock::synced() { return everSynced; }
uint32_t PagerClock::syncedAtMs() { return syncedAt; }

uint32_t PagerClock::now() {
    if (!baseUnix) return 0;
    return baseUnix + (millis() - baseMillis) / 1000;
}

// `unixTime` was the time at millis() == atMs.
static void setAt(uint32_t unixTime, uint32_t atMs) {
    baseUnix   = unixTime;
    baseMillis = atMs;
    knownDay   = (int32_t)(unixTime / 86400u);
}

static void markSynced() {
    everSynced = true;
    syncedAt   = millis();
}

void PagerClock::setUnix(uint32_t unixTime) {
    setAt(unixTime, millis());
}

bool PagerClock::applyDateLine(const char *body) {
    int y = 0, mo = 0, d = 0;
    if (sscanf(body, "date %d-%d-%d", &y, &mo, &d) != 3) return false;
    if (y < 2020 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;

    knownDay = daysFromCivil(y, (unsigned)mo, (unsigned)d);

    // A "time" that arrived first has been waiting for exactly this. It was
    // HH:MM:00 when it was heard, not now.
    if (havePendingTime) {
        havePendingTime = false;
        setAt((uint32_t)knownDay * 86400u + (uint32_t)pendingHH * 3600u + (uint32_t)pendingMM * 60u,
              pendingMs);
        markSynced();
    } else if (baseUnix) {
        // Keep the running clock's time of day, move it onto the stated date.
        uint32_t sod = PagerClock::now() % 86400u;
        setUnix((uint32_t)knownDay * 86400u + sod);
    }
    return true;
}

bool PagerClock::applyTimeLine(const char *body) {
    int hh = 0, mm = 0;
    if (sscanf(body, "time %d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;

    if (knownDay < 0) {
        // No date yet. Hold on to it rather than guessing a day - the whole
        // point of asking is to stop guessing.
        havePendingTime = true;
        pendingHH = hh;
        pendingMM = mm;
        pendingMs = millis();
        return true;
    }

    // The repeater sends this AT the minute boundary, so the seconds really
    // are zero and this is a full resync, not a nudge.
    setUnix((uint32_t)knownDay * 86400u + (uint32_t)hh * 3600u + (uint32_t)mm * 60u);
    markSynced();
    return true;
}

// 2023-11 .. 2100: anything outside it is a device that has lost its clock.
static bool plausible(uint32_t unixTime) {
    return unixTime >= 1700000000u && unixTime <= 4102444800u;
}

void PagerClock::seedFromAdvert(uint32_t unixTime) {
    if (baseUnix) return;                       // #mesh, once heard, always wins
    if (!plausible(unixTime)) return;
    setUnix(unixTime);
}

bool PagerClock::setFromApp(uint32_t unixTime) {
    if (!plausible(unixTime)) return false;
    // The clock is set; a time line still waiting for its date has nothing
    // left to complete.
    havePendingTime = false;
    setUnix(unixTime);
    markSynced();
    return true;
}
