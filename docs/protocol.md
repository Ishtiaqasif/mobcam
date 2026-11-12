# Phone ⇄ PC protocol

Both sides live in this repo; keep these in sync when changing either:

- phone: [SecureChannel.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/SecureChannel.kt),
  [Pairing.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/Pairing.kt),
  [StreamEngine.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/StreamEngine.kt),
  [StreamServer.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/StreamServer.kt),
  [DiscoveryResponder.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/DiscoveryResponder.kt),
  [MobcamStreamModule.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/MobcamStreamModule.kt)
- PC: [secure.cpp](../pc/core/secure.cpp), [decoder.cpp](../pc/core/decoder.cpp), [net.cpp](../pc/core/net.cpp), [engine.cpp](../pc/core/engine.cpp)

## Connection — TCP 5000 (phone is the server), protocol v2

Every connection is authenticated and encrypted, Wi-Fi and USB alike; there is no raw mode, so
`ffplay tcp://<phone>:5000` no longer works. Over USB the PC runs `adb forward tcp:5000 tcp:5000`
and connects to `127.0.0.1:5000`; the phone sees those as loopback connections.

Implementations: phone [SecureChannel.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/SecureChannel.kt),
PC [secure.cpp](../pc/core/secure.cpp).

### Goals

- An unpaired PC on the same Wi-Fi gets no video and cannot send commands.
- Pairing is one tap on the phone after comparing a 6-digit code shown on both screens; a
  man-in-the-middle makes the codes differ (except with probability 10⁻⁶ per attempt).
- A paired PC reconnects silently. USB (loopback) connections never prompt but use the same
  handshake and encryption.
- Old peers get a clear "update the app" message instead of a broken stream.

### Primitives

All from the platforms' built-in crypto (Android `java.security`/`javax.crypto` + Android
Keystore, Windows CNG `BCrypt`/`NCrypt`). Nothing home-made below the message level.

| Use | Primitive |
|---|---|
| Long-term identity (one per phone install, one per Windows user) | ECDSA P-256 with SHA-256. Private key never leaves Android Keystore / the CNG key store. |
| Per-connection key exchange | ephemeral ECDH P-256 |
| Hash, commitment | SHA-256 |
| Key derivation | HKDF-SHA256 (RFC 5869) |
| Record encryption | AES-256-GCM, 128-bit tag |

Encodings: public keys are uncompressed X9.62 points (`04 ‖ X ‖ Y`, 65 bytes). Signatures are raw
`r ‖ s` (64 bytes, each big-endian, zero-padded; Android converts to/from DER). The ECDH secret is
the 32-byte big-endian X coordinate. Integers are big-endian. Names are UTF-8, at most 64 bytes.

### Wire format

```
preamble   = "MOBCAM" (6 ASCII bytes) ‖ version u16            (8 bytes, version = 2)
message    = length u32 ‖ type u8 ‖ body                        (length counts type + body)
record     = length u32 ‖ AES-GCM ciphertext ‖ tag(16)          (length counts ciphertext + tag)
```

Handshake messages are at most 1 KiB, records at most 4 MiB. A peer that receives anything larger,
an unknown type, or a message out of order closes the connection.

### Handshake

```
PC                                                     Phone
 │ preamble(2)  ─────────────────────────────────────▶ │
 │ ◀───────────────────────────────────── preamble(2)  │  (a v1 phone sends H.264 instead)
 │ COMMIT  0x01: pcKey(65) ‖ commit(32) ‖ pcName ────▶ │
 │ ◀──── HELLO 0x02: phoneKey(65) ‖ ePhone(65) ‖       │
 │                   nPhone(32) ‖ phoneName            │
 │ REVEAL  0x03: ePc(65) ‖ nPc(32) ─────────────────▶ │  phone checks commit
 │ PC_AUTH 0x04: sig_pc(64) ────────────────────────▶ │  phone checks sig_pc
 │                                                     │  known PC or USB → skip to PHONE_AUTH
 │ ◀────────────────────────────── PAIRING 0x05 (empty)│  unknown PC on Wi-Fi: phone asks the user
 │ ◀────────────────────────────── PHONE_AUTH 0x06:    │  Allow (or no prompt needed)
 │                                 sig_phone(64)       │
 │ ◀────────────────────────────── DENIED 0x07:        │  Deny / timeout / busy / bad PC
 │                                 reason u8           │  signature, then the phone closes
 │ ═══════════════ encrypted records both ways ═══════ │
```

