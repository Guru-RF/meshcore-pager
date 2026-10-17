# MeshCore Pager

A one-button MeshCore pager. RP2040, an RFM96W on the IARU R1 ham MeshCore
channel, a 1.3" OLED, one button, one buzzer and one RGB LED.

It listens on the public channels you name, shows everything it can decrypt,
and makes a noise when something is addressed to your call. It advertises
itself on the mesh, works out roughly where it is from the repeaters it
hears, and gets its clock from `#mesh`.

Plug it into something running a MeshCore app and the app can use it as its
radio too - read, send, see who is around - while the pager carries on
doing all of the above. See [With a MeshCore app](#with-a-meshcore-app).

Hardware: [docs/hardware.md](docs/hardware.md). Wire formats are shared with
[meshcore-repeater](../meshcore-repeater) and verified against it by
`make test`.

## The interface

One button. What it does depends on what is on screen.

| Gesture | Effect |
|---|---|
| Press, while the alarm is sounding | Silence it. You stay on the message that caused it. |
| Press, while the screen is asleep | Wake the screen, and nothing else. |
| Press | Next message. One press past the oldest lands on the status page, then it wraps. |
| Hold (0.7 s), more than `fetchCooldown` since the last fetch | Fetch your mail from `#mbox`. |
| Hold (0.7 s), within `fetchCooldown` of the last fetch | Delete the message on screen. |
| Press or hold, while an app is attached | Nothing but `APP MODE - USE THE APP` - see [With a MeshCore app](#with-a-meshcore-app). |

The bottom right of the screen always says which of the two a hold will do -
`HOLD:FETCH` or `HOLD:DEL` - so you never have to remember how long ago you
last fetched. The default `fetchCooldown` is 600 seconds: fetch, then for ten
minutes the button is a delete key, then it is a fetch key again.

There is no separate acknowledge. Deleting a message *is* acknowledging it,
and silencing the alarm marks it read. MeshCore has no acknowledgement for a
public channel message, so nothing is sent back over the air.

## What it sounds and looks like

The alarm is the rising two-tone warble from
[LoRa433APRSpager](https://github.com/Guru-RF/LoRa433APRSpager), the previous
firmware for this board: two tones 250 Hz apart, alternating every 20 ms,
sweeping together from 330/580 Hz up to 915/1165 Hz over 1.6 seconds, then a
third of a second of silence so it can be answered rather than just endured.
Everything else is built from the same proven range.

| Event | Sound | LED |
|---|---|---|
| Message for you | The warble, repeating for `alertSeconds` | Strobes white, red, white, red |
| Other traffic | 440 then 880 Hz, short | Blue flash |
| Fetch sent | A4 sweeping up to C6 | Blue to cyan |
| Mail came back | C E G C, a major arpeggio - nothing else is in a major key | Green |
| Mailbox empty | Two falling notes, G4 then E4 | Amber |
| Message deleted | 880 Hz falling to 330 - the fetch sound backwards | Fading red |
| Button registered | One 880 Hz click | Green blink |
| Advert transmitted | One C6 tick (none in app mode) | White |
| Something is wrong | A low, rough warble, twice | Red |

Between patterns the LED reports state, and keeps doing it after the screen
has slept - which is what makes a screen timeout acceptable on a device you
glance at:

| LED | Meaning |
|---|---|
| Red, breathing | The radio did not start |
| Purple blip | App mode: a MeshCore app is attached. Dark once the screen sleeps. |
| Blue, breathing | A fetch is out, waiting for the reply |
| Orange blip | Unread messages |
| Amber, breathing | Charging |
| Yellow blip | No clock yet, so it is not advertising |
| Green blip | Idle and healthy |

## Setting it up

Plug it into a computer. It appears as a small drive with `config.txt` on it.
Edit that, **eject the drive** - an unmount is not enough, the write never
reaches flash - and the pager reboots with the new settings.

```
call=OR7F-12
channels=#mbox,#mesh
alertOn=mention
alertSeconds=30
```

That is the whole minimum. Everything else has a default; keys you leave out
keep it, so an old `config.txt` on a deployed pager still boots.

### Keys

| Key | Default | What it does |
|---|---|---|
| `call` | `NOCALL` | Your call. `@OR7F` in a message sets off the alarm; the SSID is ignored when matching, so `@OR7F` reaches `OR7F-12`. |
| `meshName` | same as `call` | Advertised name, and the prefix on anything the pager sends. |
| `nodeType` | `chat` | `chat`, `repeater`, `room` or `sensor`. |
| `route` | `flood` | Route type for adverts. A pager is a leaf, so its advert has to travel on its own legs. |
| `channels` | `#mbox,#mesh` | Public channels to listen on, up to eight. The key is derived from the name, so there is no key exchange. `Public`, without a `#`, is MeshCore's built-in channel - the one every app starts with - whose key is published rather than derived. `#public` is a different channel. |
| `mboxChannel` | `#mbox` | Where `fetch` is sent and mail comes back. |
| `timeChannel` | `#mesh` | Where repeaters answer `time` and `date`. |
| `alertOn` | `mention` | `mention` alarms only for traffic addressed to you and chirps for the rest. `all` alarms for everything. |
| `alertSeconds` | `30` | How long the alarm repeats if nobody presses the button. `0` is a single burst. |
| `tonePitch` | `100` | Shifts every tone, in percent. 100 is the range proven on this board. |
| `buzzer` | `true` | `false` leaves the LED patterns and silences the sound. |
| `ledBrightness` | `200` | 0-255. |
| `fetchCooldown` | `600` | Seconds after a fetch during which a hold deletes instead. |
| `advertInterval` | `3600` | Seconds between self-adverts. |
| `timeSyncInterval` | `21600` | Seconds between clock resyncs once the clock is set. |
| `screenTimeout` | `30` | Seconds of no button before the display sleeps. `0` never sleeps. |
| `locator` | *(empty)* | A Maidenhead square to advertise, e.g. `JO20EU`. Wins over what the pager hears; only a position from an app wins over it. |
| `gridPrecision` | `6` | `4` or `6` characters when learning a locator. |
| `frequency` `bandwidth` `sf` `cr` `preamble` | 434.890 / 62.5 / 8 / 8 / 16 | The IARU R1 ham MeshCore channel. |
| `txPower` | `17` | dBm. The SX1276 produces 2-17 and a discrete 20; anything else rounds down. |
| `debug` | `false` | Log every packet to the USB serial console. |

`mboxChannel` and `timeChannel` are added to `channels` automatically -
naming them is already a statement that you want to hear them.

`/meshid.bin` sits beside `config.txt`. It is the pager's Ed25519 identity,
which is its address on the mesh, and it cannot be regenerated: a new one is
a different node. Copy it off before anything that reformats the drive.

## With a MeshCore app

The pager speaks MeshCore's companion protocol on its USB serial port, the
same port the drive arrives with. Anything that can talk to a MeshCore
companion radio over USB serial can attach to it - meshcore-cli, the
`meshcore` Python library, or an app's serial/USB connection. There is no
Bluetooth and no Wi-Fi: the board has neither radio.

What the app gets:

| | |
|---|---|
| Messages | Everything the pager decrypts, on every channel in `channels`. The last 16 are kept while no app is attached and handed over when one connects. |
| Sending | On any of those channels, as `meshName`, exactly as another MeshCore client would. |
| Contacts | Every node heard advertising since boot, with its position. Kept in RAM, so a reboot starts the list again. |
| The clock | An app can set the pager's clock from the phone - which also lets a pager indoors, out of earshot of `#mesh`, start advertising. |
| Advert | The app's advert button, flood or zero-hop. Refused until the clock is set, like the schedule's. |
| Packet log | Every packet the radio hears, raw, live. |
| Statistics | Battery, uptime, noise floor, last RSSI/SNR, airtime and packet counts. |
| Telemetry | Its own: battery voltage and the RP2040's temperature. |
| Position | Type one into the app and the pager advertises it, as given, until it reboots or the app clears it. |

What it refuses, with an error rather than a pretend success:

- **Direct messages.** For the reason in
  [What it deliberately does not do](#what-it-deliberately-does-not-do).
- **Settings** - name, radio, channels. `config.txt` is the
  configuration, and the drive it lives on belongs to the computer or phone
  for as long as USB is attached, which for a USB link is always. Edit
  `config.txt` instead; the app shows the channels from it in the order
  they are listed there, with `mboxChannel` and `timeChannel` appended if
  they were missing.
- **Exporting the private key.** `/meshid.bin` is on the drive already.

**While an app is attached the pager is in app mode**, from the moment the
app first speaks until it closes the port or the cable comes out. The app is
the interface then, and the pager steps aside:

- Messages go to the app and nowhere else. The pager does not store them,
  alarm or chirp, so nothing heard in app mode is on the pager afterwards.
  What it had stored before is left alone, unread ones included.
- The screen says `APP MODE`, and the LED blips purple. When the screen
  sleeps the LED goes dark with it - a desk radio has no reason to blink.
- The button does nothing but say `APP MODE - USE THE APP`. No scrolling, no
  fetch, no delete. An alarm that was sounding when the app attached stops,
  and a fetch that was out is forgotten - its reply reaches the app on
  `#mbox`.
- What keeps the pager on the mesh carries on: adverts, the clock from
  `#mesh`, and learning its position. Adverts go out without their tick;
  the only sound left is the button's answer.

When the app lets go the screen says `PAGER MODE` and everything is as it
was. With `debug=true` the serial log is quiet for the same span - text in
the middle of the app's frames would break them.

## How it knows where it is

It has no GPS. It listens for repeater adverts - a hotspot advertises as a
repeater, so those count too - takes the position of the strongest one it
has heard in the last six hours, converts that to a Maidenhead square, and
advertises the **centre of the square** - about 5 km across at 6
characters.

Only adverts heard straight from the repeater count. One that another
repeater has passed on arrives at the strength of the repeater that passed
it on, which may be next door while the advert's sender is 100 km away.

The grid centre is deliberate, and not a rounding error. What the pager
actually knows is "somewhere within earshot of that repeater". A grid
centre says exactly that; copying the repeater's own coordinates would
claim to be standing on its mast.

Two things outrank what it hears:

1. **A position typed into a MeshCore app** on USB. It is advertised as
   given, not snapped to a square - whoever typed it knows where the pager
   is. It lasts until the pager reboots, or until the app clears it, which
   hands the position back to the next source down.
2. **`locator` in `config.txt`**, if you would rather say where you are
   yourself.

## How it knows the time

There is no RTC either, so the clock also comes off the mesh, from two
sources - or from a phone, when one is attached:

1. **`#mesh`.** The pager sends `date`, then `time`. The repeater answers
   *at* a minute boundary, precisely so a device with no clock gets an exact
   reference - the seconds really are zero. This is the authority.
2. **A repeater's advert.** Every advert carries a signed UNIX timestamp. The
   pager uses one to bootstrap a clock that has never been set, which usually
   happens within seconds of switching on, long before `#mesh` answers.
3. **A MeshCore app on USB.** An app can set the radio's clock from the
   phone's. A phone keeps network time, so this is trusted as `#mesh` is: it
   sets the clock outright, even one that is already running.

Once `#mesh` or an app has set it, nothing an advert says can move the
clock, so a replayed old advert cannot drag it backwards.

**Until the clock is real, the pager does not advertise.** MeshCore uses the
advert timestamp for freshness, so a made-up one poisons the mesh. Indoors
and out of range it stays silent and shows `--:--` and a yellow LED - until
an app gives it the time. That is the design, not a fault.

## Building

```console
make            # build the firmware
make upload     # flash it over USB
make test       # host protocol self-test, no hardware needed
make monitor    # serial console
```

`make test` compiles the pager's own protocol sources against
`../meshcore-repeater`'s actual `packet.c`, `advert.c` and `identity.c` and
makes the two talk to each other: channel keys, group text in both
directions, the dedup hash, packet framing and adverts. The two sides do not
even share an Ed25519 implementation - the pager uses orlp/ed25519, the
repeater TweetNaCl - so the advert tests double as proof that the signatures
really are interoperable RFC 8032 and not merely self-consistent.

It also plays an app against the companion protocol, byte for byte, and
hands what that puts on the air to the repeater's code to decrypt.

## What it deliberately does not do

- **It does not forward.** A pager is a leaf. A battery-powered device in a
  pocket is a bad repeater, and re-flooding is what would flatten it.
- **It does not read private messages.** Unicast needs a key exchange this
  firmware does not do. Public channels only, which on an amateur band is the
  only thing that is allowed anyway.
- **It does not send messages of its own.** It transmits three things: its
  own advert, `fetch` on `#mbox`, and `time`/`date` on `#mesh`. There is no
  keyboard - except an attached app's, which can send on the channels in
  `config.txt`.

## Licence

MIT - see [LICENSE](LICENSE). The vendored Ed25519, SHA-256 and AES-128 keep
their own permissive terms, listed there.
