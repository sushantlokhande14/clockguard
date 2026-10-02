"""Targeted testbenches for clockguard violations.

RTL simulation has no metastability, so a CDC bug never fails a normal
testbench. What a testbench *can* do is show the moment the hardware would
be at risk. For each violation this writes a testbench that:

  - runs every clock at an unrelated period, so edges drift past each other;
  - drives each data input in the domain that samples it, and async inputs
    at random times;
  - for reset problems, releases (or asserts) the reset just before a
    capture edge, again and again;
  - watches the nets the violation names and reports a hazard when
      capture: a source changes less than WINDOW ns before a capture edge,
      bus:     two or more bits of a bus change inside that window,
      reset:   a reset releases inside that window,
      glitch:  a clock (or reset) pulse is shorter than 40% of its period;
  - dumps those nets so the first hazard can be drawn.
"""
import os
import re
import subprocess

import vcd

WINDOW = 0.5  # ns
DEFAULT_PERIODS = [10.0, 7.3, 13.1, 8.9, 11.7, 6.1]
PHASES = [0.0, 1.37, 2.71, 0.53, 3.19, 1.91]
PS = 1000  # VCD units per ns (`timescale 1ns/1ps)


class Ref:
    """How the testbench refers to a net named in the report."""

    def __init__(self, name, ports):
        m = re.match(r"^(.*?)(\[\d+\])?$", name)
        self.base, self.idx = m.group(1), m.group(2) or ""
        self.ok = bool(re.match(r"^[A-Za-z_][A-Za-z0-9_$]*(\.[A-Za-z_][A-Za-z0-9_$]*)*$", self.base))
        self.port = self.base in ports
        self.expr = (self.base if self.port else "dut." + self.base) + self.idx
        self.dump = "tb." + self.base if self.port else "tb.dut." + self.base