- `pcKey`, `phoneKey`: identity public keys. `ePc`, `ePhone`: fresh ephemeral ECDH public keys.
  `nPc`, `nPhone`: 32 random bytes each.
- `commit = SHA-256("MOBCAM v2 commit" ‖ ePc ‖ nPc)`. The PC commits to its ephemeral key before
  it sees the phone's and reveals it only after. A man-in-the-middle therefore has to fix its own
  keys on both sides before it knows the values the codes depend on, so it cannot search for keys
  that make the two codes match.
- `T` = every byte sent by either side so far, in order, from the PC preamble through the whole
  REVEAL message (length prefixes included). `H = SHA-256(T)`.
- `sig_pc = ECDSA(pcKey, "MOBCAM v2 pc auth" ‖ H)`,
  `sig_phone = ECDSA(phoneKey, "MOBCAM v2 phone auth" ‖ H)`. Each side proves it holds the private
  identity key, bound to this connection's ephemeral keys and names.
- `Z = ECDH(ePc, ePhone)`. `PRK = HKDF-Extract(salt = H, ikm = Z)`, then with HKDF-Expand:
  - `kPcToPhone = Expand(PRK, "MOBCAM v2 pc->phone", 32)`
  - `kPhoneToPc = Expand(PRK, "MOBCAM v2 phone->pc", 32)`
  - `code = Expand(PRK, "MOBCAM v2 code", 4)` as u32, mod 1 000 000, shown as `482 913`.
- DENIED reasons: `1` the user tapped Deny, `2` no answer within 60 s, `3` another pairing prompt is
  already open, `4` the PC's signature or commitment is invalid.

The phone ends the connection if the PC's preamble, COMMIT, REVEAL or PC_AUTH doesn't arrive
within 10 s. While its prompt is open it watches the socket and closes the prompt if the PC goes
away. The PC allows up to 70 s for PHONE_AUTH after PAIRING and 10 s otherwise.

### Two-sided pairing

Pairing is a numeric comparison on **both** devices, like Bluetooth: the phone's user taps
*Allow* and the PC's user confirms *Codes match*, in either order. Without the PC-side check, any
device on the network that answers discovery could pair itself with the PC and feed it video.

```
PC                                                     Phone
 │ ◀────────────────────────────── PAIRING             │  both show the code
 │   PC user: "Codes match" (any time)                 │  phone user: Allow
 │ ◀────────────────────────────── PHONE_AUTH          │
 │   PC waits for its user (up to 60 s), then trusts   │
 │   the phone                                         │
 │ ══ PAIRED record 0x12 (empty, encrypted) ═════════▶ │  phone trusts the PC, starts video
```

- After sending PHONE_AUTH for a new PC, the phone sends **no video** and ignores commands until
  the PAIRED record arrives (65 s timeout). Only then does it add the PC to its paired list.
- If the PC's user taps *Cancel*, or doesn't confirm within 60 s of PHONE_AUTH, the PC closes the
  connection without sending PAIRED. Neither side saves anything; the phone shows "Pairing with
  PC-NAME was not confirmed on the PC".
- PAIRED is the first record the PC sends on such a connection. Being encrypted with
  `kPcToPhone`, only the PC that took part in this handshake can produce it.

### Trust

- **Phone:** keeps a list of paired PCs (identity key, name, paired time) in app storage and shows
  it with *Forget*. A Wi-Fi connection from a PC that is not listed shows *"Allow PC-NAME to use
  the camera? Code 482 913"*. The PC is added to the list once the user tapped *Allow* **and** the
  PC's PAIRED record arrived. Loopback (USB) connections are accepted without a prompt and are
  **not** added to the list.
