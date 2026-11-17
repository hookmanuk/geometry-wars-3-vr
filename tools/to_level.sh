#!/bin/bash
# Launches the game and drives the menus into the first Adventure level with the injected pad.
# Screenshots of each stage go to scratch/<prefix>_*.png.  Usage: to_level.sh [prefix] [stage]
# stage: title | select | tutorial | level (default level) - where to stop.
P=/h/gw3-vr/tools/pad.sh
G="/h/SteamLibrary/steamapps/common/Geometry Wars 3 - Dimensions"
N=${1:-run}
STOP=${2:-level}
bash $P zero
powershell -NoProfile -File 'H:\gw3-vr\tools\gw.ps1' launch
# The title is up once the game has re-made its targets after the window was placed.
for i in $(seq 1 60); do grep -q "ResetEx" "$G/gw3vr.log" 2>/dev/null && break; sleep 0.5; done
sleep 7
bash $P tap A; sleep 1
bash $P shot ${N}_title
[ "$STOP" = title ] && exit 0
bash $P tap A; sleep 2
bash $P tap A; sleep 3
bash $P shot ${N}_select
[ "$STOP" = select ] && exit 0
bash $P tap A; sleep 2.5
bash $P tap A; sleep 6
[ "$STOP" = tutorial ] && exit 0
bash $P tap A; sleep 1
bash $P shot ${N}_level
