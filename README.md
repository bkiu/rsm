# Really Simple Messenger (rsm)

<img style="max-width: 600px" alt="cardputer" src="https://github.com/user-attachments/assets/27b066a2-0039-4fbe-82d0-dda5cd8ffe0b" />

The simplest Meshtastic messenger for the **M5Stack Cardputer ADV + Cap LoRa-1262**.

On the device you do one thing: **message people**. Pick a node or channel, type, press Enter. The text is big,
the keys are few, and every message tells you plainly whether it got through.

Everything else, like channels, keys, radio settings, quick messages, GPS sharing and backups, is managed
**over USB serial** from your computer, where a real keyboard and screen make it easy. You set it up once at your
desk, then the Cardputer stays a pocket messenger.

rsm speaks the standard Meshtastic over-the-air protocol (v2.5+ encrypted direct messages, the default `LongFast`
channel), so it talks to stock Meshtastic radios and the phone apps. It needs no phone, Bluetooth or WiFi.

- [On the device](#on-the-device)
- [Over USB serial](#over-usb-serial)
- [Installing](#installing)
- [How it works](#how-it-works)

---

## On the device

### First boot

Pick your LoRa region from the list (the same list as the Meshtastic app). That's the only setup the device asks
for. You're on the public `LongFast` channel and can start messaging.

### The node list

The home screen lists your channels first (rows starting with `#`), then every node you've heard, newest first.

- A **green padlock** means you can DM that node. A **grey** one means its key hasn't arrived yet, but you can
  still write to it (see below).
- The right edge shows when you last heard the node, and a red badge counts unread messages.
- Long names shrink to a smaller font rather than get cut off.

### Keys

| Where | Key | Does |
|---|---|---|
| Node list | `;` `.` | move up / down |
| | **Enter** | open the chat |
| | **Fn + letter/digit** | send a [quick message](#quick-messages) |
| | **F** | text size (3 steps) |
| | **I** | info and help |
| | **K** | ask the selected node for its key |
| | **R** | change region |
| Chat | type, **Enter** | send |
| | **Esc** or **Fn+,** | back to the list |
| | **Fn+;** **Fn+.** | scroll |
| | **Tab** | retry a failed message, or ask for the key again |
| Anywhere | side button | back to the node list |

The screen dims after a minute and turns off after five. The first key press only wakes it, so it never types or
sends anything by accident.

### Messages never fail silently

Under each message you send, its status reads:

`getting key...` → `sending...` → `relayed` (a nearby radio passed it on) → `delivered` (the recipient confirmed)

or `FAILED - <reason>` in red.

You never have to manage encryption keys. If you message a node whose key you don't have yet, rsm asks for it,
holds the message, and sends it the moment the key arrives, even hours later. If the other side has a stale key
for you, rsm swaps keys and resends on its own. If something does fail, **Tab** tries again.

### Quick messages

Press **Fn + a key** on the node list to send a canned message instantly, such as `garage toggle` to a channel your
Home Assistant listens on. A pop-up confirms where it went, and the message appears in that chat with its delivery
status. There is no confirmation step, so pick keys you won't hit by accident. You set these up over
[USB serial](#quick-messages-1).

---

## Over USB serial

Plug the Cardputer into your computer and open its console:

```sh
./monitor.sh            # finds the port and connects; Ctrl+C to quit
```

(Or any serial terminal at 115200 baud, e.g. `screen /dev/ttyACM0 115200`.) Type `help` for every command.
Changes take effect immediately and are saved to the device.

### Your name

```
name Alice's Cardputer
short ALCE
info                       # everything at a glance: settings, radio, GPS
```

### Quick messages

```
quick g #HA garage toggle     # Fn+G sends "garage toggle" to channel HA
quick 1 !a1b2c3d4 ping        # Fn+1 sends a DM (node by !id or name)
quick                         # list them (they're also on the device's I screen)
quick g del
```

Keys are letters or digits. You can set up to 12 quick messages of up to 100 characters each.

### Private channels

Up to 7 private channels alongside the public one. Each gets its own `#` row and chat on the device.

```
chadd Family              # create one with a random key, and print the key
chadd Hiking 8w2b...=     # join an existing one: exact name + base64 key from the app
channels                  # list names and keys, to set up other devices
chset Family share on     # share GPS position on this channel only
chset Family key random   # rotate the key
chdel Hiking
```

On phones and other radios, add the channel **as a secondary channel** with exactly the same name and key.
Leave everyone's primary on LongFast: the primary channel's name sets the radio frequency.

Direct messages don't depend on channels. rsm announces its key on every channel, so anyone sharing any channel
with you can DM you.

### GPS and position sharing

The Cap's GPS is on by default. The dot in the device's top bar is green with a fix, amber while searching, and
grey when the module is silent. With a fix, a chat shows how far away the other node is.

Your position is **not broadcast unless you turn it on**:

```
share on           # exact position on the public channel, every 15 min
share on 16        # coarsened to ~350 m (13 ~ 3 km)
interval 30        # minutes between broadcasts
share off
gps off            # power the module down
```

### Backup and restore

```sh
./backup.sh export                         # -> backups/rsm-<date>.txt
./backup.sh restore backups/rsm-<date>.txt
```

A backup is a plain-text list of serial commands covering your key pair, every setting, your channels and their
keys, every known node and its key, and your quick messages. Message history is included as comments to read,
but it isn't restored. The file holds private keys, so it's saved readable only by you, and `backups/` is
git-ignored. Close `monitor.sh` first, because only one program can use the port at a time.

Restore onto **the same device** (after a wipe or `factoryreset`). The node number comes from the chip, so another
Cardputer would end up with your key under a different number.

### Everything else

```
region [CODE]            preset [NAME]          slot <n> (0 = auto)
channel <name>|default   psk <base64>|default|none|random      (the primary channel)
power <dBm>              hops <1-7>             relay on|off      font <0-2>
nodes                    msg <!id|name|ch|#chan> <text>          keyreq <!id|name>
export                   sync                   bat (battery voltage and charging trend)
log on|off               print every received packet, for troubleshooting
clearmsgs | clearnodes   factoryreset           reboot
```

---

## Installing

You need [PlatformIO](https://platformio.org/) (on NixOS: `nix shell nixpkgs#platformio`).

```sh
pio run -t upload       # build and flash over USB-C
```

If upload can't connect, hold **G0** while plugging in USB to enter download mode.

---

## How it works

### Delivery and keys

Current Meshtastic firmware rejects direct messages that aren't encrypted with the recipient's public key, and you
only learn a key from that node's NodeInfo broadcast. Stock nodes also answer key requests sparingly. That's why
DMs on other clients often just fail. rsm handles it like this:

1. It learns keys from every NodeInfo it hears and saves them to flash.
2. When it hears a node it knows nothing about (usually one several hops away), it asks for its name and key, at
   most once every 30 s.
3. A DM to a node without a key is held while rsm asks for the key (3 tries, 45 s apart). It sends automatically
   when the key arrives from a reply or any later broadcast.
4. If the recipient can't decrypt (it lacks our key, or one key is stale), rsm swaps keys both ways and resends
   once.
5. When someone DMs us and we lack *their* key, rsm replies `PKI_UNKNOWN_PUBKEY`, which makes stock firmware send
   its NodeInfo, and asks for the key itself.

On the mesh it acts as a normal `CLIENT` node. It relays other nodes' packets with the firmware's SNR-weighted
delay, skips the relay if someone else relays first, and announces itself at boot and every 3 hours.

### Code

| File | Role |
|---|---|
| `src/ui.*`, `src/input.*` | screens and keyboard (Fn arrows, key repeat) |
| `src/cli.*` | the USB serial console |
| `src/mesh.*` | framing, de-dup, relaying, ACK/NAK, NodeInfo, the outgoing message state machine and key exchange |
| `src/crypto.*` | AES-CTR channel crypto, X25519 + SHA-256 + AES-CCM for DMs (byte-compatible with the firmware) |
| `src/radio.*` | SX1262 via RadioLib, the Cap's RF enable, TX queue, listen-before-talk |
| `src/nodedb.*`, `src/messages.*` | known nodes and keys, and the last 120 messages, saved to flash |
| `src/settings.*` | settings and quick messages, saved to NVS |
| `src/gps.*` | NMEA parsing with baud auto-detect, distances |
| `src/regions.*` | region and preset tables mirrored from the firmware |
| `lib/meshtastic_protobufs` | nanopb output copied from `meshtastic/firmware` |

It's single-threaded: `loop()` polls the radio, runs the mesh timers, reads the keyboard, and redraws when
something changed.

Host-side protocol tests (run `pio run` once first to fetch libraries):

```sh
nix-shell -p gcc mbedtls --run test/host/run.sh
```

They check the channel hash, the frequency-slot math, and decryption of a real direct message captured from the
official firmware.

### Status

rsm runs on real hardware. It hears nodes, exchanges keys, and its direct and channel messages get through to stock
radios. GPS position sharing has had the least real-world testing.

Ideas that fit the "simple on the device" rule:

- Real clock time on messages (from GPS)
- Filter the node list by typing
- Emoji and non-Latin text (the fonts cover ASCII and Latin only)

### Limitations

- Public keys are trusted on first use and accept updates, so a node that reflashes is picked up automatically.
- NodeInfo broadcasts aren't signed. Stock firmware accepts unsigned nodes that have never signed.
- Traceroute and telemetry requests aren't answered (they're still relayed). Position requests are answered only
  when sharing is on.
- Long names are stored up to 24 characters.

## License

GPL-3.0 (see [LICENSE](LICENSE)). The protobuf definitions and several protocol details come from
[meshtastic/firmware](https://github.com/meshtastic/firmware) (GPL-3.0). Hardware details were cross-checked
against [d4rkmen/plai](https://github.com/d4rkmen/plai).