- **PC:** keeps trusted phones per Windows user under `HKCU\Software\MobCam\PairedPhones` (value
  name = hex SHA-256 of the phone's identity key, data = key ‖ name). After a valid PHONE_AUTH that
  followed a PAIRING message, and once its own user confirmed the code, the PC adds the phone to
  the list and sends PAIRED. Over USB (and for a hand-typed
  `127.0.0.1`, which can only be an `adb forward`) it accepts any valid PHONE_AUTH. On Wi-Fi, a phone that sends PHONE_AUTH without a prompt must already be listed,
  otherwise the PC closes the connection with "phone not paired" (the phone remembered this PC but
  the PC forgot the phone: forget the PC on the phone and pair again).
- Each side checks the other's signature against the identity key from this handshake, so a
  stolen *public* key is useless.
- The PC's identity key is sent in the clear, so a passive observer on the network can tell when
  the same PC connects again. Video and commands stay confidential.
- Over USB, any app on the phone could also connect to loopback port 5000. That is no worse than
  v1, and an Android app that wants the camera can ask for it directly.

### Encrypted records

After PHONE_AUTH every byte in both directions is a record. Each direction has its own key and a
64-bit counter starting at 0, so nonces never repeat: `nonce = 00 00 00 00 ‖ counter u64`. The
4-byte length prefix is the GCM additional data. A record that fails authentication ends the
connection.

The plaintext of a record is `type u8 ‖ payload`:

| Type | Direction | Payload |
|---|---|---|
| `0x10` VIDEO | phone → PC | one encoded frame, laid out as in [Video frames](#video-frames) |
| `0x11` COMMAND | PC → phone | one [command](#commands--pc--phone) as ASCII, no line break |
| `0x12` PAIRED | PC → phone | empty; the PC's user confirmed the pairing code ([two-sided pairing](#two-sided-pairing)) |

### Versions

- The PC sends its preamble first. A v2 phone answers with its own preamble. If it doesn't
  support the PC's version it closes right after, and the PC shows "update MobCam on the PC" (phone
  newer) or "update the phone app" (phone older).
- A v1 phone ignores the preamble and starts sending H.264 (`00 00 00 01 …`). The PC sees no
  `MOBCAM` magic and shows "The MobCam app on your phone is too old".
- A v1 PC sends nothing. The phone closes after 10 s and shows "A PC with an old MobCam version
  tried to connect".

## Video frames

Each encoded frame (one VIDEO record) is:

```
[SPS PPS LENS-SEI]   (keyframes only, every ~1 s and when a client connects)
STATUS-SEI
slice NAL(s)
AUD (00 00 00 01 09 F0)   marks the frame complete so the PC can decode it immediately
```

A new client receives nothing until its handshake is done and the next keyframe arrives. If a client falls behind (queue of 30
frames full) its backlog is dropped and it resyncs on the next keyframe.

### Metadata SEI

Both are SEI `user_data_unregistered` (payload type 5) NALs: `00 00 00 01 06 05 <size> <uuid16> <ascii> 80`.
The ASCII never contains `00 00`, so no emulation-prevention bytes are needed. Decoders other
than MobCam ignore them.

| UUID (ASCII) | Payload | Example |
|---|---|---|
| `MOBCAM-ROTATION!` | `R F HHHH LL`: rotation digit `0`–`3` (×90° clockwise to make the picture upright), facing `b`/`f`, 4-digit target height, 2-digit lens index | `1b072000` |
| `MOBCAM-LENSLIST!` | `label,zoom,b|f` entries separated by `;`, in lens-index order (≤ 220 chars) | `Ultra-wide,0.68,b;Wide,1,b;Telephoto,3.4,b;Front,1,f` |

The PC must accept shorter `MOBCAM-ROTATION!` payloads (older phones send only rotation, or
rotation + facing + height).

## Commands — PC → phone

One ASCII command per COMMAND record (v1 sent them as plain lines ending in `\n`):

| Command | Effect |
|---|---|
| `LENS <index>` | Select a lens from the lens list. Same camera → zoom change only; otherwise the camera reopens. |
| `FACING front` / `FACING back` | Front camera / last used back lens |
| `SWITCH` | Toggle front/back |
| `RES 480` / `RES 720` / `RES 1080` | Max stream height (the closest supported size at or below it, 16:9 preferred) |

The phone confirms changes indirectly through the next `MOBCAM-ROTATION!` SEI.

## Discovery — UDP 5001

PC broadcasts `MOBCAM_DISCOVER` to every interface's directed broadcast address and
255.255.255.255. Each phone answers unicast with `MOBCAM_HERE <streamPort> <device name>`.
Unchanged in v2: discovery reveals only the name and port, and nothing works without pairing.

## PC internals: virtual camera shared memory

`mobcam-core` renders every frame into a 1920×1080 NV12 canvas (rotation + fit/fill) and copies
it into `Global\MobCamSharedFrame_v1` (triple-buffered, layout in
[shared_frame.h](../pc/common/shared_frame.h)). `mobcam-source.dll`, running inside Frame
Server, creates that mapping, scales the canvas to the size the app asked for (1080p, 720p or
360p) and shows a "waiting for phone" card when no frame arrived for 1.5 s. It stamps
`lastReadTick` so the PC app can show whether an app is using the camera.
