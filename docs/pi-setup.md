# Pi appliance setup and hardware validation

Verified state of the subtitler appliance as of 2026-08-02: provisioning
complete, CV105 capture and vc4 display output validated, and the full
passthrough (with software NV16 conversion) ran for 30 minutes with zero
dropped frames.

## Operating system

- Raspberry Pi 5, Raspberry Pi OS Lite 64-bit (trixie), kernel
  6.18.39+rpt-rpi-2712
- Boots headless to a text console; no desktop or display manager
- SSH access enabled and verified
- System packages fully updated

## Software

- All build and runtime dependencies installed — see the apt command line in
  the README's Building section
- The project configures and builds on the Pi with the `default` preset
  (clang, C++26), and `ctest --test-dir build` passes

## Service user

- `subtitler` (uid 102): system account, no home directory,
  `/usr/sbin/nologin` shell — creation commands in the README's
  "Provisioning the Pi" section
- Groups: `video` (44), `render` (992), `audio` (29)
- Verified as that user: `/dev/dri/card0` is writable, and
  `v4l2-ctl --device /dev/video0 --all` succeeds

## Capture device (CV105)

USB:

- UltraSemi "USB3 Video", VID:PID `345f:2130`, serial 20210623, UVC 1.00
- Connected through a USB-C-to-USB-A adapter to a Pi USB 3 port; negotiates
  SuperSpeed (5000M) on `xhci-hcd.1-1`
- Interfaces: 2x uvcvideo (video + metadata), 2x snd-usb-audio, 1x usbhid

Video:

- Capture node `/dev/video0` (plus `/dev/video1` metadata and `/dev/media3`)
- Pixel formats: YUYV and MJPG
- Sizes (both formats): 1920x1080, 1600x1200, 1360x768, 1280x1024,
  1280x960, 1280x720, 1024x768, 800x600, 720x576, 720x480, 640x480
- Frame rates (discrete, at every size): 60, 50, 30, 20, 10 fps
- **1920x1080 YUYV at 60 fps confirmed** — matches the hardcoded capture
  format; 1920x1080 MJPG at 60 fps available as a compressed fallback
- UVC controls: brightness, contrast, saturation, hue (all 0-100)

Audio:

- ALSA card 0 device 0 ("USB3 Video" / "USB Audio"), one capture subdevice
- Address as `hw:CARD=Video,DEV=0` — the card name is stable across reboots,
  the numeric index is not

Kernel log:

- Clean enumeration; no USB/UVC errors, resets, or disconnects
- The kernel disables LPM for the device ("LPM exit latency is zeroed,
  disabling LPM") — a benign quirk workaround that keeps the link out of
  power-saving states, favorable for continuous capture

Note: `/dev/video19`-`/dev/video35` are the Pi's own pispbe (ISP) and
rpi-hevc-dec nodes, not the CV105.

## Display output (DRM/KMS)

Verified on the vc4 driver with modetest/gst-inspect-1.0 and live
gst-launch runs (Pi OS Lite console, no X11/Wayland):

- kmssink is installed (gst-plugins-bad 1.26.2)
- Connectors: HDMI-A-1 (id 35) and HDMI-A-2 (id 44); both outputs verified
  with the test pattern, via automatic selection and explicit connector-id
- 4 CRTCs (ids 59, 78, 94, 106); 56 planes, all sharing one identical
  format list; 16 overlay planes (ids 107-272, possible CRTCs 0xE), e.g.
  plane 107 is usable for video
- NV12/NV21 and NV16/NV61 supported everywhere (LINEAR plus Broadcom
  SAND64/128/256 tiling modifiers), along with the usual RGB32 variants
- **No packed 4:2:2 on any plane** (no YUYV/UYVY/YVYU/VYUY): captured YUY2
  frames cannot be scanned out directly — confirmed by the plane format
  lists and empirically (a YUY2 test-pattern run fails with not-negotiated
  while NV16 displays). The output pipeline therefore converts YUY2 to
  NV16 in software (videoconvert, #366), and the full passthrough displays
  live video on the Pi (verified on the appliance).

## PiSP hardware converter (pispconvert)

The hardware-accelerated conversion/scaling element for the `kms-pisp` output
path (see docs/video-output.md) is installed with:

```sh
sudo apt install gstreamer1.0-pispconvert
```

It exists on the Pi 5 despite widespread claims to the contrary — confirmed via
`gst-inspect-1.0 pispconvert`:

- Factory "PiSP Hardware Image Converter", `primary` rank, v1.4.0,
  `libgstpispconvert.so`, from Raspberry Pi's libpisp
- Two Always scaling source pads (`src0`/`src1`) with per-output crop
  (`crop0`/`crop1`), so the HDMI + lores-preview dual output is a single-pass
  hardware capability
- Emits **NV12** including the SAND-tiled DMABuf variant
  (`NV12:0x0700000000000004`) for zero-copy into kmssink; it **cannot** emit
  NV16, so the hardware path uses NV12 while the software reference stays NV16
- Negotiation is capability, not fact: validate an actual link with
  `gst-launch-1.0 -v` and read the caps/modifier printed after PAUSED

Hardware-path format choice (from `modetest -M vc4 -p`): every plane scans out
NV12 (LINEAR + Broadcom SAND) and YU16 (planar 4:2:2, **LINEAR only** — no
tiling), and pispconvert can emit both. NV16/NV61 are in the plane list but
pispconvert cannot produce them, so semi-planar 4:2:2 is unreachable.

- **NV12 SAND-tiled** (default): zero-copy and tiled (lowest scanout DRAM
  bandwidth), 2 planes, but 4:2:0 — half the vertical chroma. The CV105
  captures upstream content that is usually already 4:2:0, so this loss is
  mostly academic.
- **YU16 LINEAR** (lossless alternate): full 4:2:2, still zero-copy DMABuf, but
  untiled (~33% more scanout bandwidth) and 3 planes. Worth selecting only if a
  visual A/B shows NV12 softening sharp colored subtitle text (the overlay
  composites at 4:2:2 before conversion).

## Stability

The passthrough ran for 30 minutes on the appliance with 0 dropped frames
(#58): CV105 connectivity, output stability, and bounded latency verified
in one run.
