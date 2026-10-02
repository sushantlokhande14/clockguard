#!/usr/bin/env python3
"""Control runs: the simulation monitors on structures that are correct.

    python3 tools/control.py --designs bench/designs --limit 100 --out results/control.json

A monitor that fires on everything is useless, so this attaches the same
monitors the violation testbenches use to the *correct* structures in
clean designs and counts hazards. Expected: none.

  glitch  on every gated clock (latch-based or negedge-flop gates)
  reset   on every synchronized reset, against its own clock
  bus     on every gray-coded bus, at the synchronizer's clock
"""
import argparse
import json
import os
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
sys.path.insert(0, ROOT)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import clockguard  # noqa: E402
import simgen  # noqa: E402


def probes(rep):
    out = []
    for d in rep["domains"]:
        for g in d["gates"]:
            out.append({"rule": "control_glitch", "monitor": {"type": "glitch", "net": g, "clock": d["name"]}})
    for r in rep["resets"]:
        if r["kind"] == "synchronizer" and r["domain"]:
            out.append({"rule": "control_reset",
                        "monitor": {"type": "reset", "reset": r["name"], "clock": r["domain"], "active_low": True}})
    for y in rep["synchronizers"]:
        if y["kind"] == "gray-bus":
            src = [f"{y['from']}[{k}]" for k in range(y["bits"])]
            out.append({"rule": "control_bus", "monitor": {"type": "bus", "src": src, "dst": [y["to"]],
                                                            "clock": y["to_domain"]}})
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--designs", required=True)
    ap.add_argument("--lib", default=os.path.join(ROOT, "designs", "lib", "blocks.v"))
    ap.add_argument("--engine")
    ap.add_argument("--limit", type=int, default=100)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--sim-ns", type=float, default=20000)
    ap.add_argument("--out")
    a = ap.parse_args()
    with open(os.path.join(a.designs, "truth.json")) as f:
        clean = [t for t in json.load(f) if t["bug"] is None][:a.limit]

    def one(t):
        path = os.path.join(a.designs, t["design"] + ".v")
        rep, net = clockguard.run([path, a.lib], t["design"], engine=a.engine)
        ports = net["modules"][t["design"]]["ports"]
        rows = []
        with tempfile.TemporaryDirectory() as tmp:
            for i, p in enumerate(probes(rep)):
                s = simgen.run_one(i, p, rep, ports, [path, a.lib], t["design"], {}, tmp, a.sim_ns)
                rows.append({"design": t["design"], "probe": p["rule"], "status": s["status"],
                             "hazards": s.get("hazards", 0), "summary": s["summary"]})
        return rows

    with ThreadPoolExecutor(a.jobs) as ex:
        rows = [r for rs in ex.map(one, clean) for r in rs]
    summary = {}
    for r in rows:
        s = summary.setdefault(r["probe"], {"probes": 0, "quiet": 0, "fired": 0, "skipped": 0})
        s["probes"] += 1
        s[{"not observed": "quiet", "observed": "fired"}.get(r["status"], "skipped")] += 1
    print(json.dumps({"designs": len(clean), "by_probe": summary}, indent=2))
    for r in rows:
        if r["status"] != "not observed":
            print(r["status"].upper(), r["design"], r["probe"], r["summary"])
    if a.out:
        with open(a.out, "w") as f:
            json.dump({"designs": len(clean), "by_probe": summary, "rows": rows}, f, indent=1)


if __name__ == "__main__":
    main()
