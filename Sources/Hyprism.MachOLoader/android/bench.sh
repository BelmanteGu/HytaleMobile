#!/bin/sh
# Copyright (C) 2026 Hyprism Launcher
# SPDX-License-Identifier: GPL-3.0-only
#
# Frame-rate benchmark for a phone that is already inside a world.
#
#   android/bench.sh [label] [seconds]
#
# Walks forward with the left half of the screen and sweeps the camera with the right half, then
# prints the loader's fps lines and their mean. Needs HYPRISM_PERF=1 in <files>/env.txt (the loader
# then logs "[perf] fps ..." every two seconds). Turn the on-screen PAD off first if the swipes should
# act as a mouse; with PAD on they drive the virtual stick. Coordinates are for a 2340x1080 landscape
# phone (Galaxy S23); set WALK and LOOK_A/LOOK_B for another screen.
#
# Compare runs only under the same conditions: same place in the world, battery saver off, and a phone
# at a similar temperature (let it cool between runs).
set -eu
LABEL=${1:-run}
SECS=${2:-30}
WALK=${WALK:-"300 800 300 420"}
LOOK_A=${LOOK_A:-"1300 600 1900 600"}
LOOK_B=${LOOK_B:-"1900 600 1300 600"}
GPU=/sys/class/kgsl/kgsl-3d0/devfreq

adb logcat -c
adb shell input swipe $WALK $((SECS * 1000 + 2000)) &
sleep 2
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
    adb shell input swipe $LOOK_A 1200
    adb shell input swipe $LOOK_B 1200
done
wait

echo "== $LABEL  battery saver: $(adb shell settings get global low_power | tr -d '\r')," \
     "GPU clock now/max: $(adb shell cat $GPU/cur_freq 2>/dev/null | tr -d '\r')/$(adb shell cat $GPU/max_freq 2>/dev/null | tr -d '\r')"
adb logcat -d -s HytaleLoader:I | grep "\[perf\]" | cut -c40- | awk '
    { print }
    { for (i = 1; i <= NF; i++) if ($i == "fps") { s += $(i + 1); n++; if (min == 0 || $(i + 1) < min) min = $(i + 1) }
      for (i = 1; i <= NF; i++) if ($i == "max") { if ($(i + 1) > worst) worst = $(i + 1) } }
    END { if (n) printf "mean fps %.1f   lowest 2 s window %.1f fps   worst frame %.0f ms   (%d windows)\n", s / n, min, worst, n
          else print "no [perf] lines: set HYPRISM_PERF=1 in env.txt and restart the game" }'
