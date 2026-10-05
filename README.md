# Really Simple Messenger (rsm)

A deliberately small Meshtastic messenger for the **M5Stack Cardputer ADV + Cap LoRa-1262**:
see the nodes around you, pick one, type, send. Big text, and direct messages that never fail silently.

It speaks the normal Meshtastic over-the-air protocol (v2.5+ PKI direct messages, default `LongFast`
channel), so it talks to stock Meshtastic radios and phones. It's standalone — no phone, BLE, or WiFi.

## Why direct messages "just fail" elsewhere, and what this does about it

Current Meshtastic firmware **rejects direct messages that aren't encrypted with public keys** ("Rejecting
legacy DM"). You can only encrypt for a node once you have its public key, which you learn from its NodeInfo
broadcast. If you don't have the key yet, a DM can't be delivered at all.

Stock nodes also rate-limit answering key requests: roughly one reply per requester every 12 h, and none if
they sent their NodeInfo in the last ~30 min. So one request isn't enough.

What rsm does:

1. **Learns keys passively** from every NodeInfo it hears, and **saves them to flash** so they survive
   reboots.
2. When you message a node without a key, the message shows **"getting key..."**. The device sends that
   node our NodeInfo with *want_response* (up to 3 tries, 45 s apart) and holds the message.
3. The moment the key arrives, from a reply or from any later broadcast, the queued message is
   **sent automatically**.
4. If the node never answers, the message says **"no key yet – no reply; auto-sends when key arrives"**
   (not a silent failure). Press **Tab** in the chat to ask again.
5. If the recipient NAKs because *it* lacks our key, or one side has a stale key, it swaps keys both ways
   and resends once automatically.
6. Every outgoing message shows its state: `getting key...` → `sending...` → `relayed` (a neighbour
   rebroadcast it) → `delivered` (recipient ACKed), or `FAILED - <reason>`.

In the other direction, when someone DMs us and we don't have *their* key, we NAK with
`PKI_UNKNOWN_PUBKEY` (which makes stock firmware send its NodeInfo) and request their key ourselves.

## Using it

On first boot you pick your LoRa region (same list as the Meshtastic app). After that:

