#!/usr/bin/env python3
"""Score clockguard against generated designs (tools/gen.py).

    python3 tools/bench.py --designs bench/designs --jobs 12 --out results/bench.json [--sim]

For a clean design, any violation is a false alarm. For a buggy design the
bug counts as detected only if the expected rule fires *at the block the
bug was put in*; the same rule somewhere else is "wrong place". Everything
else the tool reports on a buggy design is listed as an extra diagnostic.

With --sim, the detected violation's targeted testbench is run too.
"""
import argparse
import json
import os
import statistics
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import clockguard  # noqa: E402
import simgen  # noqa: E402


def names(v):
    out = []
    for n in (v["from"], v["to"]):
        out += [x.strip() for x in n.replace(",", " ").split()]
    return out


def located(v, where):
    return any(n.startswith(w) for n in names(v) for w in where)


def label(v):
    return f"{v['rule']}: {v['from']} -> {v['to']}"


def pct(a, b):
    return round(100.0 * a / b, 1) if b else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--designs", required=True)
    ap.add_argument("--lib", default=os.path.join(ROOT, "designs", "lib", "blocks.v"))
    ap.add_argument("--engine")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--sim", action="store_true")
    ap.add_argument("--sim-ns", type=float, default=20000)
    ap.add_argument("--out")
    a = ap.parse_args()
    with open(os.path.join(a.designs, "truth.json")) as f:
        truth = json.load(f)

    def one(t):
        path = os.path.join(a.designs, t["design"] + ".v")
        t0 = time.time()
        try:
            rep, net = clockguard.run([path, a.lib], t["design"], engine=a.engine)
        except clockguard.FlowError as e:
            return {"design": t["design"], "bug": t["bug"], "status": "flow_error", "error": str(e)}
        res = {"design": t["design"], "bug": t["bug"], "rule": t["rule"], "seconds": round(time.time() - t0, 3),
               "flop_bits": rep["summary"]["flop_bits"],
               "domains": sum(d["kind"] != "async" for d in rep["domains"])}
        viols = [v for v in rep["violations"] if "waived" not in v]
        if not t["rule"]:
            res["status"] = "clean" if not viols else "false_alarm"
            res["extra"] = [label(v) for v in viols]
            return res
        hits = [v for v in viols if v["rule"] == t["rule"]]
        loc = [v for v in hits if located(v, t["where"])]
        res["status"] = "detected" if loc else "wrong_place" if hits else "missed"
        res["extra"] = [label(v) for v in viols if v not in loc]
        if loc:
            v = loc[0]
            res["path_nets"], res["cone_nets"] = v["path_nets"], v["cone_nets"]
            if a.sim:
                with tempfile.TemporaryDirectory() as tmp:
                    i = rep["violations"].index(v)
                    s = simgen.run_one(i, v, rep, net["modules"][t["design"]]["ports"], [path, a.lib], t["design"],
                                       {}, tmp, a.sim_ns)
                res["sim"] = {k: s.get(k) for k in ("status", "hazards", "cycles_to_first", "summary")}
        return res

    t0 = time.time()
    with ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(one, truth))
    wall = time.time() - t0

    clean = [r for r in rows if r.get("bug") is None]
    buggy = [r for r in rows if r.get("bug") is not None]
    fa_rules = {}
    for r in clean:
        for x in r.get("extra", []):
            k = x.split(":")[0]
            fa_rules[k] = fa_rules.get(k, 0) + 1
    by_bug = {}
    for r in buggy:
        b = by_bug.setdefault(r["bug"], {"rule": r.get("rule"), "n": 0, "detected": 0, "wrong_place": 0, "missed": 0,
                                         "flow_error": 0, "with_extra": 0, "sim_observed": 0, "sim_run": 0,
                                         "cycles": []})
        b["n"] += 1
        b[r["status"]] += 1
        b["with_extra"] += bool(r.get("extra"))
        if "sim" in r:
            b["sim_run"] += 1
            if r["sim"]["status"] == "observed":
                b["sim_observed"] += 1
                b["cycles"].append(r["sim"]["cycles_to_first"])
    for b in by_bug.values():
        c = b.pop("cycles")
        b["median_cycles_to_hazard"] = statistics.median(c) if c else None

    det = [r for r in buggy if r["status"] == "detected"]
    # Only for data crossings: without the report you'd search the
    # destination's fan-in cone for the source. A reset pin's cone is
    # usually one net, so the comparison means nothing there.
    cdc = [r for r in det if r["rule"].startswith("CDC_") and r.get("cone_nets")]
    ratios = [r["path_nets"] / r["cone_nets"] for r in cdc]
    secs = sorted(r["seconds"] for r in rows if "seconds" in r)
    sim_rows = [r for r in det if "sim" in r]
    summary = {
        "designs": len(rows), "clean": len(clean), "buggy": len(buggy),
        "flow_errors": sum(r["status"] == "flow_error" for r in rows),
        "false_alarm_designs": sum(r["status"] == "false_alarm" for r in clean),
        "false_alarms_by_rule": fa_rules,
        "detected": len(det), "detection_rate_pct": pct(len(det), len(buggy)),
        "wrong_place": sum(r["status"] == "wrong_place" for r in buggy),
        "missed": sum(r["status"] == "missed" for r in buggy),
        "buggy_with_extra_diagnostics": sum(bool(r.get("extra")) for r in buggy),
        "debug_scope_cdc": {
            "violations": len(cdc),
            "median_path_nets": statistics.median(r["path_nets"] for r in cdc) if cdc else None,
            "median_cone_nets": statistics.median(r["cone_nets"] for r in cdc) if cdc else None,
            "median_path_over_cone_pct": round(100 * statistics.median(ratios), 2) if ratios else None,
            "mean_reduction_pct": round(100 * (1 - statistics.mean(ratios)), 1) if ratios else None,
        },
        "simulation": {
            "run": len(sim_rows),
            "observed": sum(r["sim"]["status"] == "observed" for r in sim_rows),
            "observed_pct": pct(sum(r["sim"]["status"] == "observed" for r in sim_rows), len(sim_rows)),
        } if sim_rows else None,
        "seconds_per_design": {"median": statistics.median(secs), "max": secs[-1]} if secs else None,
        "median_flop_bits": statistics.median(r["flop_bits"] for r in rows if "flop_bits" in r),
        "wall_seconds": round(wall, 1),
        "by_bug": by_bug,
    }
    print(json.dumps(summary, indent=2))
    for r in rows:
        if r["status"] in ("false_alarm", "wrong_place", "missed", "flow_error"):
            print(r["status"].upper(), r["design"], r.get("extra") or r.get("error"))
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"summary": summary, "rows": rows}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
