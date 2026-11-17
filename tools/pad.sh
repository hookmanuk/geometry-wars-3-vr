#!/bin/bash
# Drives the game's controller through the proxy's pad file, and takes in-game screenshots.
#   pad.sh tap A [B ...]        press buttons one after another (0.6 s apart)
#   pad.sh set <buttons> lx ly rx ry lt rt     hold a raw state (buttons in hex)
#   pad.sh zero                 hold a neutral pad (the real controller stays ignored)
#   pad.sh off                  remove the pad file: the real controller works again
#   pad.sh shot <name> [full]   screenshot of both eyes to scratch/<name>.png
G="/h/SteamLibrary/steamapps/common/Geometry Wars 3 - Dimensions"
S="/h/gw3-vr/scratch"
declare -A B=( [A]=1000 [B]=2000 [X]=4000 [Y]=8000 [START]=0010 [BACK]=0020 [UP]=0001 [DOWN]=0002 [LEFT]=0004 [RIGHT]=0008 [LB]=0100 [RB]=0200 [L3]=0040 [R3]=0080 )
# The game may be reading the file at the moment of the rename; retry until it lands.
put() {
  printf '%s' "$1" > "$G/gw3vr_pad.tmp"
  for i in 1 2 3 4 5 6 7 8 9 10; do mv -f "$G/gw3vr_pad.tmp" "$G/gw3vr_pad.txt" 2>/dev/null && return 0; sleep 0.01; done
  echo "pad: could not update the pad file" >&2; return 1
}
case "$1" in
  tap)
    shift
    for b in "$@"; do put "${B[$b]} 0 0 0 0 0 0"; sleep 0.15; put "0 0 0 0 0 0 0"; sleep 0.6; done ;;
  set) shift; put "$*" ;;
  zero) put "0 0 0 0 0 0 0" ;;
  off) rm -f "$G/gw3vr_pad.txt" ;;
  shot)
    rm -f "$S/$2.bmp"
    printf '%s' "H:/gw3-vr/scratch/$2.bmp $3" > "$G/gw3vr_shot.tmp" && mv -f "$G/gw3vr_shot.tmp" "$G/gw3vr_shot.req"
    for i in $(seq 1 40); do [ -f "$S/$2.bmp" ] && break; sleep 0.1; done
    python -c "
from PIL import Image
im=Image.open('H:/gw3-vr/scratch/$2.bmp').convert('RGB')
w=1600 if '$3'!='full' else im.size[0]
im.resize((w, im.size[1]*w//im.size[0])).save('H:/gw3-vr/scratch/$2.png')
print(im.size)" && rm -f "$S/$2.bmp" ;;
esac