| Screen | Keys |
|---|---|
| Node list | `;` / `.` move, **Enter** open chat, **K** request node's key, **I** info and full key help, **F** text size, **R** region, **Fn+letter/digit** send a quick message |
| Chat | type and **Enter** to send, **Tab** retry failed message / re-request key, **Fn+;** / **Fn+.** scroll, **Esc** or **Fn+,** back (**Fn+`** types a backtick) |
| Anywhere | side button (G0) returns to the node list |

The top row of the node list is the primary channel (`# LongFast`). A green padlock means we have that
node's key; a hollow grey one means we don't yet. The screen dims after 1 min and turns off after 5; the
first key press only wakes it.

Text size has three steps (**F** or `font 0|1|2`); the default is DejaVu 18 px, and 2 is 24 px.

### Quick messages

Canned messages you can fire from the node list with **Fn+key**. This is handy for triggering automations, such as
Home Assistant listening on a channel. Set them up over serial:

```
quick g #HA garage toggle     # Fn+G sends "garage toggle" to channel HA
quick 1 !a1b2c3d4 ping        # Fn+1 DMs a node (by !id or name)
quick                         # list them (the I screen lists them too)
quick g del
```

A message sends as soon as you press the key, with no confirmation. A toast says where it went, and the message
shows up in that chat with its usual delivery status. Up to 12 quick messages, 100 characters each. Removing a
channel also removes its quick messages.

### Private channels

Up to 8 channels, like stock Meshtastic. Channel 0 is the primary (the public `LongFast` by default), which
also decides the radio frequency. Channels 1–7 are extra (private) channels. Each gets its own `#` row at the
top of the node list and its own chat. Set them up over serial:

```
chadd Family              # new channel with a random 256-bit key; prints the key
chadd Hiking 8w2b...=     # join an existing channel: exact name + base64 key from the app
channels                  # list names and keys (to set up other devices)
chset Family share on     # share GPS position on this channel only
chset 2 key random        # rotate a key (by number or name)
chdel Hiking
msg #Family on my way
```

On other devices, add the channel **as a secondary channel** with exactly the same name and key. Their
primary stays LongFast, so everyone stays on the same frequency. Changing the *primary* channel's name moves
the frequency (as in stock firmware), so leave the primary alone unless your whole group uses it.

The device announces its name and key on every channel, so members of a private channel can message it
directly even if they don't share the public one. Direct messages don't depend on channels; they're
encrypted with the recipient's key.

### GPS

The Cap LoRa-1262's GPS is on by default. The dot in the top bar is **green** with a fix, **amber** while
searching, and **grey** if the module isn't sending data. The first fix outdoors can take a few minutes.
The **I** screen shows your coordinates and satellite count. When both you and a node have a position,
the chat header shows the distance to it. Positions other nodes broadcast are saved with the node list.

**Your position is not broadcast unless you turn it on**, the same default as stock Meshtastic on the
public channel. Sharing is per channel (`chset <channel> share on`). For the primary:

```
share on          # exact position, every 15 min
share on 16       # coarsened to ~350 m (13 ~ 3 km)
interval 30       # minutes between broadcasts
share off
gps off           # power the module down
```

With sharing on, it also answers position requests sent directly to it.

### Serial console

Run `./monitor.sh` (or `pio device monitor`, `screen /dev/ttyACM0 115200`, …) and type `help`:

```
info                     show settings and radio status
name <long name>         short <name>
region [CODE]            preset [NAME]          slot <n> (0 = auto)
channels                 chadd <name> [key]     chset <n> name|key|share <v>   chdel <n>
channel <name>|default   psk <base64>|default|none|random
power <dBm>              hops <1-7>             relay on|off      font <0-2>
gps on|off               share on|off [bits]    interval <minutes>
export                   identity <key>         nodeadd ...       sync (write everything to flash now)
nodes                    msg <!id|name|ch|#chan> <text>                 keyreq <!id|name>
quick [<key> <!id|name|ch|#chan> <text> | <key> del]
clearmsgs | clearnodes   factoryreset           reboot
bat                      battery voltage trend  log on|off (print every received packet)
```

### Backup and restore

```sh
./backup.sh export                 # -> backups/rsm-<date>.txt
./backup.sh restore backups/rsm-20261005-133950.txt
```

The backup is plain text made of serial commands: our key pair (`identity`), every setting, channels with their
keys, every known node with its public key (`nodeadd`), and quick messages. It also includes the message history
as comments for reading; history isn't restored. It holds private keys, so it is saved readable only by you, and
`backups/` is git-ignored. Close `monitor.sh` first, since only one program can use the port.

Restore is for this same device, e.g. after `factoryreset` or a wipe. The node number comes from the chip, so
putting the identity on a different Cardputer would give that node someone else's key. Restoring onto a device that
already has a channel of the same name reports "already exists" for that line and leaves the channel as it is.

## Building and flashing

Requires [PlatformIO](https://platformio.org/). On NixOS: `nix shell nixpkgs#platformio`.

```sh
pio run                 # build
pio run -t upload       # flash over USB-C
pio device monitor      # serial console
```

If upload can't connect, hold **G0** while plugging in USB to enter download mode.

Host-side protocol tests (run `pio run` once first to fetch libraries):

```sh
nix-shell -p gcc mbedtls --run test/host/run.sh
```

They check the channel hash, frequency-slot math, and that we can decrypt a real PKI direct message
captured from the official firmware (its `test/test_crypto` vector).

## Design

| File | Role |
|---|---|
| `src/radio.*` | SX1262 via RadioLib, Cap RF-enable (PI4IOE5V6408 @0x43, pin 0), TX queue, listen-before-talk |
| `src/crypto.*` | AES-CTR channel crypto, X25519 + SHA-256 + AES-CCM for DMs (byte-compatible with firmware) |
| `src/mesh.*` | framing, de-dup, flood relay, ACK/NAK, NodeInfo, outgoing message state machine and key exchange |
| `src/nodedb.*` | known nodes and public keys, persisted to LittleFS |
| `src/messages.*` | conversation history (last 120 messages), persisted to LittleFS |
| `src/ui.*`, `src/input.*` | screens and keyboard handling (Fn arrows, key repeat) |
| `src/gps.*` | NMEA parsing (TinyGPSPlus) with 115200/9600 baud auto-detect, distances |
| `src/cli.*` | serial configuration console |
| `src/regions.*` | region/preset tables mirrored from the firmware |
| `lib/meshtastic_protobufs` | nanopb output copied from `meshtastic/firmware` |

It's single-threaded: `loop()` polls the radio, runs the mesh timers, reads the keyboard, and redraws
when something changed.

It acts as a normal `CLIENT` node: it rebroadcasts other nodes' packets using the firmware's SNR-weighted
delay, cancels its relay if someone else relays first, and broadcasts its own NodeInfo at boot and every
3 hours. Turn relaying off with `relay off`. Like stock firmware, when it hears a node it has no NodeInfo for
(usually one several hops away), it asks that node for its name and key, at most one request every 30 s.

## Status and roadmap

**Implemented, compiles, host-tested crypto — not yet tested on hardware.**
First things to check on a real device:

- [ ] Radio comes up (`info` shows region and frequency; RX counter rises near other nodes)
- [ ] Nodes appear and get padlocks as NodeInfo broadcasts arrive
- [ ] Channel message both ways with a stock node / phone app
- [ ] DM to a node whose key we have → `delivered`
- [ ] DM to a node without a key → key exchange → `delivered`
- [ ] Keyboard mapping and font sizes feel right
- [ ] `chadd` a channel, add the same name+key as a secondary on a phone, and chat both ways
- [ ] GPS dot turns green outdoors; `info` over serial shows coordinates; `share on` shows us on a phone map

Possible next steps, kept small on purpose:

- Battery and telemetry from other nodes in a node detail view
- Real clock time on messages (from GPS)
- Filter/search the node list by typing
- Unicode and emoji rendering (DejaVu covers ASCII/Latin only)
- Signed (XEdDSA) NodeInfo broadcasts. We don't sign; stock firmware accepts unsigned nodes that have
  never signed.

## Limitations

- Public keys are trust-on-first-use and accept updates, so a node that reflashes gets its new key picked up.
- Traceroute and telemetry requests aren't answered (they're relayed, though). Position requests are
  answered only when sharing is on.

## License

GPL-3.0 (see [LICENSE](LICENSE)). The protobuf definitions and several protocol details come from
[meshtastic/firmware](https://github.com/meshtastic/firmware) (GPL-3.0). Hardware details were cross-checked
against [d4rkmen/plai](https://github.com/d4rkmen/plai).
