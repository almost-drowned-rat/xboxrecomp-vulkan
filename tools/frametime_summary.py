"""frametime_summary.py <frametime.csv> [--skip-ms N] [--json]

Average / p50 / p99 per column of a RECOMP_FRAME_TIME file (src/kernel/frame_time.h).
--skip-ms drops frames before that run time (loading, the first shader compiles).
"""
import csv
import json
import sys

COLS = ["frame_ms", "game_ms", "translate_ms", "gpu_wait_ms", "present_ms", "gpu_ms", "input_ms", "acquire_ms"]


def pct(v, p):
    if not v:
        return 0.0
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p / 100.0 * (len(v) - 1))))]


def summarize(path, skip_ms=0.0):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if float(r["t_ms"]) >= skip_ms:
                rows.append(r)
    out = {"frames": len(rows)}
    for c in COLS:
        # input_ms / acquire_ms: older files lack them; input_ms -1 = no pad read that frame
        v = [float(r[c]) for r in rows if r.get(c) not in (None, "") and float(r[c]) >= 0]
        out[c] = {"avg": sum(v) / len(v) if v else 0.0, "p50": pct(v, 50), "p99": pct(v, 99)}
    if rows:
        span = float(rows[-1]["t_ms"]) - float(rows[0]["t_ms"])
        out["fps"] = (len(rows) - 1) * 1000.0 / span if span > 0 else 0.0
    return out


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    skip = 0.0
    if "--skip-ms" in args:
        skip = float(args[args.index("--skip-ms") + 1])
    s = summarize(args[0], skip)
    if "--json" in args:
        print(json.dumps(s, indent=1))
        return 0
    print("%d frames, %.1f fps" % (s["frames"], s.get("fps", 0.0)))
    for c in COLS:
        print("  %-13s avg %7.2f  p50 %7.2f  p99 %7.2f" % (c, s[c]["avg"], s[c]["p50"], s[c]["p99"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
