# meshcore-pager

RP2040 MeshCore pager firmware, Arduino/C++ on the earlephilhower core. Start
with README.md for what the product is and how it is configured, and
docs/hardware.md for the board; this file holds what those do not - the
non-obvious constraints.

## Build and verify

```console
make                 # pio run -e pico
make upload          # flash over USB (picotool, 1200 bps touch)
make test            # host protocol self-test, no hardware
```

`pio` is often not on PATH when PlatformIO was installed by its own
installer; the Makefile falls back to `~/.platformio/penv/bin/pio`. A clean
build is zero warnings - treat any warning as introduced by your change. Do
not delete `.pio/`: with no global library store it is the only copy of
RadioLib and U8g2, and removing it turns an offline build into a networked
one.

**`make test` is the real check on anything protocol-shaped.** It compiles
`src/meshcore.cpp` and `src/pagerclock.cpp` for the host against
`tools/hostshim/` and links them with `../meshcore-repeater`'s *actual*
`packet.c`, `advert.c` and `identity.c`. A framing or key-derivation mistake
shows up as a failing check rather than as a pager that sits in a pocket
hearing nothing. It needs the repeater checked out beside this tree.

The companion protocol (`src/companion.cpp`) is in `make test` too, but has
no C counterparty to link: its checks are byte offsets read off upstream's
`examples/companion_radio/MyMesh.cpp`. If upstream changes a layout, the
test will not notice - re-read that file.

`tools/selftest.cpp` contains verbatim copies of `channel_build_txt()` and
`channel_decrypt()`, which are `static` inside the repeater's `src/mesh.c`
and cannot be linked. If that file changes, re-copy them - a stale copy
agrees with nothing.

**Editing `config.txt` only takes effect if the drive is *ejected*.** Writing
it and then unmounting loses the sector; the write never reaches flash.

`/meshid.bin` is the pager's address on the mesh and cannot be regenerated.
`pio run -t uploadfs` destroys it along with `config.txt`. A firmware `.uf2`
writes only the sketch region and leaves the filesystem alone.

## Things that look like bugs but are not

- **The display is bit-banged on purpose.** GP10/GP11 are spi1 pins, but this
  core's spi1 defaults are MISO=GP12, MOSI=GP15, SS=GP13 - the button, the
  buzzer and the charge sense. `SPI1.begin()` would reconfigure all three.
  U8g2's `_2ND_HW_SPI` constructors are not compiled here anyway: they are
  guarded on `SPI_INTERFACES_COUNT`, which the earlephilhower core does not
  define.
- **`tone()` on this core uses PIO, not PWM**, so it does not contend with
  the `analogWrite()` the LEDs use. The slices would not have collided either
  (GP15 is slice 7, GP7/8/9 are slices 3 and 4), but nothing depends on that.
- **The buzzer frequencies in `src/alerts.cpp` are not tuning choices.** They
  come from `LoRa433APRSpager`'s `code.py`, which ran on this board: 330 Hz
  to 1165 Hz, 50% duty. Do not "correct" them towards a package resonance -
  an earlier draft of this firmware assumed 2.7 kHz and was wrong. Other
  facts come from the same source and are listed in docs/hardware.md: the
  display is mounted upside down (hence `U8G2_R2`) and the LED order is
  R/G/B on GP7/8/9. Its pull-down on GP13 is the one thing the schematic
  overrules.
- **Receive is polled** because no DIO line is routed on this board.
  Continuous-mode RxDone latches, so a late poll still finds the packet; what
  a late poll costs is the *second* packet of a back-to-back pair.
- **`Radio::listen()` clears every IRQ flag before arming.** `poll()` reads
  that register as its only "a frame arrived" signal, so a flag left over
  from the previous packet or transmission would be read as a fresh packet
  and hand back whatever was still in the FIFO.
- **Adverts are gated on a real clock, not on having a position.** A
  positionless advert is valid and useful; a fabricated timestamp poisons the
  mesh. That is why the pager is silent indoors - unless an app has given it
  the phone's time.
- **`config.txt` keys that are absent fall back to firmware defaults**, so a
  default change silently changes behaviour on deployed pagers.
- **The companion link refuses every settings command.** The FAT that holds
  `config.txt` belongs to the host while USB is attached, and a USB link is
  always attached, so a setting could only live in RAM until the next boot.
  An error is more honest than that. Do not "add" settings by writing to
  FatFS from the firmware. `SET_ADVERT_LATLON` is accepted because a
  position is not a setting here: it is runtime state the pager already
  learns, and the app is one more source of it.
