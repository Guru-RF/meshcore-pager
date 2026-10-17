#include "alerts.h"
#include "pins.h"

// One step of a pattern.
//
//   hzA == 0            silence
//   hzB == 0            hold hzA
//   hzB != 0            alternate hzA and hzB every WARBLE_MS - a two-tone
//   rise != 0           add `rise` Hz to both, linearly, across the step
//
// Those four cases cover every sound the pager makes, including the rising
// two-tone warble of the alarm, without a mode enum.
struct AlertStep {
    uint16_t hzA;
    uint16_t hzB;
    int16_t  rise;
    uint16_t ms;
    uint8_t  r, g, b;
};

// The old firmware alternated its two tones every 20 ms. Slower sounds like
// two separate notes; faster starts to sound like one rough one.
#define WARBLE_MS 20

// ---- the patterns ----

// E4 F4 G4 C5, from the startup tune in the previous firmware.
static const AlertStep P_BOOT[] = {
    {330, 0, 0, 120, 120,   0,   0},
    {349, 0, 0, 120,   0, 120,   0},
    {392, 0, 0, 120,   0,   0, 160},
    {523, 0, 0, 260, 120, 120, 160},
    {  0, 0, 0,  80,   0,   0,   0},
};

// The alarm, and the reason this file exists: the two-tone warble from
// LoRa433APRSpager, 330/580 Hz rising to 915/1165 over 1.6 s. It is split
// into four 400 ms steps only so the LED can strobe through it - the sweep
// itself is continuous, each step picking up where the last one left off.
// The silence afterwards is deliberate: an alarm that never pauses cannot be
// answered, only endured.
static const AlertStep P_MESSAGE[] = {
    {330,  580, 146, 400, 255, 255, 255},
    {476,  726, 146, 400, 255,   0,   0},
    {622,  872, 146, 400, 255, 255, 255},
    {768, 1018, 147, 400, 255,   0,   0},
    {  0,    0,   0, 350,   0,   0,   0},
};

// The previous firmware's "beacon" blip, 440 then 880, stretched from 20 ms
// to something you can actually hear across a room.
static const AlertStep P_CHIRP[] = {
    {440, 0, 0, 45,   0,   0, 200},
    {880, 0, 0, 45,   0,   0, 200},
    {  0, 0, 0, 25,   0,   0,   0},
};

static const AlertStep P_FETCH[] = {
    {440, 0, 606, 220,   0,  90, 220},   // A4 sweeping up to C6
    {  0, 0,   0,  60,   0,   0,   0},
};

// A major arpeggio - C5 E5 G5 C6. Nothing else the pager plays is in a major
// key, so "mail arrived" is recognisable without looking.
static const AlertStep P_FETCH_OK[] = {
    { 523, 0, 0,  70, 0, 255, 0},
    { 659, 0, 0,  70, 0, 255, 0},
    { 784, 0, 0,  70, 0, 255, 0},
    {1046, 0, 0, 200, 0, 255, 0},
    {   0, 0, 0,  90, 0,   0, 0},
};

static const AlertStep P_FETCH_NONE[] = {
    {392, 0, 0, 140, 255, 140, 0},
    {  0, 0, 0,  70,   0,   0, 0},
    {330, 0, 0, 140, 255, 140, 0},
    {  0, 0, 0,  60,   0,   0, 0},
};

static const AlertStep P_DELETE[] = {
    {880, 0, -550, 260, 255, 0, 0},      // falling: the opposite of the fetch
    {  0, 0,    0,  60,  90, 0, 0},
};

static const AlertStep P_ACK[] = {
    {880, 0, 0, 25, 0, 255, 0},
    {  0, 0, 0, 45, 0,   0, 0},
};

static const AlertStep P_TX[] = {
    {1046, 0, 0, 18, 160, 160, 255},
    {   0, 0, 0, 40,   0,   0,   0},
};

static const AlertStep P_ERROR[] = {
    {220, 165, 0, 320, 255, 0, 0},       // a low, rough warble
    {  0,   0, 0, 130,   0, 0, 0},
    {220, 165, 0, 320, 255, 0, 0},
    {  0,   0, 0,  80,   0, 0, 0},
};

struct PatternDef { const AlertStep *s; uint8_t n; };

static const PatternDef PATTERNS[] = {
    {nullptr,      0},                                            // ALERT_NONE
    {P_BOOT,       sizeof(P_BOOT)       / sizeof(AlertStep)},
    {P_MESSAGE,    sizeof(P_MESSAGE)    / sizeof(AlertStep)},
    {P_CHIRP,      sizeof(P_CHIRP)      / sizeof(AlertStep)},
    {P_FETCH,      sizeof(P_FETCH)      / sizeof(AlertStep)},
    {P_FETCH_OK,   sizeof(P_FETCH_OK)   / sizeof(AlertStep)},
    {P_FETCH_NONE, sizeof(P_FETCH_NONE) / sizeof(AlertStep)},
    {P_DELETE,     sizeof(P_DELETE)     / sizeof(AlertStep)},
    {P_ACK,        sizeof(P_ACK)        / sizeof(AlertStep)},
    {P_TX,         sizeof(P_TX)         / sizeof(AlertStep)},
    {P_ERROR,      sizeof(P_ERROR)      / sizeof(AlertStep)},
};

// ---- state ----

static PagerConfig  conf;
static bool         started       = false;
static AlertPattern current       = ALERT_NONE;
static uint8_t      step          = 0;
static uint32_t     stepStartMs   = 0;
static uint32_t     stepUntilMs   = 0;
static uint32_t     nextWarbleMs  = 0;
static bool         warblePhase   = false;
static uint32_t     repeatUntilMs = 0;
static bool         toneOn        = false;

