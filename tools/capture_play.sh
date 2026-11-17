#!/bin/bash
# While someone plays: a frame dump and a screenshot of both eyes every half second.
# Usage: capture_play.sh <seconds> [prefix]   Output: scratch/<prefix>_N.png and the game
# folder's gw3vr_dump_N.log (the two share N, so a picture can be matched to its draw calls).
G="/h/SteamLibrary/steamapps/common/Geometry Wars 3 - Dimensions"
P=/h/gw3-vr/tools/pad.sh
SECS=${1:-30}
N=${2:-cap}
rm -f "$G"/gw3vr_dump_*.log
for i in $(seq 0 $((SECS * 2 - 1))); do
  touch "$G/gw3vr_dump.req"
  bash $P shot ${N}_$i >/dev/null 2>&1
  sleep 0.3
done
ls "$G" | grep -c gw3vr_dump_
