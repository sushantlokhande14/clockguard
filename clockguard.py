#!/usr/bin/env python3
"""clockguard: clock, reset and clock-domain-crossing checks for Verilog.

    clockguard.py design.v [more.v ...] --top NAME [-c constraints.json]
                  [--json out.json] [--md report.md] [--html report.html]
                  [--sim DIR]

Yosys reads and flattens the design, cg_engine analyzes the netlist, and
this script prints the result and writes the reports. With --sim it also
writes a targeted testbench per violation, runs it with Icarus Verilog,
and keeps the waveform around the first hazard each one hits.
"""
import argparse
import html
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


class FlowError(Exception):
    pass


def find_engine(engine=None):
    for c in (engine, os.environ.get("CG_ENGINE"), os.path.join(HERE, "build", "cg_engine"),
              shutil.which("cg_engine")):
        if c and os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    raise FlowError("cg_engine not found: build it (cmake -S . -B build && cmake --build build) or set CG_ENGINE")


def synth(files, top, out_json):
    """Yosys: read, elaborate, flatten, map memories to flops, write JSON."""
    script = out_json + ".ys"
    with open(script, "w") as f:
        f.write("read_verilog -sv " + " ".join(f'"{p}"' for p in files) + "\n")
        f.write(f"hierarchy -check -top {top}\nproc\nflatten\nmemory\nopt_clean\nwrite_json {out_json}\n")
    r = subprocess.run(["yosys", "-q", "-s", script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        errs = [l for l in r.stdout.splitlines() if "ERROR" in l] or r.stdout.splitlines()[-5:]
        raise FlowError("yosys failed: " + " | ".join(errs))


def run(files, top, constraints=None, engine=None, workdir=None):
    """Returns (report, netlist) for a design."""
    eng = find_engine(engine)
    tmp = workdir or tempfile.mkdtemp(prefix="cg_")
    try:
        net = os.path.join(tmp, top + ".netlist.json")
        rep = os.path.join(tmp, top + ".report.json")
        synth(files, top, net)
        cmd = [eng, "-q", net, "--top", top, "-o", rep]
        if constraints:
            cmd += ["-c", constraints]
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode not in (0, 1):
            raise FlowError(f"cg_engine exited with {r.returncode}: {r.stdout.strip()[-500:]}")
        with open(rep) as f:
            report = json.load(f)
        with open(net) as f:
            netlist = json.load(f)
        return report, netlist
    finally:
        if not workdir:
            shutil.rmtree(tmp, ignore_errors=True)


def analyze(files, top, constraints=None, engine=None):
    return run(files, top, constraints, engine)[0]


# ------------------------------------------------------------------ reports

def short_path(path):
    """Collapse Yosys-internal net names ($...) in a path."""
    out, hidden = [], 0
    for n in path:
        if n.startswith("$"):
            hidden += 1
            continue
        if hidden:
            out.append(f"({hidden} internal)")
            hidden = 0
        out.append(n)
    if hidden:
        out.append(f"({hidden} internal)")
    return " -> ".join(out)


def where(v):
    a = f"{v['from']}" + (f" ({v['from_domain']})" if v.get("from_domain") else "")
    b = f"{v['to']}" + (f" ({v['to_domain']})" if v.get("to_domain") else "")
    return f"{a} -> {b}"


def text_report(rep, sims=None):
    s = rep["summary"]
    lines = [f"clockguard: {rep['top']}"]
    clocks = [d for d in rep["domains"] if d["kind"] != "async"]
    lines.append("  clocks:   " + ", ".join(
        f"{d['name']} ({d['kind']}, {d['flops']} flop bits{', gated by ' + '/'.join(d['gates']) if d['gates'] else ''})"
        for d in clocks))
    for r in rep["resets"]:
        if r["kind"] == "input":
            continue
        dom = f" for {r['domain']}" if r["domain"] else ""
        lines.append(f"  reset:    {r['name']} ({r['kind']}{dom}, {r['stages']} stage(s), from "
                     f"{', '.join(r['roots']) or '?'})")
    for y in rep["synchronizers"]:
        lines.append(f"  sync:     {y['from']} ({y['from_domain']}) -> {y['to']} ({y['to_domain']}), "
                     f"{y['bits']} bit(s), {y['stages']} stages, {y['kind']}")
    counts = {}
    for c in rep["crossings"]:
        counts[c["status"]] = counts.get(c["status"], 0) + 1
    lines.append("  crossings: " + (", ".join(f"{n} {k}" for k, n in sorted(counts.items())) or "none"))
    for i, v in enumerate(rep["violations"]):
        lines.append("")
        tag = "waived" if "waived" in v else v["severity"]
        lines.append(f"{tag}[{v['rule']}] {where(v)}")
        lines.append(f"  {v['message']}")
        if v["path"]:
            lines.append(f"  path: {short_path(v['path'])}")
        if "waived" in v:
            lines.append(f"  waived: {v['waived']}")
        if sims and i in sims:
            lines.append(f"  sim: {sims[i]['summary']}")
    lines.append("")
    lines.append(f"{s['errors']} error(s), {s['warnings']} warning(s), {s['waived']} waived")
    return "\n".join(lines)


def md_table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "|".join("---" for _ in head) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(x).replace("|", "\\|") for x in r) + " |")
    return "\n".join(out)


