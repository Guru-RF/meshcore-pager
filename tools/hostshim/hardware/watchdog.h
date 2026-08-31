// Host shim: the watchdog does not exist off the device.
#pragma once
static inline void watchdog_update() {}
static inline void watchdog_enable(uint32_t, bool) {}
