# Results

Measured on an Intel Core Ultra 9 185H, Ubuntu 24.04 in Docker, Yosys 0.33,
Icarus Verilog 12, 12 jobs. Raw rows: `results/bench.json`,
`results/control.json`.

## Example corpus

```bash
ctest --test-dir build     # runs tests/check_designs.py on designs/
```

8 designs in `designs/good/` (level and pulse synchronizers, a handshake,
an async FIFO, a gray counter, clock gates, a divided clock, combined
resets) report nothing. Each of the 19 in `designs/bad/` reports exactly the
rule on its `// expect:` line and nothing else. All 16 rules are covered.

## Generated designs

```bash
python3 tools/gen.py --clean 300 --per-bug 30 --seed 1 --out bench/designs
python3 tools/bench.py --designs bench/designs --jobs 12 --sim --out results/bench.json
```

`gen.py` builds each design from 2 to 4 clocks, a reset synchronizer per
clock, and 2 to 6 blocks picked at random from correct patterns: level
synchronizers (2 or 3 stages), pulse synchronizers, handshakes (4 to 32
bits), async FIFOs (4 to 16 bits, 4 to 16 entries), gray counters, latch
and negedge clock gates, divided clocks, plain registers. A buggy design
replaces one block with a broken variant, or shortens one reset
synchronizer. The designs have 12 to 596 flop bits (median 65).

A bug counts as **detected** only if its rule fires on the block it was put
in. Anything else reported on a buggy design is an **extra diagnostic**.

| | |
|---|---|
| designs | 870 (300 clean, 570 with one bug) |
| Yosys or engine failures | 0 |
| clean designs with any finding | 0 |
| bugs detected at the right block | 570 / 570 |
| same rule in the wrong place, or missed | 0, 0 |
| buggy designs with extra diagnostics | 0 |
| time per design (Yosys + engine) | median 0.048 s, max 0.30 s |
| whole run including 570 simulations | 12 s |

| variant | rule | designs | detected | right place | sim: hazard seen | median cycles to first hazard |
|---|---|--:|--:|--:|--:|--:|
| nosync | CDC_UNSYNC | 30 | 30 | 30 | 30/30 | 31 |
| single | CDC_UNSYNC | 30 | 30 | 30 | 30/30 | 24 |
| hs_noqual | CDC_UNSYNC | 30 | 30 | 30 | 28/30 | 216.5 |
| comb | CDC_COMB_BEFORE_SYNC | 30 | 30 | 30 | 30/30 | 18 |
| gray_bin | CDC_BUS | 30 | 30 | 30 | 30/30 | 61.5 |
| fifo_bin | CDC_BUS | 30 | 30 | 30 | 30/30 | 51.5 |
| fanout | CDC_SYNC_FANOUT | 30 | 30 | 30 | 30/30 | 18 |
| reconv | CDC_RECONV | 30 | 30 | 30 | 30/30 | 31 |
| rst_unsync | RST_UNSYNC | 30 | 30 | 30 | 30/30 | 24 |
| rst_wrong | RST_WRONG_DOMAIN | 30 | 30 | 30 | 30/30 | 638 |
| rst_short | RST_SYNC_SHORT | 30 | 30 | 30 | 30/30 | 24 |
| rst_comb | RST_COMB | 30 | 30 | 30 | 0/30 | - |
| rdc | RDC | 30 | 30 | 30 | 30/30 | 21 |
| gate_flop | CLK_GATE_GLITCH | 30 | 30 | 30 | 30/30 | 6 |
| gate_phase | CLK_GATE_GLITCH | 30 | 30 | 30 | 30/30 | 5 |
| gate_domain | CLK_GATE_DOMAIN | 30 | 30 | 30 | 30/30 | 6 |
| clk_mux | CLK_MUX | 30 | 30 | 30 | 30/30 | 3.5 |
| clk_as_data | CLK_AS_DATA | 30 | 30 | 30 | 30/30 | 4 |
| clk_logic | CLK_FROM_LOGIC | 30 | 30 | 30 | 0/30 | - |

### Targeted simulation

Each detected violation's testbench ran for 20 us. It showed a hazard for
**508 of 570** (89%), a median of 21 cycles after the first reset release
(2 to 1,314). The misses:

- `RST_COMB` and `CLK_FROM_LOGIC` (60): their hazard is a gate-delay
  glitch, which zero-delay RTL simulation can't produce.
- `hs_noqual` (2): the data register only changes once per handshake round
  trip (several cycles of each clock), so there are few chances; in two
  runs none landed inside the window within 20 us.
- `RST_WRONG_DOMAIN` takes longest (median 638 cycles): its reset is
  internal, so the testbench can't place the release and has to try random
  phases.

**Control.** The same monitors on correct structures in 100 clean designs
(`tools/control.py`): 71 clock gates, 266 synchronized resets, 132 gray
buses. None fired.

### Debug scope

Without a path in the report, finding the source of a bad crossing means
searching the destination's fan-in cone. For the 240 detected CDC
violations:

| rule | violations | median path nets | median cone nets | mean reduction |
|---|--:|--:|--:|--:|
| CDC_BUS | 60 | 2 | 35.5 | 91% |
| CDC_COMB_BEFORE_SYNC | 30 | 3 | 10 | 71% |
| CDC_RECONV | 30 | 3 | 14 | 79% |
| CDC_SYNC_FANOUT | 30 | 2 | 7 | 73% |
| CDC_UNSYNC | 90 | 2 | 12 | 81% |
| **all** | **240** | **2** | **12** | **81%** |

The cone is everything that feeds the destination's data and reset pins,
through flops, back to inputs. This only applies to crossings; for reset
rules the cone of a reset pin is usually a single net.

## What this does and doesn't show

- **The bug classes and the checker are both mine.** 570 of 570 says every
  check works inside larger random designs and that correct patterns
  composed together don't trip it. It says nothing about mistakes nobody
  wrote a variant for, or about RTL written in styles the generator
  doesn't use (vendor synchronizer cells, generate loops, SystemVerilog
  interfaces).
- **Real designs are bigger.** These have at most 596 flop bits. The cone
  numbers grow with design size, so the 81% isn't a prediction for a real
  SoC, in either direction.
- **The cone comparison is a proxy.** It counts nets an engineer would have
  to look at, not time. It doesn't measure how long anyone takes to debug
  anything.
- **A hazard is exposure, not a failure** (see [simulation.md](simulation.md)).