- **The debug log stops once an app has spoken**, until DTR drops. A
  client scans for `>` to find a frame, and `logf()` lines contain
  `<sender>` - one line of text desynchronises it.
- **App mode is `Companion::active()`, and it switches the pager off.**
  While an app is attached `handleChannelText()` hands the message over and
  returns before the store, the alarm and the screen; the button only
  answers `APP MODE`, and `sendAdvert()` skips its tick. It ends on DTR dropping - and on a pulled cable, which
  the RP2040 never reports as an unplug (VBUS detect is forced on). The idle
  bus reads as a suspend instead, `tud_ready()` goes false and so does
  `Serial`. Without that, a pager unplugged mid-session would leave the desk
  never to alarm again.
- **Two boards, told apart at boot.** The original's GP13 reads HIGH
  whenever a cell is fitted (R21 to the cell, D5 to CHG#), and it has no
  VBUS sense, so USB is `tud_ready()` and "full" is the voltage. A reworked
  board has CHG# straight on GP13 and a 100k/150k VCCFILTER divider on
  GP24. `probePowerSense()` drives GP24 and lets go: a floating pad keeps
  the level, the divider does not. Do not put a pull on GP24 on a reworked
  board - beside the 150k it drags 3.0 V to about 1.5 V. `readPower()`,
  docs/hardware.md.
- **The battery reading floors at about 3.4 V.** ADC_AVDD is the LDO's
  3V3, which follows the cell down once it is within dropout, so the ratio
  stops moving. `battPercent()` ends at 3.45 V for that reason; do not
  extend the curve below it.
- **USB serial writes go nowhere while DTR is low.** That is this core's
  `SerialUSB`, and upstream's RP2040 companions run on the same core, so
  every app already asserts DTR. The flip side is that a push to an app that
  has stopped reading would block for a second; pushes are dropped whole
  instead (`writeFrame()`, `push`).

## MeshCore wire format

Everything is verified against `../meshcore-repeater` (`src/packet.h`,
`src/advert.h`, `src/mesh.c`), itself verified against upstream MeshCore. The
crypto in `lib/meshcrypto` and `lib/ed25519` is vendored from the tracker,
which vendored it from the repeater, so the three cannot drift.

- **Group messages are encrypt-then-MAC.** The repeater's
  `channel_build_txt()` encrypts in place and only then calls
  `hmac_sha256()` on the same buffer, so the call reads as though the MAC
  covers the plaintext. It covers the *ciphertext*. Getting it backwards
  yields a packet the repeater receives, matches to the right channel, and
  drops with "failed public-channel MAC".
- **AES takes the 16-byte key; the HMAC takes it zero-padded to 32; the
  on-air selector hashes the raw 16.** All three, in `Mesh::channelInit()`
  and `Mesh::buildGroupText()`. `make test` checks each against the
  repeater's derivation.
- **`Public` (no `#`) is MeshCore's built-in channel**, and the one channel
  whose key is not SHA256 of its name: it is upstream's published
  `PUBLIC_GROUP_PSK`, hard-coded in `Mesh::channelInit()`. Selector and HMAC
  derive from it exactly as for any other key. `make test` checks it against
  the repeater's own `base64_decode()` of that PSK.
- **A `fetch` must be routed FLOOD.** The repeater only treats FLOOD packets
  on `#mbox` as commands; its routed reply comes back DIRECT and has to pass
  other mailboxes untouched. `doFetch()` hard-codes FLOOD whatever
  `route=` says about adverts.
- **The body of a fetch must be exactly `fetch`.** `mbox_classify()` allows
  trailing whitespace and nothing else, so the timestamp is the only field
  that can differ between two fetches - and MeshCore dedups on a hash of the
  payload. `mcTimestamp()` therefore returns a monotonic seconds count when
  the clock is unset: with a constant timestamp, the second fetch would be
  silently dropped as a duplicate by every repeater that heard the first.
- **The dedup hash covers the payload and not the path**, which is what makes
  a re-flooded copy a duplicate. It is also why `Mesh::rememberFrame()`
  before every transmission works: the repeater's re-flood of our own packet
  comes back with a hop appended and still hashes the same.
- **The app's channel index is the position in `cfg.channels`**, after
  `ensureSubscribed()` has appended `mboxChannel` and `timeChannel`. The app
  is handed the plaintext as `sender: text`, rejoined from the split
  `decryptGroupText()` made - which is why `handlePacket()` decrypts into
  buffers sized for the whole payload, not for the message store.
- **Ed25519: the pager signs with orlp/ed25519, the repeater with
  TweetNaCl.** Both are RFC 8032, and `make test` proves it by having each
  verify the other's adverts. If either side ever stops verifying, that test
  is where it shows.

## The one-button interface

The whole state machine is in `src/main.cpp`. Four details are easy to
break:

- **The alarm stops on the way DOWN, not on release** (`onPressStart`), so
  holding the button to fetch does not mean holding it through another second
  of siren.
- **A press that silenced an alarm, or woke the screen, does not also
  scroll.** Both set a flag that `onShortPress()` consumes. Without it, the
  press meaning "what does it say?" steps past the thing you wanted to read.
- **`ui.sel == Messages::count()` is the status page**, which is how one
  press past the oldest message reaches it without needing a second gesture.
  Anything that changes the message count has to keep `sel` inside
  `0..count`.
- **A hold acts on release; its thresholds are only announced.** A hold
  passes 0.7 s (fetch or delete) on its way to 3 s (suspend), so acting at
  0.7 s would fetch or delete on the way to suspending. The click and the
  `RELEASE:` toast at each threshold are what says it registered.

## Suspend

- **Dormant stops `millis()`.** The timer runs off clk_ref, which stops with
  the oscillators, so `PagerClock` comes back behind by however long the
  pager slept. `resume()` calls `PagerClock::forget()` and the clock is
  fetched again as at boot - an advert stamped with the stale time is what
  the clock gate exists to prevent. Light suspend (a host on USB) keeps
  `millis()` running and keeps the clock.
- **The SDK wakes into its own clocks, not this core's.**
  `low_power_dormant_until_gpio_pin_state()` ends in `runtime_init_clocks()`
  (125 MHz); the core's `main()` then set F_CPU, and the radio's SPI and the
  LED PWM dividers were computed against that. `sleepUntilButton()` repeats
  the `set_sys_clock_khz()`.
- **`USB.mutex` is held across the dormant call.** The SDK `tud_deinit()`s
  before stopping the clocks and `tud_init()`s after, and the core's 1 ms
  USB task would otherwise run `tud_task()` in the middle of either. The
  SDK's `stdio_flush()` is the core's `Serial.flush()`, which sees the mutex
  is this core's own and returns rather than deadlocking.
- **Off is a constant level, so dormant cannot freeze an LED on.** PWM stops
  mid-cycle with the clocks, but `analogWrite(255)` is above the wrap and
  never goes low, and `noTone()` parks the buzzer pin low.

## Position and clock

- Position is the **centre of the Maidenhead square** of the strongest
  repeater heard in the last six hours - not the repeater's own coordinates.
  What the pager knows is "within earshot of that repeater", and a grid
  centre says exactly that. `include/grid.h`, `learnPosition()`.
- **Only zero-hop repeater adverts are measured.** RSSI is the last hop's,
  so a re-flooded advert reports the relay's strength against the far
  repeater's coordinates. A hotspot (`../meshcore-repeater`, `role=hotspot`)
  advertises as `ADV_TYPE_REPEATER` and counts like one.
- Three position sources are kept apart - the app's, `locator`'s and the
  heard one - and `choosePosition()` takes the first that is set. Folding
  them into one would leave the app's position behind after it was cleared.
- The clock comes from `#mesh` (authoritative, minute-aligned) and, only to
  bootstrap, from a signed repeater advert. `PagerClock::seedFromAdvert()`
  refuses once the clock is set, so a replayed old advert cannot drag it
  backwards. See `include/pagerclock.h`.
- A `time` line that arrives before its `date` is **held**, not guessed at.
  The whole point of asking is to stop guessing. It is held with the
  `millis()` it was heard at, because it was HH:MM:00 *then*.
- **The repeater answers `time`/`date` at the next minute boundary**, so an
  answer can take a minute and more. `serviceTimeSync()` therefore asks only
  for what is missing (`PagerClock::haveDate()`), never repeats a request
  within `TIME_RETRY_MS`, and pushes the next request out after any answer
  on `#mesh`, ours or anyone's (`PagerClock::synced()`). Each of those
  three was once a duplicate request on air.
- An app's `SET_DEVICE_TIME` is trusted like `#mesh`
  (`PagerClock::setFromApp()`): the phone keeps network time.
