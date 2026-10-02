#!/usr/bin/env python3
"""Run every example design and compare against its `// expect:` line.

designs/good/*.v must come back with no errors or warnings.
designs/bad/*.v must report the expected rule(s) and nothing else.
A NAME.cg.json next to a design is passed as its constraints.
"""
import argparse
import glob
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import clockguard  # noqa: E402


def expected(path):
    with open(path) as f:
        for line in f:
            m = re.match(r"//\s*expect:\s*(.+)", line)
            if m:
                return {r.strip() for r in m.group(1).split(",")} - {"clean"}
    raise SystemExit(f"{path}: no '// expect:' line")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine")
    ap.add_argument("designs")
    a = ap.parse_args()
    lib = sorted(glob.glob(os.path.join(a.designs, "lib", "*.v")))
    failures = 0
    for path in sorted(glob.glob(os.path.join(a.designs, "good", "*.v")) +
                       glob.glob(os.path.join(a.designs, "bad", "*.v"))):
        top = os.path.splitext(os.path.basename(path))[0]
        cons = os.path.splitext(path)[0] + ".cg.json"
        want = expected(path)
        try:
            rep = clockguard.analyze([path] + lib, top, cons if os.path.exists(cons) else None, engine=a.engine)
        except clockguard.FlowError as e:
            print(f"FAIL {top}: {e}")
            failures += 1
            continue
        got = {v["rule"] for v in rep["violations"] if "waived" not in v}
        ok = got == want
        failures += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {top}: expected {sorted(want) or 'clean'}, got {sorted(got) or 'clean'}")
        if not ok:
            for v in rep["violations"]:
                print(f"       {v['severity']}[{v['rule']}] {v['message']}")
    print(f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
