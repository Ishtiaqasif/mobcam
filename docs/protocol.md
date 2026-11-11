# Phone ⇄ PC protocol

Both sides live in this repo; keep these in sync when changing either:

- phone: [StreamEngine.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/StreamEngine.kt),
  [StreamServer.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/StreamServer.kt),
  [DiscoveryResponder.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/DiscoveryResponder.kt),
  [MobcamStreamModule.kt](../app/modules/mobcam-stream/android/src/main/java/expo/modules/mobcamstream/MobcamStreamModule.kt)
- PC: [decoder.cpp](../pc/core/decoder.cpp), [net.cpp](../pc/core/net.cpp), [engine.cpp](../pc/core/engine.cpp)

## Video stream — TCP 5000 (phone is the server)

A raw H.264 Annex-B elementary stream, no extra framing, so `ffplay -f h264 tcp://<phone>:5000`
works. Over USB the PC runs `adb forward tcp:5000 tcp:5000` and connects to `127.0.0.1:5000`.

Each encoded frame is sent as:

```
[SPS PPS LENS-SEI]   (keyframes only, every ~1 s and when a client connects)
STATUS-SEI
slice NAL(s)
AUD (00 00 00 01 09 F0)   marks the frame complete so the PC can decode it immediately
```

A new client receives nothing until the next keyframe. If a client falls behind (queue of 30
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

## Commands — PC → phone, same TCP connection

ASCII lines ending in `\n`:

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

## PC internals: virtual camera shared memory

`mobcam-core` renders every frame into a 1920×1080 NV12 canvas (rotation + fit/fill) and copies
it into `Global\MobCamSharedFrame_v1` (triple-buffered, layout in
[shared_frame.h](../pc/common/shared_frame.h)). `mobcam-source.dll`, running inside Frame
Server, creates that mapping, scales the canvas to the size the app asked for (1080p, 720p or
360p) and shows a "waiting for phone" card when no frame arrived for 1.5 s. It stamps
`lastReadTick` so the PC app can show whether an app is using the camera.
