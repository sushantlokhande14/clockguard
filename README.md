# clockguard

Structural clock, reset and clock-domain-crossing (CDC) checks for Verilog.
It finds every clock domain, clock gate and mux, traces every asynchronous
reset back to its source, recognizes synchronizers, and classifies every
signal that crosses between unrelated clocks. Each violation comes with the
path that causes it and a fix. With `--sim`, clockguard also writes a
targeted testbench for each violation, runs it, and draws the waveform
around the first moment the hardware would be at risk.

```
$ python3 clockguard.py designs/bad/cdc_bus.v designs/lib/blocks.v --top cdc_bus --sim out/
clockguard: cdc_bus
  clocks:   clk_a (primary, 6 flop bits), clk_b (primary, 10 flop bits)
  reset:    rst_a_n (synchronizer for clk_a, 2 stage(s), from rst_n)
  reset:    rst_b_n (synchronizer for clk_b, 2 stage(s), from rst_n)
  sync:     count_a (clk_a) -> u_sync.s1 (clk_b), 4 bit(s), 2 stages, bus
  crossings: 1 synchronized

error[CDC_BUS] count_a (clk_a) -> u_sync.s1 (clk_b)
  count_a (4 bits, clk_a) is synchronized bit by bit into u_sync.s1 (clk_b); bits that change
  together can land in different cycles and give a value that never existed. Gray-code it so
  one bit changes at a time, or use a handshake or an async FIFO
  path: count_a[0] -> u_sync.s1[0]
  sim: observed 69 hazard(s); first at 275.12 ns (2 bits changed inside one 0.5 ns capture
  window), 32 clk_b cycles after reset release
```

![waveform around the first hazard](docs/example/cdc_bus/v00_cdc_bus.svg)

## How it works

```
design.v --> Yosys --> netlist.json --> cg_engine --> report.json --> text / Markdown / HTML
            (flatten,                   (C++)                     \-> testbench per violation
             map memories)                                             --> iverilog --> VCD --> SVG
```

- **Yosys** parses the RTL, flattens it and maps memories to flops, so the
  engine sees one netlist of flops, latches and gates.
- **cg_engine** (C++, about 1,800 lines) does the analysis at bit level:
  clock tracing, reset tracing, synchronizer recognition, crossing
  classification. See [docs/design.md](docs/design.md).
- **clockguard.py** runs both, prints the result, and writes the reports.
  **tools/simgen.py** writes and runs the testbenches.

## Rules

| rule | | what |
|---|---|---|
| `CDC_UNSYNC` | error | a crossing with no synchronizer, or one flop whose output is used right away |
| `CDC_COMB_BEFORE_SYNC` | error | logic (not a flop) feeding a synchronizer |
| `CDC_BUS` | error | a multi-bit value synchronized bit by bit, unless it's gray-coded |
| `CDC_SYNC_FANOUT` | warning | the first synchronizer stage feeds more than one flop |
| `CDC_RECONV` | warning | separately synchronized signals from one domain meeting again |
| `RST_UNSYNC` | error | an async reset input released straight into a domain |
| `RST_WRONG_DOMAIN` | error | a reset synchronized to a different clock |
| `RST_SYNC_SHORT` | warning | a one-flop reset synchronizer |
| `RST_COMB` | warning | an async reset built from logic after the synchronizer |
| `RDC` | warning | data between flops in one domain that are reset by unrelated resets |
| `CLK_GATE_GLITCH` | error | a clock gate whose enable can change while the clock passes |
| `CLK_GATE_DOMAIN` | error | a clock gate whose enable comes from another clock |
| `CLK_MUX` | warning | a plain mux between clocks (or a clock and data) |
| `CLK_AS_DATA` | warning | a clock sampled as data |
| `CLK_FROM_LOGIC` | error | flops clocked by logic that isn't a gate, mux or divider |
| `CLK_CONST` | warning | flops with a constant or undriven clock |

[docs/rules.md](docs/rules.md) explains each one, what clockguard accepts
as correct (latch-based gates, gray-coded buses, handshakes, async FIFOs,
reset synchronizers, dividers), and how to waive a finding.

## Use

```bash
cmake -S . -B build && cmake --build build      # needs nlohmann/json (fetched if missing)
python3 clockguard.py rtl/*.v --top soc_top \
    -c soc.cg.json --md report.md --html report.html --sim sims/
```

Needs Yosys, Python 3, and Icarus Verilog for `--sim`. Exit status is 1 if
there are unwaived errors. Constraints are optional:

```json
{
  "clocks": {"clk_a": {"period": 10}, "clk_b": {"period": 7.3}},
  "groups": [["clk_a", "clk_a_div2"]],
  "async_inputs": ["button"],
  "inputs": {"cfg": "clk_a"},
  "resets": {"por_n": {"sync_to": "clk_a"}},
  "waivers": [{"rule": "CDC_BUS", "from": "u_dbg.*", "reason": "debug only, sampled when frozen"}]
}
```

## Results

Measured on generated multi-clock designs (2 to 4 clocks, 12 to 596 flop
bits) and on the example corpus. Details, commands and caveats are in
[docs/results.md](docs/results.md); raw rows are in [results/](results/).

- **Example corpus:** 8 correct designs come back clean; 19 broken ones each
  report exactly the rule they were written for.
- **870 generated designs:** 0 false alarms on 300 clean designs; all 570
  injected bugs (19 bug variants over 15 rules) reported at the right block,
  with no extra diagnostics. These are bug classes the checks were written
  for, so this says the checks work inside larger designs, not that they
  catch everything.
- **Targeted testbenches:** reproduced a hazard for 508 of the 570 bugs
  (89%), a median of 21 cycles after reset. They can't show glitches that
  need gate delays (`RST_COMB`, `CLK_FROM_LOGIC`). The same monitors on 469
  correct structures (clock gates, synchronized resets, gray buses) stayed
  quiet.
- **Debug scope:** for CDC violations, the reported path is a median of 2
  nets against 12 in the destination's fan-in cone (81% fewer on average).
- **Speed:** 0.05 s per design (Yosys and engine) on these sizes.

## Layout

```
src/            cg_engine: netlist loading, analysis, JSON report
clockguard.py   the flow and the text / Markdown / HTML reports
tools/          simgen.py + vcd.py (testbenches, waveforms), gen.py, bench.py, control.py
designs/        lib/ (correct building blocks), good/, bad/ (one per rule)
tests/          check_designs.py: every design against its `// expect:` line
results/        bench.json, control.json
docs/           rules.md, design.md, simulation.md, results.md, example/
```
