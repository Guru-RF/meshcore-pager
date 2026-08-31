#include "messages.h"

static PagerMessage store[MSG_MAX];
static int          nStored = 0;

void Messages::clear() { nStored = 0; }

int Messages::count() { return nStored; }

int Messages::unread() {
    int n = 0;
    for (int i = 0; i < nStored; i++) if (store[i].unread) n++;
    return n;
}

const PagerMessage *Messages::get(int i) {
    if (i < 0 || i >= nStored) return nullptr;
    return &store[i];
}

int Messages::add(const PagerMessage &m) {
    if (nStored == MSG_MAX) nStored--;              // the oldest falls off the end
    memmove(&store[1], &store[0], (size_t)nStored * sizeof(PagerMessage));
    store[0] = m;
    nStored++;
    return 0;
}

bool Messages::remove(int i) {
    if (i < 0 || i >= nStored) return false;
    memmove(&store[i], &store[i + 1], (size_t)(nStored - i - 1) * sizeof(PagerMessage));
    nStored--;
    return true;
}

void Messages::markRead(int i) {
    if (i >= 0 && i < nStored) store[i].unread = false;
}

void Messages::markAllRead() {
    for (int i = 0; i < nStored; i++) store[i].unread = false;
}
