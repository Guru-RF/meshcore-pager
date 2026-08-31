// ============================================================
// RF.Guru MeshCore Pager - the message store
//
// A flat newest-first array rather than a ring, because the interface
// deletes from the middle: a hold on the button removes the message on
// screen, and shifting 24 slots costs nothing next to the redraw it
// triggers. Index 0 is always the newest message; that is the order the
// button scrolls in.
// ============================================================

#pragma once

#include <Arduino.h>
#include "config_file.h"

#define MSG_MAX       24
#define MSG_TEXT_LEN 132
#define MSG_FROM_LEN  24

struct PagerMessage {
    char     chan[PAGER_CHAN_LEN];
    char     from[MSG_FROM_LEN];
    char     text[MSG_TEXT_LEN];
    uint32_t ts;        // sender's clock, seconds. 0 when it is not usable.
    uint32_t rxMs;      // millis() at reception - the only age that survives a
                        // pager whose own clock is still unset
    int16_t  rssi;
    int8_t   snrQ;      // SNR in quarter-dB, as MeshCore carries it
    bool     mention;   // addressed to this pager (alarms, not chirps)
    bool     unread;
};

namespace Messages {

void  clear();
int   count();
int   unread();

// Newest first: get(0) is the most recent message. NULL if i is out of range.
const PagerMessage *get(int i);

// Insert as the newest, dropping the oldest if the store is full. Always
// returns 0, the index the new message now has.
int   add(const PagerMessage &m);

// Remove one message. Returns false if i is out of range.
bool  remove(int i);

void  markRead(int i);
void  markAllRead();

}  // namespace Messages
