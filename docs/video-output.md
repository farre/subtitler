# PiSP Output and MJPEG Preview Plan

## Goal

Use the Raspberry Pi 5 PiSP hardware to convert captured YUY2 video into a
semi-planar format supported by DRM/KMS, while also generating a low-resolution
MJPEG preview for the web interface.

The output format is a property of the backend, not a global constant:

- `kms-software` (the correctness reference) targets **NV16**. It is a lossless
  4:2:2 repack of the captured YUY2 (Y plane plus interleaved CbCr, no chroma
  subsampling), and it is the format empirically proven to scan out on vc4 —
  the `videotestsrc ! NV16 ! kmssink` run that closed #74/#5. A correctness
  reference must be the highest-fidelity path, so it must not discard chroma.
- `kms-pisp` (the hardware path) targets **NV12 SAND-tiled** by default, with
  **YU16 LINEAR** as a lossless alternate. This was an open question for the
  step 0 spike; it is now **resolved** by `gst-inspect-1.0 pispconvert` and
  `modetest -M vc4 -p`:
  - **NV16 is unreachable.** vc4 planes support it, but `pispconvert` cannot
    emit NV16 — so semi-planar 4:2:2 is off the table on this path.
  - **NV12** is emitted by `pispconvert` in a SAND-tiled DMABuf variant
    (`NV12:0x0700000000000004`), and vc4 scans out the same tiled format. This
    is the efficient path: zero-copy *and* tiled (lower scanout DRAM
    bandwidth). Cost: 4:2:0, i.e. half the vertical chroma.
  - **YU16** (planar 4:2:2) is emitted by `pispconvert` and scanned out by vc4,
    but **LINEAR only** (no tiling modifier on either side). It is lossless
    4:2:2 and still zero-copy DMABuf, but untiled (~33% more scanout bandwidth,
    3 planes).

  Default to NV12: the CV105 captures upstream content that is almost always
  already 4:2:0, so YU16's fidelity edge is largely theoretical, and SAND
  tiling leaves memory headroom for the preview branch and overlay
  compositing. Keep YU16 selectable for the one case it helps — sharp colored
  subtitle text, which is composited at 4:2:2 before conversion and can soften
  under NV12's chroma subsampling. Decide with a visual A/B on real content.

## Main video path

Input format:

- YUY2
- 1920x1080
- 60 fps
- Supplied through application-owned buffers into `appsrc`

Output path:

```text
appsrc
  -> subtitle overlay
  -> pispconvert
  -> NV12 DMABUF
  -> kmssink
```

