// ============================================================
// RF.Guru MeshCore Pager - the clock
//
// There is no RTC and no GPS on this board, so the only time the pager can
// have comes off the mesh. Two sources, in order of quality:
//
//   1. "time HH:MM UTC" and "date YYYY-MM-DD" on #mesh. The repeater
//      deliberately answers AT a minute boundary, so the seconds are known
//      to be zero and the result is exact to well under a second.
//   2. The timestamp inside a verified repeater ADVERT. Signed, and a few
//      seconds late by the time it has been flooded to us - good enough to
//      show, and good enough to let this pager start advertising, which it
//      must not do with a fabricated timestamp.
//
// (2) is only ever used to bootstrap a clock that has never been set. Once
// #mesh has answered, nothing an advert says can move the clock, so a
// replayed old advert cannot drag it backwards.
// ============================================================

#pragma once

#include <Arduino.h>

namespace PagerClock {

bool     valid();
uint32_t now();                 // UNIX seconds, or 0 when never set

void     setUnix(uint32_t unix);

// Accept "date YYYY-MM-DD". Returns false if it does not parse.
bool     applyDateLine(const char *body);

// Accept "time HH:MM UTC" (seconds assumed zero, which is why the repeater
// waits for the minute boundary). Needs a date - from a date line or from a
// clock that is already running - or it is remembered until one arrives.
bool     applyTimeLine(const char *body);

// Bootstrap from a signed repeater advert. Ignored once the clock is set.
void     seedFromAdvert(uint32_t unix);

}  // namespace PagerClock
