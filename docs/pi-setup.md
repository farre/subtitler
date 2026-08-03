# Pi appliance setup and hardware validation

Verified state of the subtitler appliance as of 2026-08-02: provisioning
complete, and the CV105 capture device validated for 1080p60 capture. The
long-running stability soak and DRM/KMS output validation are still pending
(see the last section).

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

Verified with `modetest`/`gst-inspect-1.0` on the vc4 driver (no display
attached yet):

- kmssink is installed (gst-plugins-bad 1.26.2)
- Connectors: HDMI-A-1 (id 35) and HDMI-A-2 (id 44)
- 4 CRTCs (ids 59, 78, 94, 106); 56 planes, all sharing one identical
  format list; 16 overlay planes (ids 107-272, possible CRTCs 0xE), e.g.
  plane 107 is usable for video
- NV12/NV21 and NV16/NV61 supported everywhere (LINEAR plus Broadcom
  SAND64/128/256 tiling modifiers), along with the usual RGB32 variants
- **No packed 4:2:2 on any plane** (no YUYV/UYVY/YVYU/VYUY): captured YUY2
  frames cannot be scanned out directly — kmssink intersects its caps with
  the plane formats at runtime. The pipeline needs a conversion step (e.g.
  YUY2 to NV16 via videoconvert, or hardware-accelerated via the
  pispbe/v4l2convert)

## Pending validation

- Long-running USB stability soak (#58): 60 minutes of 1080p60 YUYV capture
  to /dev/null
- Live DRM/KMS display checks (#5: #71-#74, #80, #81): kmssink test
  pattern, connected-connector identification, both HDMI outputs — needs a
  display attached. Include this controlled pair to empirically confirm
  that packed 4:2:2 scanout is impossible (expect YUY2 to fail negotiation,
  NV16 to display):

  ```sh
  gst-launch-1.0 videotestsrc ! video/x-raw,format=YUY2,width=1920,height=1080 ! kmssink driver-name=vc4 force-modesetting=true
  gst-launch-1.0 videotestsrc ! video/x-raw,format=NV16,width=1920,height=1080 ! kmssink driver-name=vc4 force-modesetting=true
  ```
