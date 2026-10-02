# Design

## Front end

Writing a Verilog parser is not the interesting part, so clockguard lets
Yosys do it:

```
read_verilog -sv <files>
hierarchy -check -top <top>
proc        # always blocks -> $dff, $adff, $dlatch, $mux ...
flatten     # one module; instance paths become names like u_sync.s1
memory      # memories -> flops and read muxes
opt_clean
write_json
```

No other optimization runs, so the netlist keeps the structure the RTL
describes. `proc` turns an enable into a `$mux` that feeds the register
back, which the checks below rely on.

`netlist.cpp` loads that JSON at bit level: a driver and a list of loads
for every net bit, and a readable name per bit (top ports first, then the
shallowest public name, then Yosys's internal ones).

## Clocks

For every flop clock pin (and latch enable pin) the engine walks backwards:

- through buffers and inverters, keeping track of polarity;
- through an AND/OR gate: one input is the clock, the other is recorded as
  a **gate** enable. Which input is the clock is decided by what reaches a
  known clock (an input that drives some clock pin directly, or one named
  in the constraints; a clock-looking name only breaks ties);
- through a mux: if both inputs are clocks, it's a **clock mux** and becomes
  its own domain; if one input is a constant, it's a gate written as a mux;
- stopping at an input (a **primary** clock), a flop output (a **derived**
  clock, in the same group as that flop's clock), or anything else (a clock
  **from logic**).

Domains go into a union-find of synchronous groups: derived clocks join
their parent, constraint groups are merged, everything else stays apart.

Each gate is then checked. For an AND gate the enable may only change
while the clock is low, so its source has to be a latch transparent while
the clock is low, or a negedge flop, on the same clock. For an OR gate
it's the other way round, and an inverted clock at the gate flips both.

## Resets

Every async reset pin (`ARST`, `SET`/`CLR`, `ALOAD`) is traced back through
buffers and inverters to an input, a flop, or logic:

- **input**: the release is asynchronous unless the constraints say the
  input is already synchronous to a clock;
- **flop**: the engine walks that flop's D chain back. A chain that starts
  from a constant (with flops reset by the input) or from the reset input
  itself is a **reset synchronizer**; its length is the stage count. The
  reset releases synchronously to that flop's clock;
- **logic**: each input of the logic is analyzed the same way, and the
  logic itself is reported unless it sits in front of a synchronizer.

Synchronizer flops are found first, so they aren't reported as consumers of
the raw reset they are synchronizing.

## Crossings

Pass 1 finds synchronizers: a flop whose D bit is *directly* another
group's flop output (or an async input), and whose Q bit goes only to the D
of one flop in its own group. The chain is followed while each stage has a
single flop load.

Pass 2 walks the fan-in of every flop's data pins (D, enable, sync reset)
back to flops and inputs. The walk carries a set of **qualifiers**: when it
passes a mux from the output to a data input, the clock groups whose
synchronized signals appear in that mux's select are added. A source from
another group is **qualified** if every path from it carried that group;
that covers handshakes, mux recirculation and async FIFO reads. The rest
are sorted by shape:

| destination | capture | rule |
|---|---|---|
| synchronizer first stage | direct | fine |
| feeds only flops in its domain | direct, but several flops | `CDC_SYNC_FANOUT` |
| feeds only flops in its domain | through logic | `CDC_COMB_BEFORE_SYNC` |
| feeds logic | direct | `CDC_UNSYNC` (one-flop synchronizer) |
| feeds logic | through logic | `CDC_UNSYNC` |

Synchronizers are then grouped by source register: several bits of one
register synchronized separately is `CDC_BUS`, unless the register is gray
coded. Separately synchronized registers from one group that meet in the
same logic cone are `CDC_RECONV`. Flops in one group whose sources are
reset by unrelated resets are `RDC`.

## Report

Each violation carries the rule, a message with the fix, the source and
destination with their domains, the path between them (bit names), and
the size of the destination's fan-in cone through flops (data and reset
pins, not clock pins). It also carries a **monitor**: what a testbench
should watch to see the problem (see [simulation.md](simulation.md)).

## Things that went wrong

- **Gate polarity was measured from the wrong end.** The walk recorded the
  clock's inversion between the flop pin and the gate, but whether the
  enable may change while the clock is high depends on the inversion
  between the gate's input and the root clock. Fixed by recording the
  first and subtracting it from the total once the walk ends.
- **A data latch's enable became a clock net.** Latches whose enable isn't
  a clock are skipped, but their enable nets were added to the clock tree
  first, which would have made any logic reading them `CLK_AS_DATA`.
- **The reconvergence monitor never fired.** It watched the synchronizer
  outputs, which change exactly on the destination clock edge, and the
  monitor ignores same-edge changes on purpose. The risk is at the
  sources, so the monitor now checks whether two source bits change inside
  one capture window, like the bus monitor.
- **Clock-as-data was invisible in simulation.** A clock sampled as data
  changes exactly at the capture edge (dt = 0), which every other monitor
  must ignore (a same-domain register changes at the edge too). Those
  monitors now look a picosecond after the edge, and only this rule counts
  dt = 0.
- **The fan-in-cone metric was meaningless for resets.** A reset pin's
  fan-in is usually one net, so the "reported path" was longer than the
  cone. The cone comparison is now only reported for CDC rules.
