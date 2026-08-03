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

## Pending validation

- Long-running USB stability soak (#58): 30 minutes of 1080p60 YUYV capture
  to /dev/null
- DRM/KMS output validation (#5, issues #71-#82): kmssink presence and test
  pattern, connector IDs, plane format support — needs a display attached
