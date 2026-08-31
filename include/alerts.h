// ============================================================
// RF.Guru MeshCore Pager - buzzer and LED choreography
//
// Everything here is non-blocking: play() arms a pattern and service(),
// called from the main loop, walks it. Nothing sleeps, because the radio is
// polled and a delay() long enough to be heard is long enough to sit on top
// of a packet.
//
// The frequencies are not guesses. The previous firmware for this same board
// (github.com/Guru-RF/LoRa433APRSpager, code.py) drove the transducer with
// pwmio at a 50% duty cycle and swept it from 330 Hz to 1165 Hz, so it is
// audible across the whole of that range - it is not the narrow-resonance
// part its package suggests. The alarm below reproduces that sweep exactly.
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"

enum AlertPattern {
    ALERT_NONE = 0,
    ALERT_BOOT,         // E4 F4 G4 C5, LED sweep: the pager is alive
    ALERT_MESSAGE,      // the alarm. Loops until the button or alertSeconds.
    ALERT_CHIRP,        // traffic that is not for us
    ALERT_FETCH,        // a fetch just went out
    ALERT_FETCH_OK,     // mail came back
    ALERT_FETCH_NONE,   // the mailbox was empty
    ALERT_DELETE,       // message dropped
    ALERT_ACK,          // a press was registered
    ALERT_TX,           // an advert went out
    ALERT_ERROR
};

// What the LED does when no pattern is playing.
enum AlertIdle {
    IDLE_OFF = 0,
    IDLE_STEADY,
    IDLE_PULSE,         // a short blip every few seconds - cheap on a battery
    IDLE_BREATHE        // slow fade in and out
};

namespace Alerts {

void begin(const PagerConfig &cfg);

// Arm a pattern. `repeatMs` > 0 loops it for that long (the alarm); 0 plays
// it once. A new play() replaces whatever was running.
void play(AlertPattern p, uint32_t repeatMs = 0);

// Silence and darken immediately, and drop any repeat.
void stop();

// True while a looping alarm is running - that is, while a button press
// should mean "shut up" rather than "next message".
bool alarming();

void setIdle(AlertIdle mode, uint8_t r, uint8_t g, uint8_t b);

// Call every loop pass.
void service();

}  // namespace Alerts
