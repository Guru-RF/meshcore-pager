// ============================================================
// RF.Guru MeshCore Pager - display
//
// 128x64 mono OLED on software SPI. Software, not the spi1 block, because
// SPI1's default pins on this core are MISO=GP12, MOSI=GP15 and SS=GP13 -
// which on this board are the button, the buzzer and the charge sense. Any
// SPI1.begin() would reconfigure all three out from under the firmware, and
// U8g2's second-hardware-SPI constructors are not even compiled on this core
// (it does not define SPI_INTERFACES_COUNT). Bit-banging GP10/GP11 touches
// nothing else and costs a few milliseconds a frame, against a redraw rate
// of a handful per second.
//
// main.cpp owns the state and fills a UiModel; this file only draws. That
// keeps every decision about what the pager is doing in one place, and makes
// the screen impossible to disagree with the LED.
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"

// Where the position shown came from - see choosePosition() in main.cpp.
enum PosSource { POS_NONE = 0, POS_HEARD, POS_CONFIG, POS_APP };

// What the pager is running on - see readPower() in main.cpp. Both USB
// states mean the charger has power; PWR_FULL is the charger saying it is
// done (or, on the original board, the voltage saying so).
enum PowerSource { PWR_BATTERY = 0, PWR_CHARGING, PWR_FULL };

enum UiView {
    VIEW_BOOT = 0,      // splash and setup progress
    VIEW_MESSAGE,       // a message, or the empty-store screen
    VIEW_STATUS,        // node identity, position, link health
    VIEW_ALERT,         // an alarm is sounding
    VIEW_FETCH,         // a fetch is out and we are waiting
    VIEW_APP,           // a MeshCore app is attached and is the interface
    VIEW_FATAL          // setup failed and said why
};

struct UiModel {
    const PagerConfig *cfg;
    UiView   view;

    // Scroll position. 0..count-1 select a message; count is the status
    // page, so one more press past the oldest message lands on it.
    int      sel;

    uint32_t nowUnix;           // 0 = the clock has never been set
    bool     clockValid;

    float    battVolts;
    int      battPct;
    PowerSource power;

    bool     radioOk;
    bool     identityOk;
    int      repeaters;         // distinct repeaters heard since boot
    char     locator[8];        // "" until one is learned or configured
    PosSource posSource;

    bool     holdIsFetch;       // what a long press would do right now
    uint32_t fetchStartedMs;    // 0 = no fetch outstanding
    uint32_t adverts;
    int      nodes;             // nodes heard advertising, as the app's contacts

    int16_t  lastRssi;
    int8_t   lastSnrQ;
    uint32_t lastRxMs;          // 0 = nothing heard yet

    char     toast[40];         // transient one-line overlay
    uint32_t toastUntilMs;

    char     line[64];          // boot progress / fatal reason
};

namespace Ui {

bool begin();
bool present();

// Draw the model. Cheap enough to call whenever anything changes; main.cpp
// also calls it on a timer while an animation is running.
void render(const UiModel &m);

// Screen power. The LED keeps reporting state after the panel sleeps, which
// is what makes a timeout acceptable on a device you glance at.
void wake();
void sleep();
bool awake();

}  // namespace Ui