def markdown_report(rep, sims=None):
    s = rep["summary"]
    out = [f"# clockguard: {rep['top']}", "",
           f"{s['errors']} error(s), {s['warnings']} warning(s), {s['waived']} waived. "
           f"{s['flop_bits']} flop bits, {s['domains']} domain(s), {s['synchronizers']} synchronizer(s).", "",
           "## Clock domains", "",
           md_table(["domain", "kind", "group", "flop bits", "posedge / negedge", "gated by"],
                    [[d["name"], d["kind"] + (f" of {d['parent']}" if d.get("parent") else ""), d["group"],
                      d["flops"], f"{d['posedge']} / {d['negedge']}", ", ".join(d["gates"]) or "-"]
                     for d in rep["domains"] if d["kind"] != "async"]),
           "", "## Resets", "",
           md_table(["reset", "kind", "stages", "synchronous to", "from", "consumers"],
                    [[r["name"], r["kind"], r["stages"] or "-", r["domain"] or "-", ", ".join(r["roots"]) or "-",
                      ", ".join(f"{k}: {n}" for k, n in r["consumers"].items())] for r in rep["resets"]]),
           "", "## Synchronizers", "",
           md_table(["from", "to", "bits", "stages", "kind"],
                    [[f"{y['from']} ({y['from_domain']})", f"{y['to']} ({y['to_domain']})", y["bits"], y["stages"],
                      y["kind"]] for y in rep["synchronizers"]]) if rep["synchronizers"] else "none",
           "", "## Crossings", "",
           md_table(["from", "to", "bits", "status"],
                    [[f"{c['from']} ({c['from_domain']})", f"{c['to']} ({c['to_domain']})", c["bits"], c["status"]]
                     for c in rep["crossings"]]) if rep["crossings"] else "none",
           "", "## Violations", ""]
    if not rep["violations"]:
        out.append("none")
    for i, v in enumerate(rep["violations"]):
        tag = "waived" if "waived" in v else v["severity"]
        out += [f"### {tag}: {v['rule']}", "", f"`{where(v)}`", "", v["message"], ""]
        if v["path"]:
            out += [f"Path: `{short_path(v['path'])}`", ""]
        if v.get("cone_nets"):
            out += [f"Debug scope: {v['path_nets']} net(s) on the reported path, out of {v['cone_nets']} in the "
                    f"destination's fan-in cone.", ""]
        if "waived" in v:
            out += [f"Waived: {v['waived']}", ""]
        if sims and i in sims:
            r = sims[i]
            out += [f"Simulation: {r['summary']}", ""]
            if r.get("svg"):
                out += [f"![waveform]({os.path.basename(r['svg'])})", ""]
    if rep["notes"]:
        out += ["## Notes", ""] + [f"- {n}" for n in rep["notes"]]
    return "\n".join(out) + "\n"


CSS = """
body{font:14px/1.45 system-ui,sans-serif;margin:24px auto;max-width:1100px;padding:0 16px;color:#1d2330;background:#fff}
h1{font-size:22px}h2{font-size:17px;margin-top:28px;border-bottom:1px solid #e3e6ec;padding-bottom:4px}
table{border-collapse:collapse;width:100%;margin:8px 0}td,th{border:1px solid #e3e6ec;padding:4px 8px;text-align:left}
th{background:#f5f7fa}code{background:#f5f7fa;padding:1px 4px;border-radius:3px}
.v{border:1px solid #e3e6ec;border-left:4px solid #c0392b;border-radius:4px;padding:8px 12px;margin:12px 0}
.v.warning{border-left-color:#d68910}.v.waived{border-left-color:#7f8c8d}
.tag{font-weight:600}.muted{color:#5f6b7a}svg{max-width:100%;height:auto}
"""


