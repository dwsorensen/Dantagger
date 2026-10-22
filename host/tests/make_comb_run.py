# Synthetic run file: 1 kHz square wave into STOP1 and STOP2 (STOP2 96.455 ns later, 3 ps rms jitter between them),
# each time rounded to a 44.12 ps grid from its 125 ns reference edge (the GPX2's internal step).
import random, struct, sys
random.seed(5)
Q, REF = 44.12, 125000.0
out = []
def marker(kind, coarse, info=0): out.append((1 << 63) | (kind << 60) | (info << 48) | coarse)
def event(ch, t_ps):
    ref = int(t_ps // REF); frac = t_ps - ref * REF
    stop = min(int(round(round(frac / Q) * Q)), 124999)
    coarse = int(t_ps // 10000) + 50                  # frame received ~500 ns later
    out.append((ch << 61) | ((coarse & 0xFFFF) << 44) | ((ref & 0xFFFFFF) << 20) | stop)
    return coarse
marker(0, 0)
next_time = 1 << 14
for k in range(5000):
    t1 = k * 1e9 + random.uniform(0, REF) + 1e6
    t2 = t1 + 96455 + random.gauss(0, 3)
    c = int(t1 // 10000) + 50
    while next_time <= c:
        marker(1, next_time); next_time += 1 << 14
    event(0, t1); event(1, t2)
marker(3, next_time, 1)
open(sys.argv[1], 'wb').write(struct.pack('<%dQ' % len(out), *out))
print(len(out), "records")
