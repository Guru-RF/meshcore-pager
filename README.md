# MeshCore Pager

A one-button MeshCore pager. RP2040, an RFM96W on the IARU R1 ham MeshCore
channel, a 1.3" OLED, one button, one buzzer and one RGB LED.

It listens on the public channels you name, shows everything it can decrypt,
and makes a noise when something is addressed to your call. It advertises
itself on the mesh, works out roughly where it is from the repeaters it
hears, and gets its clock from `#mesh`.

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
| Advert transmitted | One C6 tick | White |
| Something is wrong | A low, rough warble, twice | Red |

Between patterns the LED reports state, and keeps doing it after the screen
has slept - which is what makes a screen timeout acceptable on a device you
glance at:

| LED | Meaning |
|---|---|
| Red, breathing | The radio did not start |
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
| `channels` | `#mbox,#mesh` | Public channels to listen on, up to four. The key is derived from the name, so there is no key exchange. |
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
| `locator` | *(empty)* | A Maidenhead square to advertise, e.g. `JO20EU`. Setting it stops the pager learning one. |
| `gridPrecision` | `6` | `4` or `6` characters when learning a locator. |
| `frequency` `bandwidth` `sf` `cr` `preamble` | 434.890 / 62.5 / 8 / 8 / 16 | The IARU R1 ham MeshCore channel. |
| `txPower` | `17` | dBm. The SX1276 produces 2-17 and a discrete 20; anything else rounds down. |
| `debug` | `false` | Log every packet to the USB serial console. |

`mboxChannel` and `timeChannel` are added to `channels` automatically -
naming them is already a statement that you want to hear them.

`/meshid.bin` sits beside `config.txt`. It is the pager's Ed25519 identity,
which is its address on the mesh, and it cannot be regenerated: a new one is
a different node. Copy it off before anything that reformats the drive.

## How it knows where it is

It has no GPS. It listens for repeater adverts, takes the position of the
strongest repeater it has heard in the last six hours, converts that to a
Maidenhead square, and advertises the **centre of the square** - about 5 km
across at 6 characters.

That is deliberate, and not a rounding error. What the pager actually knows
is "somewhere within earshot of that repeater". A grid centre says exactly
that; copying the repeater's own coordinates would claim to be standing on
its mast.

Set `locator` if you would rather say where you are yourself.

## How it knows the time

There is no RTC either, so the clock also comes off the mesh, from two
sources:

1. **`#mesh`.** The pager sends `date`, then `time`. The repeater answers
   *at* a minute boundary, precisely so a device with no clock gets an exact
   reference - the seconds really are zero. This is the authority.
2. **A repeater's advert.** Every advert carries a signed UNIX timestamp. The
   pager uses one to bootstrap a clock that has never been set, which usually
   happens within seconds of switching on, long before `#mesh` answers.

Once `#mesh` has answered, nothing an advert says can move the clock, so a
replayed old advert cannot drag it backwards.

**Until the clock is real, the pager does not advertise.** MeshCore uses the
advert timestamp for freshness, so a made-up one poisons the mesh. Indoors
and out of range it stays silent and shows `--:--` and a yellow LED. That is
the design, not a fault.

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

## What it deliberately does not do

- **It does not forward.** A pager is a leaf. A battery-powered device in a
  pocket is a bad repeater, and re-flooding is what would flatten it.
- **It does not read private messages.** Unicast needs a key exchange this
  firmware does not do. Public channels only, which on an amateur band is the
  only thing that is allowed anyway.
- **It does not send messages.** It transmits three things: its own advert,
  `fetch` on `#mbox`, and `time`/`date` on `#mesh`. There is no keyboard.

## Licence

MIT - see [LICENSE](LICENSE). The vendored Ed25519, SHA-256 and AES-128 keep
their own permissive terms, listed there.