def html_report(rep, sims=None):
    e = html.escape
    s = rep["summary"]

    def table(head, rows):
        return ("<table><tr>" + "".join(f"<th>{e(str(h))}</th>" for h in head) + "</tr>" +
                "".join("<tr>" + "".join(f"<td>{e(str(x))}</td>" for x in r) + "</tr>" for r in rows) + "</table>")

    parts = [f"<!doctype html><meta charset=utf-8><title>clockguard: {e(rep['top'])}</title><style>{CSS}</style>",
             f"<h1>clockguard: {e(rep['top'])}</h1>",
             f"<p>{s['errors']} error(s), {s['warnings']} warning(s), {s['waived']} waived &middot; "
             f"{s['flop_bits']} flop bits, {s['domains']} domain(s), {s['synchronizers']} synchronizer(s)</p>",
             "<h2>Clock domains</h2>",
             table(["domain", "kind", "group", "flop bits", "posedge / negedge", "gated by"],
                   [[d["name"], d["kind"] + (f" of {d['parent']}" if d.get("parent") else ""), d["group"], d["flops"],
                     f"{d['posedge']} / {d['negedge']}", ", ".join(d["gates"]) or "-"]
                    for d in rep["domains"] if d["kind"] != "async"]),
             "<h2>Resets</h2>",
             table(["reset", "kind", "stages", "synchronous to", "from", "consumers"],
                   [[r["name"], r["kind"], r["stages"] or "-", r["domain"] or "-", ", ".join(r["roots"]) or "-",
                     ", ".join(f"{k}: {n}" for k, n in r["consumers"].items())] for r in rep["resets"]]),
             "<h2>Synchronizers</h2>",
             table(["from", "to", "bits", "stages", "kind"],
                   [[f"{y['from']} ({y['from_domain']})", f"{y['to']} ({y['to_domain']})", y["bits"], y["stages"],
                     y["kind"]] for y in rep["synchronizers"]]),
             "<h2>Crossings</h2>",
             table(["from", "to", "bits", "status"],
                   [[f"{c['from']} ({c['from_domain']})", f"{c['to']} ({c['to_domain']})", c["bits"], c["status"]]
                    for c in rep["crossings"]]),
             "<h2>Violations</h2>"]
    if not rep["violations"]:
        parts.append("<p>none</p>")
    for i, v in enumerate(rep["violations"]):
        tag = "waived" if "waived" in v else v["severity"]
        p = [f"<div class='v {tag}'><div><span class=tag>{tag}[{e(v['rule'])}]</span> <code>{e(where(v))}</code></div>",
             f"<p>{e(v['message'])}</p>"]
        if v["path"]:
            p.append(f"<p class=muted>path: <code>{e(short_path(v['path']))}</code></p>")
        if v.get("cone_nets"):
            p.append(f"<p class=muted>debug scope: {v['path_nets']} net(s) on the path, {v['cone_nets']} in the "
                     f"fan-in cone</p>")
        if "waived" in v:
            p.append(f"<p class=muted>waived: {e(v['waived'])}</p>")
        if sims and i in sims:
            r = sims[i]
            p.append(f"<p>simulation: {e(r['summary'])}</p>")
            if r.get("svg") and os.path.exists(r["svg"]):
                with open(r["svg"]) as f:
                    p.append(f.read())
        p.append("</div>")
        parts.append("".join(p))
    if rep["notes"]:
        parts.append("<h2>Notes</h2><ul>" + "".join(f"<li>{e(n)}</li>" for n in rep["notes"]) + "</ul>")
    return "\n".join(parts) + "\n"


def main():
    ap = argparse.ArgumentParser(description="clock, reset and CDC checks for Verilog")
    ap.add_argument("files", nargs="+")
    ap.add_argument("--top", required=True)
    ap.add_argument("-c", "--constraints")
    ap.add_argument("--engine")
    ap.add_argument("--json")
    ap.add_argument("--md")
    ap.add_argument("--html")
    ap.add_argument("--sim", metavar="DIR", help="generate and run targeted testbenches, write waveforms here")
    ap.add_argument("--sim-ns", type=float, default=20000, help="simulated time per testbench")
    ap.add_argument("-q", action="store_true", help="only print violations")
    a = ap.parse_args()

    try:
        rep, netlist = run(a.files, a.top, a.constraints, a.engine)
    except FlowError as err:
        print(f"clockguard: {err}", file=sys.stderr)
        return 2

    sims = None
    if a.sim:
        sys.path.insert(0, os.path.join(HERE, "tools"))
        import simgen
        cons = {}
        if a.constraints:
            with open(a.constraints) as f:
                cons = json.load(f)
        sims = simgen.run_all(rep, netlist, a.files, a.top, cons, a.sim, sim_ns=a.sim_ns)

    text = text_report(rep, sims)
    if a.q:  # drop the inventory between the title and the first violation
        lines = text.splitlines()
        first_blank = next((i for i, l in enumerate(lines) if l == ""), len(lines))
        text = "\n".join(lines[:1] + lines[first_blank:])
    print(text)
    if a.json:
        out = dict(rep)
        if sims:
            out["simulation"] = {str(k): v for k, v in sims.items()}
        with open(a.json, "w") as f:
            json.dump(out, f, indent=1)
    if a.md:
        with open(a.md, "w") as f:
            f.write(markdown_report(rep, sims))
    if a.html:
        with open(a.html, "w") as f:
            f.write(html_report(rep, sims))
    return 1 if rep["summary"]["errors"] else 0


if __name__ == "__main__":
    sys.exit(main())