def rnd(width):
    words = max(1, (width + 31) // 32)
    return "{" + ", ".join(["$random(seed)"] * words) + "}" if words > 1 else "$random(seed)"


def testbench(v, rep, ports, top, cons, sim_ns, vcd_path):
    mon = v["monitor"]
    kind = mon["type"]
    inputs = rep["inputs"]
    clocks = sorted(p for p, j in inputs.items() if j["kind"] == "clock")
    period, phase = {}, {}
    for k, c in enumerate(clocks):
        period[c] = float(cons.get("clocks", {}).get(c, {}).get("period") or DEFAULT_PERIODS[k % len(DEFAULT_PERIODS)])
        phase[c] = PHASES[k % len(PHASES)]
    resets = {p: j.get("active_low", True) for p, j in inputs.items() if j["kind"] == "reset"}
    maxp = max(period.values(), default=10.0)
    release = round(4 * maxp + 0.37, 3)

    clk = Ref(mon.get("clock", clocks[0] if clocks else ""), ports)
    clk_p = period.get(clk.base, min(period.values(), default=10.0))
    edge = mon.get("edge", "posedge")
    dumps = [Ref(c, ports) for c in clocks] + [Ref(r, ports) for r in resets]

    L = [f"// clockguard: targeted test for {v['rule']} ({v['from']} -> {v['to']})",
         f"// monitor: {kind}, window {WINDOW} ns, prints CG_HAZARD <time> <detail> for each hit",
         "`timescale 1ns/1ps", "module tb;", "  integer seed = 7, hazards = 0, k, n;",
         "  realtime first = -1, dt;"]
    for name, pj in ports.items():
        w = len(pj["bits"])
        rng = f"[{w - 1}:0] " if w > 1 else ""
        L.append(f"  {'reg' if pj['direction'] == 'input' else 'wire'} {rng}{name};")
    L.append(f"  {top} dut (" + ", ".join(f".{p}({p})" for p in ports) + ");")
    L.append("  initial begin")
    for name, pj in ports.items():
        if pj["direction"] == "input":
            val = (0 if resets[name] else 1) if name in resets else 0
            L.append(f"    {name} = {val};")
    L.append("  end")
    for c in clocks:
        L.append(f"  initial begin #{phase[c]:.3f}; forever #{period[c] / 2:.4f} {c} = ~{c}; end")

    # data inputs: launched by the clock that samples them, or at random times
    for name, j in inputs.items():
        if j["kind"] in ("clock", "reset"):
            continue
        w = len(ports[name]["bits"])
        src = j.get("clock") or next((d for d in j.get("sampled_by", []) if d in clocks), None)
        if src and j["kind"] != "async":
            L.append(f"  always @(negedge {src}) {name} = {rnd(w)};")
        else:
            L.append(f"  initial forever begin #(1.7 + ($random(seed) & 15) * 0.61); {name} = {rnd(w)}; end")

    # resets: released once at the start; reset monitors keep hitting the window
    targeted = set()
    if kind == "reset" or mon.get("pulse"):
        watch = Ref(mon.get("reset", ""), ports) if kind == "reset" else None
        drive = []
        if kind == "reset":
            if watch.port:
                drive = [watch.base]
            else:
                drive = next((r["roots"] for r in rep["resets"] if r["name"] == watch.base), [])
        else:
            drive = list(mon["pulse"])
        drive = [d for d in drive if d in resets]
        for r in drive:
            targeted.add(r)
            on, off = (0, 1) if resets[r] else (1, 0)
            if kind == "reset" and watch.port and clk.port:
                # release half a window before a capture edge
                step = f"@({edge} {clk.expr}); #{clk_p - WINDOW / 2:.4f} {r} = {off};"
                L.append(f"  initial begin #{release}; {r} = {off}; repeat (40) begin #{20 * clk_p:.3f}; "
                         f"{r} = {on}; repeat (3) @({edge} {clk.expr}); {step} end end")
            elif kind == "reset":
                # can't place an internal release directly: release at random phases instead
                L.append(f"  initial begin #{release}; {r} = {off}; repeat (60) begin #{20 * clk_p:.3f}; "
                         f"{r} = {on}; #{4 * maxp:.3f}; #(($random(seed) & 1023) * {clk_p / 1024:.5f}); "
                         f"{r} = {off}; end end")
            else:
                # assert this reset alone, just before a capture edge of the victim
                L.append(f"  initial begin #{release}; {r} = {off}; repeat (40) begin #{20 * clk_p:.3f}; "
                         f"@({edge} {clk.expr}); #{clk_p - WINDOW / 2:.4f} {r} = {on}; "
                         f"repeat (3) @({edge} {clk.expr}); {r} = {off}; end end")
    for r, low in resets.items():
        if r not in targeted:
            L.append(f"  initial #{release} {r} = {1 if low else 0};")

    armed = release + 3 * maxp
    L += ["  task hazard(input real d);", "    begin",
          "      hazards = hazards + 1;",
          "      if (first < 0) first = $realtime;",
          "      if (hazards <= 20) $display(\"CG_HAZARD %0.3f %0.3f\", $realtime, d);",
          "    end", "  endtask"]

    if kind == "capture":
        srcs = [Ref(s, ports) for s in mon.get("src", [])]
        srcs = [s for s in srcs if s.ok]
        if not srcs or not clk.ok:
            return None, "the nets it names aren't visible in simulation"
        dumps += srcs + [Ref(mon["dst"], ports)] if mon.get("dst") else srcs
        # Look a picosecond after the edge so a change at the edge itself is
        # already recorded. A same-domain change lands exactly on the edge
        # (dt == 0) and is normal, except for a clock sampled as data.
        lo = -0.0005 if mon.get("same_edge") else 0.0005
        L.append("  realtime t_src = -1e9;")
        L.append("  always @(" + " or ".join(s.expr for s in srcs) + ") t_src = $realtime;")
        L.append(f"  always @({edge} {clk.expr}) if ($realtime > {armed}) begin #0.001; "
                 f"dt = $realtime - 0.001 - t_src; if (dt > {lo} && dt < {WINDOW}) hazard(dt); end")
    elif kind == "bus":
        srcs = [Ref(s, ports) for s in mon.get("src", [])]
        srcs = [s for s in srcs if s.ok]
        if len(srcs) < 2 or not clk.ok:
            return None, "the bus isn't visible in simulation"
        dumps += srcs + [Ref(d, ports) for d in mon.get("dst", [])]
        L.append(f"  realtime t_b [0:{len(srcs) - 1}];")
        L.append(f"  initial for (k = 0; k < {len(srcs)}; k = k + 1) t_b[k] = -1e9;")
        for k, s in enumerate(srcs):
            L.append(f"  always @({s.expr}) t_b[{k}] = $realtime;")
        L.append(f"  always @({edge} {clk.expr}) if ($realtime > {armed}) begin n = 0; "
                 f"for (k = 0; k < {len(srcs)}; k = k + 1) if ($realtime - t_b[k] > 0 && "
                 f"$realtime - t_b[k] < {WINDOW}) n = n + 1; if (n >= 2) hazard(n); end")
    elif kind == "reset":
        if not watch.ok or not clk.ok:
            return None, "the reset net isn't visible in simulation"
        dumps += [watch]
        off = 1 if mon.get("active_low", True) else 0
        L.append("  realtime t_rel = -1e9;")
        L.append(f"  always @({watch.expr}) if ({watch.expr} === 1'b{off}) t_rel = $realtime;")
        L.append(f"  always @({edge} {clk.expr}) begin dt = $realtime - t_rel; "
                 f"if (dt > 0 && dt < {WINDOW}) hazard(dt); end")
    elif kind in ("glitch", "pulse"):
        net = Ref(mon["net"], ports)
        if not net.ok:
            return None, "the clock net isn't visible in simulation"
        dumps += [net] + ([Ref(mon["enable"], ports)] if mon.get("enable") else [])
        minw = 0.4 * clk_p
        L.append("  realtime t_r = -1e9, t_f = -1e9;")
        L.append(f"  always @({net.expr}) begin if ({net.expr} === 1'b1) begin "
                 f"if ($realtime > {armed} && $realtime - t_f < {minw:.3f}) hazard($realtime - t_f); "
                 f"t_r = $realtime; end else if ({net.expr} === 1'b0) begin "
                 f"if ($realtime > {armed} && $realtime - t_r < {minw:.3f}) hazard($realtime - t_r); "
                 f"t_f = $realtime; end end")
    else:
        return None, "no targeted test for this rule"

    if v.get("to") and "," not in v["to"]:  # show the register the violation is about
        dumps.append(Ref(v["to"], ports))
    seen, uniq = set(), []
    for d in dumps:
        if d.ok and d.dump not in seen:
            seen.add(d.dump)
            uniq.append(d)
    L.append(f"  initial begin $dumpfile(\"{vcd_path}\"); $dumpvars(0, " +
             ", ".join(d.dump for d in uniq) + "); end")
    L.append(f"  initial begin #{sim_ns}; $display(\"CG_DONE %0d %0.3f\", hazards, first); $finish; end")
    L.append("endmodule")
    meta = {"clock": clk.base, "period": clk_p, "release": release, "signals": [d.dump for d in uniq],
            "clocks": ["tb." + c for c in clocks]}
    return "\n".join(L) + "\n", meta


def describe(kind, d):
    if kind == "capture":
        return f"source changed {d:.2f} ns before the capture edge"
    if kind == "bus":
        return f"{int(d)} bits changed inside one {WINDOW} ns capture window"
    if kind == "reset":
        return f"reset released {d:.2f} ns before the edge"
    return f"{d:.2f} ns pulse"


def run_one(i, v, rep, ports, files, top, cons, outdir, sim_ns):
    base = os.path.join(outdir, f"v{i:02d}_{v['rule'].lower()}")
    tb, meta = testbench(v, rep, ports, top, cons, sim_ns, base + ".vcd")
    if tb is None:
        return {"status": "skipped", "summary": f"skipped: {meta}"}
    with open(base + "_tb.v", "w") as f:
        f.write(tb)
    c = subprocess.run(["iverilog", "-g2012", "-o", base + ".vvp", base + "_tb.v"] + list(files),
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if c.returncode != 0:
        first_err = next((l for l in c.stdout.splitlines() if "error" in l.lower()), c.stdout.strip()[:200])
        return {"status": "skipped", "summary": f"skipped: testbench didn't compile ({first_err.strip()})",
                "tb": base + "_tb.v"}
    r = subprocess.run(["vvp", "-n", base + ".vvp"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       timeout=600)
    hazards, first = 0, -1.0
    detail = None
    for line in r.stdout.splitlines():
        if line.startswith("CG_HAZARD") and detail is None:
            detail = float(line.split()[2])
        if line.startswith("CG_DONE"):
            _, n, t = line.split()
            hazards, first = int(n), float(t)
    p, rel = meta["period"], meta["release"]
    res = {"status": "observed" if hazards else "not observed", "hazards": hazards, "tb": base + "_tb.v",
           "vcd": base + ".vcd", "clock": meta["clock"], "period": p}
    if hazards:
        res["first_ns"] = first
        res["cycles_to_first"] = int((first - rel) / p)
        what = describe(v["monitor"]["type"], detail)
        res["first_detail"] = what
        res["summary"] = (f"observed {hazards} hazard(s); first at {first:.2f} ns ({what}), "
                          f"{res['cycles_to_first']} {meta['clock']} cycles after reset release")
        sig = vcd.read(base + ".vcd", set(meta["signals"]))
        order = [s for s in meta["signals"] if s in sig]
        t0, t1 = int((first - 4 * p) * PS), int((first + 2 * p) * PS)
        labels = {s: s.replace("tb.dut.", "").replace("tb.", "") for s in order}
        with open(base + ".svg", "w") as f:
            f.write(vcd.svg(sig, order, max(0, t0), t1, marks=[(int(first * PS), f"{first:.2f} ns: {what}")],
                            labels=labels))
        res["svg"] = base + ".svg"
    else:
        res["summary"] = f"not observed in {sim_ns / 1000:.0f} us ({int((sim_ns - rel) / p)} {meta['clock']} cycles)"
    return res


def run_all(rep, netlist, files, top, cons, outdir, sim_ns=20000):
    os.makedirs(outdir, exist_ok=True)
    ports = netlist["modules"][top]["ports"]
    out = {}
    for i, v in enumerate(rep["violations"]):
        if "waived" in v or not v.get("monitor"):
            continue
        out[i] = run_one(i, v, rep, ports, files, top, cons, outdir, sim_ns)
    return out
