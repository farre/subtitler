#!/bin/sh
# Generates the flash-and-click A/V sync test clip (#133): a full-screen
# white flash (3 frames, 50 ms) and a 1 kHz beep (125 ms) every second,
# both aligned at the same timestamps. PCM audio keeps the click onset
# sample-exact (no codec priming delay).
#
# Play it fullscreen on the machine feeding the CV105's HDMI input, with
# audio routed to that HDMI output.
set -eu

output=${1:-sync-test.mkv}

ffmpeg -y \
    -f lavfi -i "color=black:size=1920x1080:rate=60,drawbox=c=white:t=fill:enable='lt(mod(n\,60)\,3)'" \
    -f lavfi -i "aevalsrc=exprs='sin(2*PI*1000*t)*lt(mod(t\,1)\,0.125)':s=48000" \
    -t 60 \
    -c:v libx264 -preset veryfast -crf 18 -pix_fmt yuv420p \
    -c:a pcm_s16le \
    "$output"
