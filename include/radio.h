// ============================================================
// RF.Guru MeshCore Pager - SX1276 radio
//
// One chip, one profile: the IARU R1 ham MeshCore channel. Unlike the
// tracker, which retunes between APRS and mesh between packets, the pager
// only ever speaks MeshCore, so the radio sits in continuous receive and
// only leaves it to transmit.
//
// The module's DIO lines are not routed on this board, so there is no
// RxDone or TxDone interrupt: both directions poll the chip's IRQ flag
// register. The transmit poll feeds the watchdog, which a blocking
// transmit() could not.
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"

namespace Radio {

// Bring up the radio and leave it receiving. On failure fills `err`.
bool begin(const PagerConfig &cfg, char *err, size_t errLen);

bool ok();

// Transmit one frame, blocking until the chip reports TxDone or the time
// on air budget expires, then return to receive. Feeds the watchdog.
bool send(const uint8_t *data, size_t len);

// Non-blocking receive. Returns the frame length (>0) when one is waiting,
// 0 when nothing has arrived, and -1 when a frame was dropped (CRC or
// oversize). rssi is dBm and snrQ is SNR in quarter-dB, the unit MeshCore
// carries it in.
int poll(uint8_t *buf, size_t bufSz, int16_t *rssi, int8_t *snrQ);

// Put the chip back into continuous receive. send() does this itself; it is
// exposed for the recovery path.
void listen();

// Sleep mode, for a suspended pager: the chip keeps its settings and draws
// next to nothing. listen() wakes it.
void sleep();

uint32_t timeOnAirMs(size_t len);

// Read the settings back out of the chip, for the boot banner.
void describe(char *buf, size_t len);

// Counters for the status screen and the app's statistics page.
uint32_t rxCount();
uint32_t txCount();
uint32_t rxErrors();            // frames dropped for CRC or length
uint32_t rxAirMs();             // time on air of everything received
uint32_t txAirMs();             // and of everything sent

// What the receiver hears between packets, dBm: the lowest of a few reads of
// the live RSSI register. 0 when the radio did not start.
int16_t  noiseFloor();

}  // namespace Radio
