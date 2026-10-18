#include "radio.h"

#include <RadioLib.h>
#include <SPI.h>
#include <hardware/watchdog.h>

#include "pins.h"

// MeshCore's sync word is RadioLib's default 0x12, expanded to 0x1424 by the
// SX127x driver. Nothing else about the profile is shared with anything.
static const uint8_t LORA_SYNC_WORD = 0x12;

// The RFM96W has no heatsink and a coin-cell-sized supply behind it; 140 mA
// is the same limit the tracker runs its SX1276 at.
static const float LORA_OCP_MA = 140.0f;

static const uint32_t LORA_SPI_HZ = 2000000;

static SPISettings loraSpi(LORA_SPI_HZ, MSBFIRST, SPI_MODE0);

// No interrupt and no busy line: both are RADIOLIB_NC because neither is
// routed. RadioLib copes as long as nothing asks it to wait on one, which is
// why send() and poll() below drive the IRQ register by hand.
static Module modSx1276(PIN_LORA_CS, RADIOLIB_NC, PIN_LORA_RST, RADIOLIB_NC, SPI, loraSpi);
static SX1276 radio(&modSx1276);

static bool     started = false;
static uint32_t nRx = 0, nTx = 0, nRxErr = 0;
static uint32_t rxAir = 0, txAir = 0;
static PagerConfig conf;

bool Radio::ok() { return started; }

uint32_t Radio::rxCount() { return nRx; }
uint32_t Radio::txCount() { return nTx; }
uint32_t Radio::rxErrors() { return nRxErr; }
uint32_t Radio::rxAirMs() { return rxAir; }
uint32_t Radio::txAirMs() { return txAir; }

int16_t Radio::noiseFloor() {
    if (!started) return 0;
    // In LoRa mode this reads RegRssiValue and nothing else - no change of
    // mode, so receive carries on. A packet arriving mid-read raises one
    // sample; the minimum is the floor under it.
    float lowest = 0;
    for (int i = 0; i < 4; i++) {
        float r = radio.getRSSI(false);
        if (i == 0 || r < lowest) lowest = r;
        delayMicroseconds(500);
    }
    return (int16_t)lroundf(lowest);
}

bool Radio::begin(const PagerConfig &cfg, char *err, size_t errLen) {
    conf = cfg;

    pinMode(PIN_LORA_CS, OUTPUT);
    digitalWrite(PIN_LORA_CS, HIGH);
    pinMode(PIN_LORA_RST, OUTPUT);
    digitalWrite(PIN_LORA_RST, HIGH);

    // The core's SPI object defaults to other pins, and Module::begin() calls
    // SPI.begin() itself - so the pins have to be chosen before RadioLib gets
    // there or the first register read talks to nothing.
    SPI.setRX(PIN_LORA_MISO);
    SPI.setTX(PIN_LORA_MOSI);
    SPI.setSCK(PIN_LORA_SCK);
    SPI.begin();

    // RadioLib's PA_BOOST path produces 2..17 dBm plus the discrete
    // high-power point at 20; 18 and 19 are rejected outright and begin()
    // would fail rather than clamp. Round down to what the chip can make.
    int8_t power = (cfg.txPower >= 20) ? 20 : (int8_t)constrain(cfg.txPower, 2, 17);

    int16_t state = radio.begin(cfg.frequency, cfg.bandwidth, (uint8_t)cfg.sf,
                                (uint8_t)cfg.cr, LORA_SYNC_WORD, power,
                                (uint16_t)cfg.preamble);
    if (state != RADIOLIB_ERR_NONE) {
        snprintf(err, errLen, "SX1276 begin failed (%d) on CS=GP%d RST=GP%d",
                 state, PIN_LORA_CS, PIN_LORA_RST);
        return false;
    }

    radio.setCRC(true);
    radio.explicitHeader();
    // Low data rate optimisation belongs above a 16 ms symbol and is wrong
    // below it. SF8 at 62.5 kHz is 4.1 ms, so the default profile does not
    // want it - but frequency, SF and bandwidth are all configurable, so
    // decide from the numbers rather than hard-coding the answer.
    float symbolMs = (float)(1UL << cfg.sf) / cfg.bandwidth;
    radio.forceLDRO(symbolMs >= 16.0f);
    radio.setCurrentLimit(LORA_OCP_MA);

    started = true;
    Radio::listen();
    return true;
}

void Radio::listen() {
    if (!started) return;
    radio.standby();
    // Clear anything latched from the last packet or transmission before
    // arming: poll() reads this register as its only "a frame arrived"
    // signal, so a stale RxDone would be read as a fresh packet and hand
    // back whatever is still in the FIFO.
    radio.clearIrqFlags(0xFFFFFFFFu);
    radio.startReceive();
}

void Radio::sleep() {
    if (!started) return;
    radio.sleep();
}

uint32_t Radio::timeOnAirMs(size_t len) {
    if (!started) return 0;
    return (uint32_t)(radio.getTimeOnAir(len) / 1000);
}

bool Radio::send(const uint8_t *data, size_t len) {
    if (!started) return false;

    watchdog_update();
    radio.standby();
    radio.clearIrqFlags(0xFFFFFFFFu);

    int16_t state = radio.startTransmit(const_cast<uint8_t *>(data), len);
    if (state != RADIOLIB_ERR_NONE) {
        radio.finishTransmit();
        Radio::listen();
        return false;
    }

    // Twice the nominal time on air, plus a second. At SF8/62.5 kHz a full
    // frame is about a second, well inside the watchdog - but SF and
    // bandwidth are configurable, and at SF12 this is the loop that keeps
    // the watchdog fed through a multi-second transmission.
    uint32_t budgetMs = timeOnAirMs(len) * 2 + 1000;
    uint32_t start    = millis();
    bool     done     = false;

    while ((millis() - start) < budgetMs) {
        watchdog_update();
        if (radio.getIrqFlags() & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_TX_DONE) {
            done = true;
            break;
        }
        delay(2);
    }

    radio.finishTransmit();
    watchdog_update();
    Radio::listen();
    if (done) {
        nTx++;
        txAir += timeOnAirMs(len);
    }
    return done;
}

int Radio::poll(uint8_t *buf, size_t bufSz, int16_t *rssi, int8_t *snrQ) {
    if (!started) return 0;

    uint32_t flags = radio.getIrqFlags();
    if (!(flags & RADIOLIB_SX127X_CLEAR_IRQ_FLAG_RX_DONE)) return 0;

    size_t len = radio.getPacketLength();       // must be read before readData
    if (len == 0 || len > bufSz) {
        Radio::listen();
        nRxErr++;
        return -1;
    }

    int16_t state = radio.readData(buf, len);

    // Read the signal figures before re-arming: startReceive() moves the chip
    // out of the state these registers describe.
    if (rssi) *rssi = (int16_t)lroundf(radio.getRSSI());
    if (snrQ) *snrQ = (int8_t)constrain(lroundf(radio.getSNR() * 4.0f), -128, 127);

    Radio::listen();

    rxAir += timeOnAirMs(len);                  // on air whether or not the CRC held
    if (state != RADIOLIB_ERR_NONE) {           // CRC mismatch and friends
        nRxErr++;
        return -1;
    }
    nRx++;
    return (int)len;
}

void Radio::describe(char *buf, size_t len) {
    if (!started) {
        snprintf(buf, len, "radio not initialised");
        return;
    }
    snprintf(buf, len, "SX1276 %.3f MHz BW%.1f SF%d CR4:%d pre%d %ddBm",
             (double)conf.frequency, (double)conf.bandwidth, conf.sf, conf.cr,
             conf.preamble, conf.txPower);
}
