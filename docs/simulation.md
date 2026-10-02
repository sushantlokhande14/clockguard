# Targeted simulation

RTL simulation has no metastability: a missing synchronizer never makes a
normal testbench fail. What a testbench can do is show *when* the hardware
would be at risk, with a waveform an engineer can look at. That's what
`--sim` does, one testbench per violation (`tools/simgen.py`).

## What the testbench does

- **Clocks** run at the periods from the constraints, or at unrelated
  defaults (10, 7.3, 13.1, 8.9, 11.7, 6.1 ns) with different phases, so
  edges drift past each other and every phase relation shows up.
- **Data inputs** change on the falling edge of the clock that samples
  them (as found by the engine). Asynchronous inputs, and inputs nothing
  samples (a clock mux select, say), change at random times.
- **Resets** are asserted at time zero and released after a few cycles.
  For reset violations the testbench keeps going: it asserts the reset
  again 40 times and each time **releases it 0.25 ns before a capture
  edge**. When the reset in question is internal (a reset synchronized to
  the wrong clock), it can't be placed directly, so the input behind it is
  released at random phases 60 times instead. For `RDC`, the source's own
  reset is **asserted** just before the victim's capture edge.

## Monitors

`WINDOW` is 0.5 ns: a stand-in for setup plus hold.

| monitor | used for | a hazard is |
|---|---|---|
| capture | `CDC_UNSYNC`, `CDC_COMB_BEFORE_SYNC`, `CDC_SYNC_FANOUT`, `RDC`, `CLK_AS_DATA` | a source net changes less than WINDOW before a capture edge |
| bus | `CDC_BUS`, `CDC_RECONV` | two or more source bits change inside one window before a capture edge |
| reset | `RST_UNSYNC`, `RST_WRONG_DOMAIN`, `RST_SYNC_SHORT` | the reset releases less than WINDOW before a capture edge |
| glitch | `CLK_GATE_*`, `CLK_MUX`, `CLK_FROM_LOGIC`, `RST_COMB` | a pulse on the net is shorter than 40% of the clock period |

A change exactly on the capture edge is normal for a register in the same
domain, so it doesn't count, except for `CLK_AS_DATA`, where the clock
itself changes on the edge. Capture, bus and glitch monitors only arm a few
cycles after the first reset release, so power-up X transitions don't count.

A hazard is *exposure*, not a failure: it's the moment a real chip could
fail. The waveform shows the clocks, the resets, the nets the violation
names, and a marker with what was measured
("source changed 0.32 ns before the capture edge",
"reset released 0.25 ns before the edge", "0.00 ns pulse").

## What it can't show

- **Gate-delay glitches.** Zero-delay simulation evaluates logic once per
  time step, so the glitch on `rst_s_n & keep` (`RST_COMB`) or on a clock
  made from two registers (`CLK_FROM_LOGIC`) never happens. Those two
  rules report "not observed", and the benchmark counts them as misses.
- **Metastability itself.** The monitors show when a flop *would* be at
  risk; nothing in the simulation goes metastable.

## Do the monitors cry wolf?

`tools/control.py` puts the same monitors on structures that are correct,
in 100 clean generated designs: every latch-based or negedge-flop clock
gate (glitch monitor), every synchronized reset against its own clock
(reset monitor), every gray-coded bus (bus monitor). 469 monitors, none
fired. Results are in `results/control.json`.

A capture monitor *would* fire on the first stage of a correct synchronizer:
sampling a changing signal is that flop's job. That's why capture monitors
are only attached to violations.
