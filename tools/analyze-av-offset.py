#!/usr/bin/env python3
"""A/V offset analysis for a recording of the sync-test clip (#133).

Usage: analyze-av-offset.py <recording>

Finds white-flash onsets in the video stream (per-frame mean luma with
partial-exposure refinement) and 1 kHz beep onsets in the audio stream
(10 ms RMS windows), then prints the per-cycle (beep - flash) offsets
and their median. Works for phone recordings of a screen as well as
direct v4l2+alsa capture recordings.
"""

import json
import re
import struct
import subprocess
import sys
import tempfile
import wave

rec = sys.argv[1]
tmp = tempfile.mkdtemp()


def start_time(selector):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", selector,
         "-show_entries", "stream=start_time", "-of", "json", rec],
        capture_output=True, text=True, check=True).stdout
    return float(json.loads(out)["streams"][0].get("start_time", 0))


video_start = start_time("v:0")
audio_start = start_time("a:0")

# Per-frame mean luma.
yavg_path = f"{tmp}/yavg.txt"
subprocess.run(
    ["ffmpeg", "-v", "error", "-i", rec, "-vf",
     f"signalstats,metadata=print:key=lavfi.signalstats.YAVG:file={yavg_path}",
     "-f", "null", "-"], check=True)
frames = []
pts = None
for line in open(yavg_path):
    m = re.search(r"pts_time:([\d.]+)", line)
    if m:
        pts = float(m.group(1)) + video_start
    elif line.startswith("lavfi.signalstats.YAVG="):
        frames.append((pts, float(line.split("=")[1])))

baseline = min(y for _, y in frames)
peak = max(y for _, y in frames)
mid = baseline + (peak - baseline) / 2
flashes = []
prev_white = False
for i, (t, y) in enumerate(frames):
    white = y > mid
    if white and not prev_white:
        frac = (y - baseline) / (peak - baseline)
        period = frames[i + 1][0] - t if i + 1 < len(frames) else 1 / 60
        flashes.append(t + period * (1 - min(frac, 1.0)))
    prev_white = white

# Beep onsets from the audio envelope (10 ms windows).
wav_path = f"{tmp}/audio.wav"
subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", rec, "-map", "0:a:0",
                "-f", "wav", wav_path], check=True)
with wave.open(wav_path) as w:
    rate, channels = w.getframerate(), w.getnchannels()
    raw = w.readframes(w.getnframes())
s = struct.unpack("<%dh" % (len(raw) // 2), raw)[::channels]
win = rate // 100
env = [(sum(x * x for x in s[i:i + win]) / win) ** 0.5
       for i in range(0, len(s) - win, win)]
floor = sorted(env)[len(env) // 2]
threshold = max(floor * 4, 100)
clicks = [i / 100 + audio_start for i, rms in enumerate(env)
          if rms > threshold and (i == 0 or env[i - 1] <= threshold)]

offsets = []
for c in clicks:
    f = max((f for f in flashes if f < c), default=None)
    if f is not None and c - f < 1.0:
        offsets.append(c - f)
offsets.sort()
mid_i = len(offsets) // 2
median = (offsets[mid_i] if len(offsets) % 2
          else (offsets[mid_i - 1] + offsets[mid_i]) / 2) if offsets else 0
print(f"luma baseline {baseline:.1f}, peak {peak:.1f}")
print(f"flashes: {len(flashes)}, clicks: {len(clicks)}")
print("offsets (ms):", [round(o * 1000) for o in offsets])
print(f"median (beep - flash): {median * 1000:+.1f} ms")
