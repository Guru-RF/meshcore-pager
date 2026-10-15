// ============================================================
// RF.Guru MeshCore Pager - the MeshCore companion protocol, over USB
//
// Lets a MeshCore app use the pager as its radio. While one is attached -
// active() - the pager is in app mode and its own interface stands aside;
// main.cpp does that part. The board has no Bluetooth and no Wi-Fi, so the
// link is the USB serial port, framed exactly as upstream's
// ArduinoSerialInterface frames it:
//
//   app -> pager    '<'  len_lo len_hi  payload
//   pager -> app    '>'  len_lo len_hi  payload
//
// Command and reply layouts are upstream's examples/companion_radio/
// MyMesh.cpp; tools/selftest.cpp checks them byte by byte.
//
// The app gets what the pager already does, and no more:
//
//   - every channel message the pager decrypts, queued while no app is
//     attached and handed over when one asks
//   - sending on the channels in config.txt, signed as meshName
//   - the nodes it has heard advertise, as contacts. In RAM only: the FAT is
//     the host's while USB is attached, so nothing here writes to flash.
//   - the clock. The phone's time is real time, so it sets the pager's clock
//     like #mesh does - and lets an indoor pager start advertising.
//
// And it refuses the rest with an error rather than pretending:
//
//   - direct messages. Unicast needs a key exchange this firmware does not
//     do; public channels are the only thing an amateur band allows anyway.
//   - settings. config.txt is the configuration, and the drive it lives on
//     belongs to the host for as long as USB is attached - which, for a USB
//     link, is always. A setting the app could change until the next reboot
//     would be worse than one it cannot change at all.
//
// The port is shared with the debug log. Text on it would corrupt the
// framing, so once an app has spoken the log goes quiet until the port is
// closed - see active().
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"
#include "meshcore.h"

// The firmware protocol level the pager reports. 8 covers everything
// implemented here; higher levels promise commands it does not have.
#define CMP_FIRMWARE_VER     8

#define CMP_MAX_FRAME      176      // upstream MAX_FRAME_SIZE
#define CMP_MAX_CONTACTS    32
#define CMP_QUEUE_SIZE      16      // upstream OFFLINE_QUEUE_SIZE
#define CMP_MAX_TEXT       160      // upstream MAX_TEXT_LEN: "sender: text"

#define CMP_FRAME_IN       '<'
#define CMP_FRAME_OUT      '>'

#define CMD_APP_START               1
#define CMD_SEND_TXT_MSG            2
#define CMD_SEND_CHANNEL_TXT_MSG    3
#define CMD_GET_CONTACTS            4
#define CMD_GET_DEVICE_TIME         5
#define CMD_SET_DEVICE_TIME         6
#define CMD_SEND_SELF_ADVERT        7
#define CMD_SET_ADVERT_NAME         8
#define CMD_SYNC_NEXT_MESSAGE      10
#define CMD_RESET_PATH             13
#define CMD_REMOVE_CONTACT         15
#define CMD_REBOOT                 19
#define CMD_GET_BATT_AND_STORAGE   20
#define CMD_DEVICE_QUERY           22
#define CMD_EXPORT_PRIVATE_KEY     23
#define CMD_HAS_CONNECTION         28
#define CMD_LOGOUT                 29
#define CMD_GET_CONTACT_BY_KEY     30
#define CMD_GET_CHANNEL            31
#define CMD_SET_CHANNEL            32
#define CMD_GET_CUSTOM_VARS        40
#define CMD_GET_STATS              56
#define CMD_GET_AUTOADD_CONFIG     59

#define STATS_TYPE_CORE             0
#define STATS_TYPE_RADIO            1
#define STATS_TYPE_PACKETS          2

#define RESP_CODE_OK                   0
#define RESP_CODE_ERR                  1
#define RESP_CODE_CONTACTS_START       2
#define RESP_CODE_CONTACT              3
#define RESP_CODE_END_OF_CONTACTS      4
#define RESP_CODE_SELF_INFO            5
#define RESP_CODE_CHANNEL_MSG_RECV     8
#define RESP_CODE_CURR_TIME            9
#define RESP_CODE_NO_MORE_MESSAGES    10
#define RESP_CODE_BATT_AND_STORAGE    12
#define RESP_CODE_DEVICE_INFO         13
#define RESP_CODE_DISABLED            15
#define RESP_CODE_CHANNEL_MSG_RECV_V3 17
#define RESP_CODE_CHANNEL_INFO        18
#define RESP_CODE_CUSTOM_VARS         21
#define RESP_CODE_STATS               24
#define RESP_CODE_AUTOADD_CONFIG      25

#define PUSH_CODE_ADVERT            0x80
#define PUSH_CODE_MSG_WAITING       0x83
#define PUSH_CODE_LOG_RX_DATA       0x88

#define ERR_CODE_UNSUPPORTED_CMD    1
#define ERR_CODE_NOT_FOUND          2
#define ERR_CODE_BAD_STATE          4
#define ERR_CODE_ILLEGAL_ARG        6

// The radio's counters, for the app's statistics page.
struct CompanionRadioStats {
    int16_t  noiseFloor;            // dBm
    uint32_t rxAirMs, txAirMs;
    uint32_t recv, recvErrors, sent;
    uint32_t recvFlood, recvDirect; // by the route a frame arrived on
    uint32_t sentFlood, sentDirect;
};

// Everything the companion needs from the rest of the firmware. main.cpp
// fills it in from the real hardware; tools/selftest.cpp fills it with
// fakes, which is what lets the protocol be tested off the device.
struct CompanionHost {
    const PagerConfig *cfg;
    const MeshChannel *channels;    // the app's channel index is the index here
    int                nChannels;
    const char        *version;     // shown by the app as the firmware version
    const char        *buildDate;

    int      (*writable)();                                 // bytes the port takes without blocking
    size_t   (*write)(const uint8_t *data, size_t len);
    bool     (*transmit)(const uint8_t *frame, size_t len); // on air; false if the radio refused
    bool     (*advert)(bool flood);                         // false when there is no clock to sign with
    bool     (*position)(double *lat, double *lon);         // false when the pager has none
    uint16_t (*batteryMv)();
    void     (*radioStats)(CompanionRadioStats *s);
    void     (*reboot)();
};

namespace Companion {

// Also a full reset: forgets the queue, the contacts and any attached app.
void begin(const CompanionHost &host);

// One byte from the serial port.
void feed(uint8_t c);

// Call every loop pass. Streams the contact list a frame at a time, so a
// long list never holds the radio poll up.
void service();

// An app has sent a valid frame since the port was last opened. While this
// holds, the port carries frames and nothing else.
bool active();

// The host dropped DTR: whatever was listening has gone.
void portClosed();

// Events from the pager. `chan` is the index into host.channels.
void onChannelText(int chan, const MeshPacket &p, uint32_t ts,
                   const char *sender, const char *body);
void onAdvert(const uint8_t pub[MC_PUB_KEY_SIZE], uint8_t type, const char *name,
              bool hasLoc, double lat, double lon, uint32_t ts);
void onRawPacket(const uint8_t *raw, size_t len, int16_t rssi, int8_t snrQ);

int queued();       // channel messages waiting for the app
int contacts();     // nodes heard

}  // namespace Companion
