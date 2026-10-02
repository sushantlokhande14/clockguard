# Rules

Every rule has a broken example in `designs/bad/` and the correct pattern it
should have used in `designs/good/` or `designs/lib/blocks.v`.

## What counts as correct

clockguard doesn't flag these, because they're the standard ways to do it:

| pattern | how it's recognized |
|---|---|
| 2+ flop synchronizer | a flop whose D is another domain's flop (or an async input) directly, and whose Q goes only to the D of one flop in its own domain |
| gray-coded bus | a multi-bit register whose D is `x ^ (x >> 1)` for one vector `x` (through enable or reset muxes), synchronized bit by bit |
| handshake / mux recirculation | data from another domain that only reaches the destination through a mux whose select depends on a synchronized signal from that same domain (up to 3 flops back) |
| async FIFO | the memory read is qualified by `rempty`, which is computed from the synchronized gray write pointer, so it falls under the rule above |
| reset synchronizer | flops reset asynchronously by the reset input, D chain starting from a constant, all on one clock; or a plain 2-flop synchronizer of the reset input |
| latch-based clock gate | an AND gate whose enable comes from a latch on the same clock that is closed while the clock is high (OR gate: the opposite) |
| opposite-edge gate | an AND gate whose enable comes from a negedge flop of the same clock |
| divided clock | a flop output used as a clock; it's in the same synchronous group as the flop's own clock |

Clocks are inputs that reach a flop's clock pin through buffers and
inverters, inputs named in the constraints, and (to break ties at a gate)
inputs whose name looks like a clock. Every primary clock is its own
asynchronous group unless the constraints put several in one group.

Data inputs are assumed synchronous to whatever samples them, unless the
constraints say otherwise (`async_inputs`, or `inputs` for the launching
clock). Reset inputs are asynchronous.

## CDC

**CDC_UNSYNC** (error). Data from another clock group reaches a flop
through logic with no synchronizer, or is caught by a single flop whose
output goes straight into logic. The capturing flop can go metastable, and
for a multi-bit value it can catch some old bits and some new ones.
Fix: a 2-flop synchronizer for a level, a pulse synchronizer for a pulse, a
handshake or async FIFO for data.

**CDC_COMB_BEFORE_SYNC** (error). A synchronizer samples the output of logic
instead of a flop. When two inputs of that logic change at once, its output
can glitch, and the synchronizer can turn the glitch into a real pulse.
Fix: register the signal in its own domain first.

**CDC_BUS** (error). The bits of one register cross through separate
synchronizers. A binary counter going from 0111 to 1000 changes every bit;
each synchronizer can resolve its bit a cycle early or late, so the
destination can see 1111 or 0000. Fix: gray-code the value (one bit
changes per step), or use a handshake or async FIFO.

**CDC_SYNC_FANOUT** (warning). The first synchronizer stage feeds more than
one flop. Each can resolve a metastable value differently, and they
disagree for a cycle. Fix: only the second stage reads the first.

**CDC_RECONV** (warning). Two signals from one domain are synchronized
separately and combined again. They can arrive a cycle apart, so the
destination briefly sees a combination that never existed in the source.
Fix: synchronize one signal and derive the others from it, or use a
handshake.

## Resets

**RST_UNSYNC** (error). Flops are reset straight from a reset input. Assert
can be asynchronous, but release has to be synchronous: if it lands near a
clock edge, some flops leave reset on this edge and some on the next
(recovery/removal). Fix: a reset synchronizer per clock.

**RST_WRONG_DOMAIN** (error). Flops use a reset that was synchronized to a
different clock, so its release is asynchronous to theirs. Fix: synchronize
the reset to each clock that uses it.

**RST_SYNC_SHORT** (warning). A reset synchronizer with one flop: its output
can still be metastable when it reaches the flops it resets. Fix: two or
more stages.

**RST_COMB** (warning). An async reset pin is driven by logic (for example
the synchronized reset ANDed with a register bit). A glitch on that logic
resets flops. Fix: combine reset sources before the synchronizer
(`designs/good/reset_tree.v`), or make every input glitch-free.

**RDC** (warning). Data moves between flops in one clock domain that are
reset by unrelated resets. When the source's reset asserts on its own, its
output changes asynchronously while the destination is still running. Fix:
reset both from the same source, or hold the destination while the source
is in reset.

## Clocks

**CLK_GATE_GLITCH** (error). A clock gate whose enable can change while the
gate is passing the clock: an enable from a posedge flop (it changes right
after the rising edge, while the clock is high), from logic, from an input,
or a latch on the wrong phase. The gated clock gets a short pulse. Fix: a
latch-based clock gate (`icg` in `designs/lib/blocks.v`), or an AND gate
with a negedge-flop enable.

**CLK_GATE_DOMAIN** (error). A clock gate's enable comes from another clock
domain, so it can change at any phase of the gated clock. Fix: launch the
enable in the gated clock's domain (through a synchronizer if it starts
elsewhere).

**CLK_MUX** (warning). A plain mux between two clocks, or between a clock
and a data signal. Changing the select while either clock is high can give
a runt pulse. A glitch-free clock mux is a specific circuit; review it and
waive the finding if that's what this is.

**CLK_AS_DATA** (warning). A clock reaches a flop's data input. Sampled at
its own edge, it has no stable value. Fix: use a toggle flop or an enable.

**CLK_FROM_LOGIC** (error). Flops are clocked by logic that isn't a clock
gate, clock mux or divider flop (two data registers ANDed, a latch output,
two clocks combined). That clock can glitch and has no defined relation to
any other domain.

**CLK_CONST** (warning). Flops whose clock is constant or undriven: they
never update.

## Waivers

```json
{"waivers": [{"rule": "CLK_MUX", "to": "u_clkmux.*", "reason": "glitch-free mux, reviewed 2026-09"}]}
```

`rule`, `from` and `to` are glob patterns (`*`, `?`) matched against the
violation; any field left out matches everything. Waived violations stay in
the report with their reason and don't count toward the exit status.
