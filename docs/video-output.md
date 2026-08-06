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
- `kms-pisp` (the hardware path) targets **NV12** *if and only if* the PiSP's
  tiled DMABUF output requires 4:2:0. Prefer NV16 here too if the converter can
  emit tiled 4:2:2 — this is one of the open questions for the step 0 spike
  below. NV12 discards half the vertical chroma resolution, so only accept it
  when the hardware zero-copy path forces it.

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

> **Unverified element name.** `pispconvert` has not been confirmed to exist as
> a GStreamer element. docs/pi-setup.md records the hardware path as
> `pispbe`/`v4l2convert` (the generic V4L2 mem2mem element driving the PiSP
> backend node). The step 0 spike must resolve the actual element name before
> the rest of this section is treated as final; substitute the real element
> throughout if it differs.

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

Each branch must contain a separate leaky queue:

```text
queue
    max-size-buffers=1
    max-size-bytes=0
    max-size-time=0
    leaky=downstream
```

This prevents a slow browser or JPEG encoder from blocking HDMI output.

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

## Application-side frame distribution

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

Possible strategy:

```text
client count becomes nonzero
  -> enable preview branch

client count becomes zero
  -> disable preview branch
```

Use a GStreamer `valve`, pad probe, or equivalent branch-control mechanism.

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

0. Spike the hardware converter: confirm the real element name
   (`gst-inspect-1.0 pispconvert`, else `v4l2convert` over the PiSP backend
   node), whether it exposes multiple scaled source pads, and which formats and
   DMABUF layouts it can emit (in particular, whether tiled NV16 is available
   or the path forces NV12). The answers decide the format target and whether
   the dual-output branching above is reachable. Also measure sustained
   1080p60 conversion throughput and CPU cost with system-memory input —
   the main path depends on it, making this the design's gating measurement.
1. Install and verify the converter element from step 0.
2. Test YUY2 to NV12 DMABUF conversion with `kmssink` (use NV16 if step 0 shows
   tiled 4:2:2 is available).
3. Add the `kms-pisp` output backend.
4. Keep `kms-software` (NV16) as a fallback and correctness reference.
5. Add subtitle rendering before conversion.
6. Add a low-resolution preview branch.
7. Add `videorate`, `jpegenc`, and `appsink`.
8. Implement the shared latest-JPEG buffer.
9. Add `/api/preview.jpg`.
10. Add `/api/preview.mjpeg`.
11. Add client limits and frame dropping.
12. Disable preview encoding when unused.
13. Measure HDMI latency, preview latency, CPU usage, and dropped frames.
