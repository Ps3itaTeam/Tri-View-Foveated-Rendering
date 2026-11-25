# model of CameraDuplicator::apply_tone_test's calcRealEV schedule (fix105): calls per frame for a steady and a moving value
import sys

def run(value_at, frames, steady_opt=True, mode_at=lambda f: 0):
    interval, since, prev, pending = 4, 1000, -1000.0, False
    seen_mode = None
    own_zero = [0, 0]; wait = [0, 0]
    real_frame = 0
    per_frame = []
    main_calls = clone_calls = 0
    last_main = None
    worst_gap = 0
    for f in range(frames):
        calls = 0
        mode = mode_at(f)
        mode_changed = mode != seen_mode
        seen_mode = mode
        since = min(since + 1, 1000)
        pending = pending or mode_changed
        main_calc = since >= interval or (pending and since >= 4)
        clone_tick = real_frame % 4 == 0
        real_frame += 1
        if main_calc:
            calls += 1; main_calls += 1
            now = value_at(f)
            steady = steady_opt and not pending and abs(now - prev) < 0.002
            interval = 8 if steady else 4
            prev = now; since = 0; pending = False
            if last_main is not None:
                worst_gap = max(worst_gap, f - last_main)
            last_main = f
        settled_measured = False
        for i in range(2):
            measure = False
            if own_zero[i] < 8:
                measure = clone_tick
            elif wait[i] > 0:
                wait[i] -= 1
            else:
                measure = (not main_calc) and (not settled_measured)
            if measure:
                was = own_zero[i] >= 8
                calls += 1; clone_calls += 1
                own_zero[i] = min(own_zero[i] + 1, 8)
                if own_zero[i] >= 8:
                    wait[i] = 128 if was else 64 + 64 * (i % 2)
                    settled_measured = settled_measured or was
        per_frame.append(calls)
    return main_calls, clone_calls, per_frame, worst_gap

for name, fn in (('steady value', lambda f: 7.5), ('moving value', lambda f: 5.0 + 0.01 * f),
                 ('steady, then moving from frame 2000', lambda f: 7.5 if f < 2000 else 7.5 - 0.008 * (f - 2000))):
    m, c, pf, gap = run(fn, 4500)
    tail = pf[450:]
    print(f'{name}: 4500 frames: game camera {m} calls, fovea views {c}; after the first 450 frames: '
          f'{sum(tail) / len(tail) * 0.7:.3f} ms per frame at 0.7 ms a call, at most {max(tail)} calls in one frame, '
          f'longest gap between game camera calls {gap} frames')
m, c, pf, gap = run(lambda f: 7.5, 4500, steady_opt=False)
print(f'steady value, option off: game camera {m} calls, fovea views {c}, {sum(pf[450:]) / len(pf[450:]) * 0.7:.3f} ms per frame')
m, c, pf, gap = run(lambda f: 7.5 if (f // 900) % 2 == 0 else 3.5, 4500, mode_at=lambda f: (f // 900) % 2)
first = [f for f in range(900, 908) if pf[f] > 0]
print(f'mode change every 900 frames: game camera {m} calls; first call after the change at frame 900: frame {first[0] if first else None}')
print(f'before fix105: 3 calls every 4th frame = {3 * 0.7 / 4:.3f} ms per frame, 3 calls in one frame')