The Raspberry Pi DRM planes do not support packed YUY2 directly, so
conversion is required before `kmssink`. This is confirmed both by the
plane format lists and empirically (a YUY2 test pattern fails `kmssink`
negotiation with not-negotiated; see docs/pi-setup.md and #74).

Conversion happens on the output side, after `appsrc` and the subtitle
overlay, rather than on the capture side before frames enter the app:
compositing operates on the 4:2:2 frame (better text chroma), the
application boundary stays in the capture format, and a single conversion
point feeds both the HDMI and preview branches.

Use `pispconvert` as the primary hardware-accelerated conversion backend.

> **Element confirmed.** `pispconvert` exists (install with
> `sudo apt install gstreamer1.0-pispconvert`; v1.4.0, from Raspberry Pi's
> libpisp) and exposes two Always scaling source pads (`src0`/`src1`), so the
> dual-output branching below is reachable. See docs/pi-setup.md for the full
> `gst-inspect-1.0` findings.

Target KMS caps:

```text
video/x-raw(memory:DMABuf),
format=DMA_DRM,
drm-format=NV12,
width=1920,
height=1080,
framerate=60/1
```

> **Caps detail for the spike.** DMABuf caps express the format as
> `fourcc:modifier` (e.g. the Broadcom SAND tiling modifiers from the vc4
> plane IN_FORMATS blobs), and the modifier is what makes the path
> zero-copy. Step 0 should record the exact caps strings that negotiate,
> not just the fourcc.

> **How to validate negotiation.** Pad templates advertise *capability*, not
> what actually links. Run the leg with `gst-launch-1.0 -v` and read the caps
> GStreamer prints on each pad after PAUSED — that is the negotiated result:
>
> ```sh
> gst-launch-1.0 -v videotestsrc num-buffers=120 \
>   ! video/x-raw,format=YUY2,width=1920,height=1080,framerate=60/1 \
>   ! pispconvert \
>   ! 'video/x-raw(memory:DMABuf),format=DMA_DRM,drm-format=NV12,width=1920,height=1080,framerate=60/1' \
>   ! kmssink driver-name=vc4 force-modesetting=true
> ```
>
> Check the caps on the `pispconvert`→`kmssink` link: a bare `drm-format=NV12`
> means a copy, while `NV12:0x0700000000000004` (SAND modifier) confirms the
> zero-copy tiled path. A `not-negotiated` (-4) error means the format/modifier
> the plane wants was never offered — record what each side proposed.

Conceptual GStreamer pipeline:

```text
appsrc
    name=output_source
    is-live=true
    format=time
    block=true
    caps=video/x-raw,format=YUY2,width=1920,height=1080,framerate=60/1
! textoverlay
    name=subtitle_overlay
    halignment=center
    valignment=bottom
! pispconvert
    name=converter
    output-buffer-count=4
! video/x-raw(memory:DMABuf),
    format=DMA_DRM,
    drm-format=NV12,
    width=1920,
    height=1080,
    framerate=60/1
! kmssink
    driver-name=vc4
    force-modesetting=true
    sync=true
```

## Software fallback

Keep a software conversion backend for testing and diagnostics. It converts to
**NV16** — the lossless 4:2:2 repack, and the format empirically proven to
display (#74) — so it stays the highest-fidelity reference:

```text
appsrc
  -> subtitle overlay
  -> videoconvert
  -> NV16
  -> kmssink
```

Pipeline tail:

```text
! videoconvert n-threads=4
! video/x-raw,format=NV16,width=1920,height=1080,framerate=60/1
! kmssink driver-name=vc4 force-modesetting=true sync=true
```

Supported output modes:

```text
kms-pisp
kms-software
window
null
```

`window` targets a dev machine with a display server (e.g. `glimagesink`)
for debugging; the headless appliance uses the `kms-*` modes or `null`.

Use `kms-software` as the correctness reference when debugging PiSP or
DMABUF issues. Because it is lossless NV16, any chroma artifact seen only
on the `kms-pisp` path can be attributed to the hardware conversion rather
than to 4:2:0 subsampling.

## MJPEG web preview

The preview should show the composited video after subtitles have been rendered.

Preview path:

```text
subtitle overlay
  -> PiSP scaling/conversion
  -> JPEG encoder
  -> appsink
  -> shared latest-frame buffer
  -> HTTP clients
```

Recommended preview settings:

```text
Resolution: 640x360
Frame rate: 10 fps
JPEG quality: 75
Maximum clients: 4
```

Use one JPEG encoder for all clients. Do not create a separate encoder per
browser connection.

Each browser should receive the most recent completed JPEG. Slow clients
should skip frames rather than building a backlog.

> **As implemented (#376).** Until the overlay exists, the tee sits right
> after the `output_source` appsrc — the point the overlay will be inserted
> at — which is also the only spot whose format (YUY2) `jpegenc` accepts:
> NV16 (software path) and SAND-tiled DMABuf (pisp path) are both
> unreachable for it. The branch is `tee ! queue(leaky=1) ! videoscale
> 640x360 ! jpegenc q75 ! appsink(async=false)`, drained by a pull thread
> into the shared latest-frame buffer. Rate limiting is decimation in the
> gate probe (every 6th frame), not `videorate`: videorate anchors its
> output cadence to the segment start and emits a catch-up burst after
> every gated gap. Stream seeds the buffer with a magenta placeholder at
> startup, so the endpoints always have a frame (symmetric with the
> no-signal screen). Serving is enabled with `--web` (port 8080).

## Preferred PiSP branching

When supported reliably, use the two PiSP outputs:

```text
textoverlay
  -> pispconvert
       |
       +-> output 0: 1920x1080 NV12 DMABUF -> kmssink
       |
       +-> output 1: 640x360 raw video -> videorate -> jpegenc -> appsink
```

> **Depends on the element exposing two source pads.** This dual-output design
> maps to a real PiSP hardware capability (the camera main + lores streams),
> but only works if the chosen GStreamer element surfaces both scaled outputs.
> A generic `v4l2convert` mem2mem element has a single output. If the step 0
> spike shows a single-output element, derive the preview branch instead with a
> `tee ! videoscale` off the converter output (one extra CPU downscale at
> 640x360, which is cheap), and keep the isolation guarantees below.

The HDMI output must never depend on the preview branch.

Each branch must contain a separate queue; the preview side is leaky:

```text
queue
    max-size-buffers=1
    max-size-bytes=0
    max-size-time=0
    leaky=downstream
```

This prevents a slow browser or JPEG encoder from blocking HDMI output.

> **As implemented.** Both queues are `max-size-buffers=1`; the HDMI
> branch's queue is **not** leaky (dropped HDMI frames would be silent
> glitches). Every tee branch needs its own queue: without one the
> clock-synced sink blocks the tee's streaming thread in preroll and the
> pipeline deadlocks. The preview appsink additionally sets `async=false`:
> a starving (gated) branch would otherwise hold the whole pipeline in
> preroll — the output pipeline isn't live in the preroll sense, it
> reaches PLAYING only when every sink prerolls. All of this is locked in
> by a regression test (`tests/output_pipeline_tests.cpp`) that pushes
> frames through the real description with the gate closed and open.

## MJPEG HTTP endpoint

Endpoint:

```text
GET /api/preview.mjpeg
```

Response content type:

```http
Content-Type: multipart/x-mixed-replace; boundary=frame
Cache-Control: no-store
Pragma: no-cache
```

Each frame:

```http
--frame
Content-Type: image/jpeg
Content-Length: <number of bytes>
X-Frame-Sequence: <sequence number>

<JPEG data>
```

The web page can display it using:

```html
<img src="/api/preview.mjpeg" alt="Video preview" />
```

Also provide a single-frame endpoint:

```text
GET /api/preview.jpg
```

### Serving mechanics (libsoup 3, as implemented in `src/web/`)

- The MJPEG handler registers the client and returns immediately; a frame
  loop or sleep in the handler would stall every other endpoint. libsoup
  auto-pauses a chunked response while no chunk is available, so no
  explicit pause is needed.
- `soup_message_headers_set_encoding(..., SOUP_ENCODING_CHUNKED)` for the
  HTTP transfer framing (no outer Content-Length; the Content-Length
  inside each part describes only that JPEG), and
  `soup_message_body_set_accumulate(body, FALSE)` so written chunks are
  discarded and an indefinite response cannot grow.
- Backpressure: each client has one frame in flight and one pending;
  `wrote-chunk` clears in-flight and flushes only the newest pending
  frame. Slow clients skip frames; memory stays bounded.
- `finished` removes the client. Disconnects are detected on the next
  frame write (an idle chunked response schedules no I/O), i.e. within
  one frame period while the encoder runs.
- A watcher thread blocked on the shared latest-frame buffer posts
  deliveries onto the server's GMainContext with
  `g_main_context_invoke_full`; `SoupServerMessage` objects are only ever
  touched on the server context. The GStreamer side never writes to HTTP
  sockets.
- At most 4 concurrent MJPEG clients (503 beyond); the client count
  toggles the preview gate at the zero/nonzero transitions.

## Application-side frame distribution

> Implemented as `PreviewFrameBuffer` (`src/utils/preview_frame.h`):
> dependency-free so both the stream module (producer) and the web module
> (consumer) can use it; `Store`/`Latest`/`WaitNewer`.

Store only the newest encoded frame:

```cpp
struct EncodedFrame {
    std::uint64_t sequence;
    std::uint64_t pts_ns;
    std::shared_ptr<const std::vector<std::byte>> data;
};
```

The GStreamer `appsink` callback should:

1. Pull the JPEG buffer.
2. Copy or retain the encoded data safely.
3. Replace the shared latest frame.
4. Notify connected clients.
5. Return immediately.

It must not write to HTTP sockets from the GStreamer streaming thread.

Each HTTP client tracks the last sequence number it sent. If several
frames are produced before the client is ready, send only the newest
frame.

## Preview configuration

Example configuration:

```json
{
  "preview": {
    "enabled": true,
    "width": 640,
    "height": 360,
    "frame_rate": 10,
    "jpeg_quality": 75,
    "max_clients": 4
  }
}
```

Suggested presets:

```text
off:
  disabled

low:
  426x240
  5 fps
  quality 65

normal:
  640x360
  10 fps
  quality 75

high:
  960x540
  15 fps
  quality 80
```

Avoid arbitrary per-client resolutions because that would require
additional scaling or encoding pipelines.

## Preview activation

Do not encode MJPEG when no clients are connected.

Strategy (as implemented):

```text
client count becomes nonzero
  -> enable preview branch

client count becomes zero
  -> disable preview branch
```

The gate is a buffer-dropping pad probe on the preview queue
(`InstallPreviewGate` in `src/stream/preview_gate.{h,cpp}`), not a
`valve`: while dropping, a valve fails serialized queries
(`GST_QUERY_LATENCY` included), which stalls latency computation for the
whole pipeline, HDMI branch included. The probe drops only buffers, so
queries and sticky events (caps/segment) keep flowing and the branch
starts instantly when activated. Rate limiting is the same probe keeping
every 6th frame while active — see the note in "MJPEG web preview".

## Timing limitation

The MJPEG preview is not an authoritative synchronization display.

It adds variable delay from:

```text
scaling
JPEG encoding
HTTP transport
browser buffering
JPEG decoding
screen rendering
```

Use it for:

- confirming capture
- selecting subtitle files
- checking displayed cues
- approximate monitoring

For precise manual synchronization, watch the physical HDMI output while
using the web controls.

## Implementation order

0. Spike the hardware converter — **done** (see docs/pi-setup.md): element is
   `pispconvert` (`gstreamer1.0-pispconvert`), two scaling src pads, emits NV12
   SAND-tiled (`NV12:0x0700000000000004`) and YU16 LINEAR; NV16 unreachable.
   Sustained 1080p60 conversion throughput confirmed at a solid 60.0 fps
   with zero-copy DMABuf scanout.
1. Install the converter element (`sudo apt install gstreamer1.0-pispconvert`).
2. Test YUY2 to NV12 SAND-tiled DMABUF conversion with `kmssink`; confirm the
   negotiated modifier per the validation note above. (YU16 LINEAR is the
   lossless alternate if colored-text fidelity needs it.) **Blocked on
   BCM2712C1:** pispconvert NV12 output renders blue (raspberrypi/libpisp#76);
   see docs/pi-setup.md. `kms-software` remains the default until fixed.
3. Add the `kms-pisp` output backend.
4. Keep `kms-software` (NV16) as a fallback and correctness reference.
5. Add subtitle rendering before conversion.
6. ~~Add a low-resolution preview branch~~ — done (#379), with the
   deviations noted above.
7. ~~Add `videorate`, `jpegenc`, and `appsink`~~ — done (#379), except
   `videorate`: rate limiting is decimation in the gate probe.
8. ~~Implement the shared latest-JPEG buffer~~ — done (#381).
9. ~~Add `/api/preview.jpg`~~ — done (#382), including the magenta
   placeholder seeding.
10. ~~Add `/api/preview.mjpeg`~~ — done (#382), plus a minimal index page.
11. ~~Add client limits and frame dropping~~ — done (#384).
12. ~~Disable preview encoding when unused~~ — done (#384), gate driven by
    the MJPEG client count.
13. Measure HDMI latency, preview latency, CPU usage, and dropped frames
    (on the Pi).
