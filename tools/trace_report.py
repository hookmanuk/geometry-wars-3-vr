"""Reads a gw3vr_trace_N.csv and reports frame delivery, pacing and whether the game's world
advanced every frame.  Usage: python trace_report.py <trace.csv>"""
import csv
import sys


def pct(v, p):
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * p))]


rows = [r for r in csv.DictReader(open(sys.argv[1])) if float(r['period_ms']) > 0]
f = lambda k: [float(r[k]) for r in rows]
n = len(rows)
start, disp, period = f('start_ms'), f('display_ms'), f('period_ms')
interval = [b - a for a, b in zip(start, start[1:])]
dstep = [b - a for a, b in zip(disp, disp[1:])]
p = period[0]
print('frames %d, display period %.3f ms (%.1f Hz)' % (n, p, 1000 / p))
print('frame start interval: avg %.2f  min %.2f  max %.2f  p1 %.2f  p99 %.2f' % (
    sum(interval) / len(interval), min(interval), max(interval), pct(interval, .01), pct(interval, .99)))
skips = [round(d / p) for d in dstep]
hist = {}
for s in skips:
    hist[s] = hist.get(s, 0) + 1
print('predicted display time steps (in periods):', dict(sorted(hist.items())))
for k in ('game_ms', 'flush_ms', 'copy_ms', 'endframe_ms', 'present_ms', 'wait_ms', 'begin_ms', 'locate_ms'):
    v = f(k)
    print('%-12s avg %6.2f  p99 %6.2f  max %6.2f' % (k, sum(v) / n, pct(v, .99), max(v)))

# Did the world move between consecutive frames?
same_world = sum(1 for a, b in zip(rows, rows[1:]) if a['world_hash'] == b['world_hash'])
cam = [(float(r['cam_x']), float(r['cam_y']), float(r['cam_z'])) for r in rows]
same_cam = sum(1 for a, b in zip(cam, cam[1:]) if a == b)
print('frames where every object transform repeated the previous frame: %d of %d' % (same_world, n - 1))
print('frames where the camera position repeated the previous frame:   %d of %d' % (same_cam, n - 1))
# Camera step sizes: a game ticking at a lower rate shows a pattern of zero and double steps.
steps = [sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5 for a, b in zip(cam, cam[1:])]
moving = [s for s in steps if s > 0]
if moving:
    avg = sum(moving) / len(moving)
    print('camera step: avg %.3f, ratio max/avg %.2f, min/avg %.2f' % (avg, max(moving) / avg, min(moving) / avg))
    print('first 40 camera steps:', ' '.join('%.2f' % s for s in steps[:40]))