static AlertIdle idleMode = IDLE_OFF;
static uint8_t   idleR = 0, idleG = 0, idleB = 0;
static bool      ledDark = false;

// The three dice share one 470R resistor to +3V3 and light when driven LOW,
// so every duty cycle is inverted and scaled by ledBrightness.
static void ledWrite(uint8_t r, uint8_t g, uint8_t b) {
    if (ledDark) r = g = b = 0;
    uint16_t scale = (uint16_t)conf.ledBrightness;
    analogWrite(PIN_LED_R, 255 - (uint8_t)((uint16_t)r * scale / 255));
    analogWrite(PIN_LED_G, 255 - (uint8_t)((uint16_t)g * scale / 255));
    analogWrite(PIN_LED_B, 255 - (uint8_t)((uint16_t)b * scale / 255));
}

static void silence() {
    if (toneOn) { noTone(PIN_BUZZER); toneOn = false; }
}

// Work out what the buzzer should be doing at `now` within the current step,
// and set it. Called on every step change and on every warble tick.
static void applyTone(uint32_t now) {
    const AlertStep &s = PATTERNS[current].s[step];
    if (!conf.buzzer || s.hzA == 0) { silence(); return; }

    uint32_t elapsed = now - stepStartMs;
    if (elapsed > s.ms) elapsed = s.ms;

    int32_t hz = (int32_t)((warblePhase && s.hzB) ? s.hzB : s.hzA);
    if (s.rise && s.ms) hz += (int32_t)s.rise * (int32_t)elapsed / (int32_t)s.ms;

    hz = hz * conf.tonePitch / 100;
    tone(PIN_BUZZER, (unsigned int)constrain(hz, 100, 6000));
    toneOn = true;
}

static void applyStep() {
    const AlertStep &s = PATTERNS[current].s[step];
    uint32_t now = millis();
    stepStartMs  = now;
    stepUntilMs  = now + s.ms;
    nextWarbleMs = now + WARBLE_MS;
    warblePhase  = false;
    applyTone(now);
    ledWrite(s.r, s.g, s.b);
}

void Alerts::begin(const PagerConfig &cfg) {
    conf = cfg;
    pinMode(PIN_BUZZER, OUTPUT);
    digitalWrite(PIN_BUZZER, LOW);
    // analogWrite claims the pin on first use; write the off state (HIGH,
    // because the LEDs are common anode) before anything can see it flash.
    ledWrite(0, 0, 0);
    started = true;
}

void Alerts::play(AlertPattern p, uint32_t repeatMs) {
    if (!started || p == ALERT_NONE || p >= (int)(sizeof(PATTERNS) / sizeof(PATTERNS[0])))
        return;
    current       = p;
    step          = 0;
    repeatUntilMs = repeatMs ? millis() + repeatMs : 0;
    applyStep();
}

void Alerts::stop() {
    current       = ALERT_NONE;
    repeatUntilMs = 0;
    silence();
    ledWrite(0, 0, 0);
}

bool Alerts::alarming() {
    return current != ALERT_NONE && repeatUntilMs != 0;
}

void Alerts::setIdle(AlertIdle mode, uint8_t r, uint8_t g, uint8_t b) {
    idleMode = mode;
    idleR = r; idleG = g; idleB = b;
}

void Alerts::setLedDark(bool dark) {
    if (dark == ledDark) return;
    ledDark = dark;
    // Now, not at the next step: a pattern step can hold its colour for a
    // while, and the idle code only writes when it runs.
    if (dark && started) ledWrite(0, 0, 0);
}

void Alerts::service() {
    if (!started) return;
    uint32_t now = millis();

    if (current != ALERT_NONE) {
        if ((int32_t)(now - stepUntilMs) < 0) {
            // Still inside the step. A step that warbles or sweeps has to be
            // re-driven as it goes; a plain held note has nothing to do.
            const AlertStep &s = PATTERNS[current].s[step];
            if (s.hzA && (s.hzB || s.rise) && (int32_t)(now - nextWarbleMs) >= 0) {
                nextWarbleMs = now + WARBLE_MS;
                if (s.hzB) warblePhase = !warblePhase;
                applyTone(now);
            }
            return;
        }

        step++;
        if (step < PATTERNS[current].n) { applyStep(); return; }

        // Pattern finished. Loop it while the repeat window is open - that
        // window is what alertSeconds buys, and what a button press closes.
        if (repeatUntilMs && (int32_t)(now - repeatUntilMs) < 0) {
            step = 0;
            applyStep();
            return;
        }
        current       = ALERT_NONE;
        repeatUntilMs = 0;
        silence();
    }

    // Idle: the LED is the only thing that says what the pager is doing when
    // the screen has gone to sleep, so it keeps working after the OLED does.
    switch (idleMode) {
        case IDLE_OFF:
            ledWrite(0, 0, 0);
            break;
        case IDLE_STEADY:
            ledWrite(idleR, idleG, idleB);
            break;
        case IDLE_PULSE: {
            // 40 ms on out of every 3 s: visible across a room, invisible on
            // the battery budget.
            bool on = (now % 3000) < 40;
            ledWrite(on ? idleR : 0, on ? idleG : 0, on ? idleB : 0);
            break;
        }
        case IDLE_BREATHE: {
            uint32_t phase = now % 4000;
            uint32_t half  = phase < 2000 ? phase : 4000 - phase;   // 0..2000..0
            uint16_t lvl   = (uint16_t)(half * 255 / 2000);
            ledWrite((uint8_t)((uint16_t)idleR * lvl / 255),
                     (uint8_t)((uint16_t)idleG * lvl / 255),
                     (uint8_t)((uint16_t)idleB * lvl / 255));
            break;
        }
    }
}
