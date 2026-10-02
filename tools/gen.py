#!/usr/bin/env python3
"""Generate multi-clock benchmark designs, clean or with one known bug.

    python3 tools/gen.py --clean 300 --per-bug 30 --seed 1 --out bench/designs

Each design has 2-4 clocks with a reset synchronizer each, and 2-6 blocks
built from the correct patterns (synchronizers, pulse synchronizers,
handshakes, async FIFOs, gray counters, clock gates, dividers). A buggy
design swaps one block for a broken variant. truth.json records, for every
design, the rule that should fire and where (a name prefix).
"""
import argparse
import json
import os
import random

BUGS = {  # variant: rule
    "nosync": "CDC_UNSYNC", "single": "CDC_UNSYNC", "hs_noqual": "CDC_UNSYNC",
    "comb": "CDC_COMB_BEFORE_SYNC", "gray_bin": "CDC_BUS", "fifo_bin": "CDC_BUS",
    "fanout": "CDC_SYNC_FANOUT", "reconv": "CDC_RECONV",
    "rst_unsync": "RST_UNSYNC", "rst_wrong": "RST_WRONG_DOMAIN", "rst_short": "RST_SYNC_SHORT",
    "rst_comb": "RST_COMB", "rdc": "RDC",
    "gate_flop": "CLK_GATE_GLITCH", "gate_phase": "CLK_GATE_GLITCH", "gate_domain": "CLK_GATE_DOMAIN",
    "clk_mux": "CLK_MUX", "clk_as_data": "CLK_AS_DATA", "clk_logic": "CLK_FROM_LOGIC",
}

LEVEL_SRC = """
  // @P: level from @CA to @CB
  reg @Psrc;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Psrc <= 1'b0;
    else if (@Pgo) @Psrc <= ~@Psrc;"""

SYNC = {
    "2": """
  wire @Psync;
  sync2 @Pus (.clk(@CB), .rst_n(@RB), .d(@Psrc), .q(@Psync));""",
    "3": """
  reg @Ps1, @Ps2, @Ps3;
  always @(posedge @CB or negedge @RB)
    if (!@RB) begin @Ps1 <= 1'b0; @Ps2 <= 1'b0; @Ps3 <= 1'b0; end
    else begin @Ps1 <= @Psrc; @Ps2 <= @Ps1; @Ps3 <= @Ps2; end
  wire @Psync = @Ps3;""",
    "nosync": """
  wire @Psync = @Psrc;""",
    "single": """
  reg @Ps1;
  always @(posedge @CB or negedge @RB)
    if (!@RB) @Ps1 <= 1'b0;
    else @Ps1 <= @Psrc;
  wire @Psync = @Ps1;""",
    "comb": """
  reg @Psrc2;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Psrc2 <= 1'b0;
    else @Psrc2 <= @Pgo2;
  wire @Psync;
  sync2 @Pus (.clk(@CB), .rst_n(@RB), .d(@Psrc ^ @Psrc2), .q(@Psync));""",
    "fanout": """
  reg @Ps1, @Ps2a, @Ps2b;
  always @(posedge @CB or negedge @RB)
    if (!@RB) begin @Ps1 <= 1'b0; @Ps2a <= 1'b0; @Ps2b <= 1'b0; end
    else begin @Ps1 <= @Psrc; @Ps2a <= @Ps1; @Ps2b <= @Ps1; end
  wire @Psync = @Ps2a ^ @Ps2b;""",
}

LEVEL_DST = """
  reg @Pseen;
  always @(posedge @CB or negedge @RB)
    if (!@RB) @Pseen <= 1'b0;
    else @Pseen <= @Pseen ^ @Psync;
  assign @Pout = @Pseen;"""

PULSE = """
  // @P: pulse from @CA to @CB
  reg @Ptog;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Ptog <= 1'b0;
    else if (@Pgo) @Ptog <= ~@Ptog;
  wire @Ptog_b;
  sync2 @Pus (.clk(@CB), .rst_n(@RB), .d(@Ptog), .q(@Ptog_b));
  reg @Ptog_q;
  always @(posedge @CB or negedge @RB)
    if (!@RB) @Ptog_q <= 1'b0;
    else @Ptog_q <= @Ptog_b;
  assign @Pout = @Ptog_b ^ @Ptog_q;"""

HANDSHAKE = """
  // @P: @WID-bit handshake from @CA to @CB
  reg @Preq;
  reg [@WID-1:0] @Pdata_a;
  wire @Pack_a;
  wire @Pbusy = @Preq | @Pack_a;
  always @(posedge @CA or negedge @RA)
    if (!@RA) begin @Preq <= 1'b0; @Pdata_a <= 0; end
    else if (@Pgo && !@Pbusy) begin @Preq <= 1'b1; @Pdata_a <= @Pdin; end
    else if (@Pack_a) @Preq <= 1'b0;
  wire @Preq_b;
  reg @Preq_q, @Pack;
  reg [@WID-1:0] @Pdout;
  sync2 @Pur (.clk(@CB), .rst_n(@RB), .d(@Preq), .q(@Preq_b));
  wire @Ptake = @Preq_b & ~@Preq_q;
  always @(posedge @CB or negedge @RB)
    if (!@RB) begin @Preq_q <= 1'b0; @Pack <= 1'b0; @Pdout <= 0; end
    else begin @Preq_q <= @Preq_b; @Pack <= @Preq_b; @LOAD end
  sync2 @Pua (.clk(@CA), .rst_n(@RA), .d(@Pack), .q(@Pack_a));
  assign @Pout = @Pdout;"""

FIFO = """
  // @P: async FIFO, @WID bits x 2^@ADR, @CA -> @CB
  reg [@WID-1:0] @Pmem [0:(1<<@ADR)-1];
  reg [@ADR:0] @Pwbin, @Pwptr, @Prbin, @Prptr;
  wire [@ADR:0] @Pwq2_rptr, @Prq2_wptr;
  sync2 #(.W(@ADR+1)) @Pw2r (.clk(@CB), .rst_n(@RB), .d(@WSRC), .q(@Prq2_wptr));
  sync2 #(.W(@ADR+1)) @Pr2w (.clk(@CA), .rst_n(@RA), .d(@Prptr), .q(@Pwq2_rptr));
  reg @Pwfull, @Prempty;
  wire [@ADR:0] @Pwbinnext = @Pwbin + {{@ADR{1'b0}}, @Pwinc & ~@Pwfull};
  wire [@ADR:0] @Pwgraynext = (@Pwbinnext >> 1) ^ @Pwbinnext;
  always @(posedge @CA or negedge @RA)
    if (!@RA) begin @Pwbin <= 0; @Pwptr <= 0; @Pwfull <= 1'b0; end
    else begin
      @Pwbin <= @Pwbinnext;
      @Pwptr <= @Pwgraynext;
      @Pwfull <= @Pwgraynext == {~@Pwq2_rptr[@ADR:@ADR-1], @Pwq2_rptr[@ADR-2:0]};
    end
  always @(posedge @CA)
    if (@Pwinc && !@Pwfull) @Pmem[@Pwbin[@ADR-1:0]] <= @Pwdata;
  wire [@ADR:0] @Prbinnext = @Prbin + {{@ADR{1'b0}}, @Princ & ~@Prempty};
  wire [@ADR:0] @Prgraynext = (@Prbinnext >> 1) ^ @Prbinnext;
  reg [@WID-1:0] @Prdata;
  always @(posedge @CB or negedge @RB)
    if (!@RB) begin @Prbin <= 0; @Prptr <= 0; @Prempty <= 1'b1; @Prdata <= 0; end
    else begin
      @Prbin <= @Prbinnext;
      @Prptr <= @Prgraynext;
      @Prempty <= @Prgraynext == @Prq2_wptr;
      if (@Princ && !@Prempty) @Prdata <= @Pmem[@Prbin[@ADR-1:0]];
    end
  assign @Pout = @Prdata;
  assign @Pfull = @Pwfull;
  assign @Pempty = @Prempty;"""

GRAY = """
  // @P: @WID-bit gray counter from @CA to @CB
  reg [@WID-1:0] @Pbin, @Pgray;
  wire [@WID-1:0] @Pbin_next = @Pbin + {{(@WID-1){1'b0}}, @Pgo};
  always @(posedge @CA or negedge @RA)
    if (!@RA) begin @Pbin <= 0; @Pgray <= 0; end
    else begin @Pbin <= @Pbin_next; @Pgray <= @Pbin_next ^ (@Pbin_next >> 1); end
  sync2 #(.W(@WID)) @Pus (.clk(@CB), .rst_n(@RB), .d(@GSRC), .q(@Pout));"""

RECONV = """
  // @P: two flags synchronized separately (reconvergence)
  reg @Pv, @Pm;
  always @(posedge @CA or negedge @RA)
    if (!@RA) begin @Pv <= 1'b0; @Pm <= 1'b0; end
    else begin @Pv <= @Pgo; @Pm <= @Pgo2; end
  wire @Pv_b, @Pm_b;
  sync2 @Pusv (.clk(@CB), .rst_n(@RB), .d(@Pv), .q(@Pv_b));
  sync2 @Pusm (.clk(@CB), .rst_n(@RB), .d(@Pm), .q(@Pm_b));
  reg @Pfire;
  always @(posedge @CB or negedge @RB)
    if (!@RB) @Pfire <= 1'b0;
    else @Pfire <= @Pv_b & @Pm_b;
  assign @Pout = @Pfire;"""

GATE_EN = """
  // @P: gated clock in @CA
  reg @Pen_q;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pen_q <= 1'b0;
    else @Pen_q <= @Pgo;"""

GATES = {
    "icg": """
  wire @Pgclk;
  icg @Pug (.clk(@CA), .en(@Pen_q), .gclk(@Pgclk));""",
    "negicg": """
  reg @Pen_n;
  always @(negedge @CA or negedge @RA)
    if (!@RA) @Pen_n <= 1'b0;
    else @Pen_n <= @Pen_q;
  wire @Pgclk = @CA & @Pen_n;""",
    "gate_flop": """
  wire @Pgclk = @CA & @Pen_q;""",
    "gate_phase": """
  reg @Pen_l;
  always @(*)
    if (@CA) @Pen_l = @Pen_q;
  wire @Pgclk = @CA & @Pen_l;""",
    "gate_domain": """
  reg @Pen_c, @Pen_l;
  always @(posedge @CC or negedge @RC)
    if (!@RC) @Pen_c <= 1'b0;
    else @Pen_c <= @Pgo;
  always @(*)
    if (!@CC) @Pen_l = @Pen_c;
  wire @Pgclk = @CA & @Pen_l;""",
}

GATED_FLOPS = """
  reg [7:0] @Pq;
  always @(posedge @Pgclk or negedge @RA)
    if (!@RA) @Pq <= 8'd0;
    else @Pq <= @Pq + @Pd;
  assign @Pout = @Pq;"""

DIVIDER = """
  // @P: divide-by-two clock from @CA
  reg @Pdiv;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pdiv <= 1'b0;
    else @Pdiv <= ~@Pdiv;
  reg [7:0] @Pslow, @Pfast;
  always @(posedge @Pdiv or negedge @RA)
    if (!@RA) @Pslow <= 8'd0;
    else @Pslow <= @Pfast + @Pd;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pfast <= 8'd0;
    else @Pfast <= @Pslow;
  assign @Pout = @Pfast;"""

LOCAL = {
    "local": """
  // @P: accumulator in @CA
  reg [7:0] @Pacc;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + @Pd;
  assign @Pout = @Pacc;""",
    "rst_unsync": """
  // @P: accumulator reset straight from the reset input
  reg [7:0] @Pacc;
  always @(posedge @CA or negedge rst_n)
    if (!rst_n) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + @Pd;
  assign @Pout = @Pacc;""",
    "rst_comb": """
  // @P: accumulator whose reset is ANDed after the synchronizer
  reg @Pkeep;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pkeep <= 1'b1;
    else @Pkeep <= ~@Pgo;
  wire @Prst_n = @RA & @Pkeep;
  reg [7:0] @Pacc;
  always @(posedge @CA or negedge @Prst_n)
    if (!@Prst_n) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + @Pd;
  assign @Pout = @Pacc;""",
    "rdc": """
  // @P: config register on its own reset feeding the main datapath
  wire @Psw_n;
  rst_sync @Pusr (.clk(@CA), .arst_n(@Psw_rst_n), .rst_n(@Psw_n));
  reg [7:0] @Pcfg, @Pacc;
  always @(posedge @CA or negedge @Psw_n)
    if (!@Psw_n) @Pcfg <= 8'd0;
    else @Pcfg <= @Pd;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + @Pcfg;
  assign @Pout = @Pacc;""",
    "clk_as_data": """
  // @P: samples its own clock
  reg [7:0] @Pacc;
  always @(posedge @CA or negedge @RA)
    if (!@RA) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + {@CA, @Pd[6:0]};
  assign @Pout = @Pacc;""",
    "clk_logic": """
  // @P: a flop clocked by logic
  reg @Pra, @Prb;
  always @(posedge @CA or negedge @RA)
    if (!@RA) begin @Pra <= 1'b0; @Prb <= 1'b0; end
    else begin @Pra <= @Pgo; @Prb <= @Pd[0]; end
  wire @Pstrobe = @Pra & @Prb;
  reg [7:0] @Pacc;
  always @(posedge @Pstrobe)
    @Pacc <= @Pd;
  assign @Pout = @Pacc;""",
    "clk_mux": """
  // @P: flops on a muxed clock
  wire @Pclk_m = @Pgo ? @CB : @CA;
  wire @Prm_n;
  rst_sync @Purm (.clk(@Pclk_m), .arst_n(rst_n), .rst_n(@Prm_n));
  reg [7:0] @Pacc;
  always @(posedge @Pclk_m or negedge @Prm_n)
    if (!@Prm_n) @Pacc <= 8'd0;
    else @Pacc <= @Pacc + @Pd;
  assign @Pout = @Pacc;""",
}

CROSSING = {"level", "level3", "pulse", "handshake", "fifo", "gray"}
CLEAN_KINDS = ["level", "level3", "pulse", "handshake", "fifo", "gray", "icg", "negicg", "divider", "local"]


def block(kind, i, a, b, c, rnd):
    """Verilog for one block, its ports, and the clocks it needs."""
    p = f"b{i}_"
    ports = [("in", 1, p + "go"), ("out", 1, p + "out")]
    subs = {"@P": p, "@CA": f"clk{a}", "@CB": f"clk{b}", "@RA": f"r{a}_n", "@RB": f"r{b}_n",
            "@CC": f"clk{c}", "@RC": f"r{c}_n"}
    if kind in ("level", "level3", "nosync", "single", "comb", "fanout", "rst_wrong"):
        sync = {"level": "2", "level3": "3", "rst_wrong": "2"}.get(kind, kind)
        text = LEVEL_SRC + SYNC[sync] + LEVEL_DST
        if kind == "comb":
            ports.append(("in", 1, p + "go2"))
        if kind == "rst_wrong":  # the clk_b side uses clk_a's reset
            subs["@RB"] = f"r{a}_n"
    elif kind == "pulse":
        text = PULSE
    elif kind in ("handshake", "hs_noqual"):
        w = rnd.choice([4, 8, 16, 32])
        load = f"{p}dout <= {p}data_a;" if kind == "hs_noqual" else f"if ({p}take) {p}dout <= {p}data_a;"
        text = HANDSHAKE.replace("@LOAD", load).replace("@WID", str(w))
        ports = [("in", 1, p + "go"), ("in", w, p + "din"), ("out", w, p + "out")]
    elif kind in ("fifo", "fifo_bin"):
        w, adr = rnd.choice([4, 8, 16]), rnd.choice([2, 3, 4])
        text = FIFO.replace("@WSRC", p + ("wbin" if kind == "fifo_bin" else "wptr"))
        text = text.replace("@WID", str(w)).replace("@ADR", str(adr))
        ports = [("in", 1, p + "winc"), ("in", w, p + "wdata"), ("out", 1, p + "full"), ("in", 1, p + "rinc"),
                 ("out", w, p + "out"), ("out", 1, p + "empty")]
    elif kind in ("gray", "gray_bin"):
        w = rnd.randint(3, 8)
        text = GRAY.replace("@GSRC", p + ("bin" if kind == "gray_bin" else "gray")).replace("@WID", str(w))
        ports = [("in", 1, p + "go"), ("out", w, p + "out")]
    elif kind == "reconv":
        text = RECONV
        ports.append(("in", 1, p + "go2"))
    elif kind in GATES:
        text = GATE_EN + GATES[kind] + GATED_FLOPS
        ports = [("in", 1, p + "go"), ("in", 8, p + "d"), ("out", 8, p + "out")]
    elif kind == "divider":
        text = DIVIDER
        ports = [("in", 8, p + "d"), ("out", 8, p + "out")]
    else:
        text = LOCAL[kind]
        ports = [("in", 8, p + "d"), ("out", 8, p + "out")]
        if kind in ("rst_comb", "clk_logic", "clk_mux"):
            ports.append(("in", 1, p + "go"))
        if kind == "rdc":
            ports.append(("in", 1, p + "sw_rst_n"))
    for k in sorted(subs, key=len, reverse=True):
        text = text.replace(k, subs[k])
    return text, ports


def design(name, rnd, bug=None):
    k = rnd.randint(2, 4)
    n = rnd.randint(2, 6)
    kinds = [rnd.choice(CLEAN_KINDS) for _ in range(n)]
    where, short = [], None
    if bug == "rst_short":
        short = rnd.randrange(k)
        kinds[rnd.randrange(n)] = ("local", short)  # make sure that reset is used
        where = [f"r{short}_n", f"rs{short}"]
    elif bug:
        slot = rnd.randrange(n)
        kinds[slot] = bug
        where = [f"b{slot}_"]
    blocks, ports = [], [("in", 1, f"clk{d}") for d in range(k)] + [("in", 1, "rst_n")]
    for i, kind in enumerate(kinds):
        fixed = None
        if isinstance(kind, tuple):
            kind, fixed = kind
        a = fixed if fixed is not None else rnd.randrange(k)
        b = rnd.choice([d for d in range(k) if d != a])
        c = b
        text, bp = block(kind, i, a, b, c, rnd)
        blocks.append(text)
        ports += bp
    lines = [f"// generated by tools/gen.py: {bug or 'clean'}", f"module {name} ("]
    decl = []
    for d, w, pn in ports:
        decl.append(f"  {'input ' if d == 'in' else 'output'} {f'[{w - 1}:0] ' if w > 1 else ''}{pn}")
    lines.append(",\n".join(decl))
    lines.append(");")
    for d in range(k):
        if d == short:
            lines.append(f"  reg rs{d}_q;\n  always @(posedge clk{d} or negedge rst_n)\n"
                         f"    if (!rst_n) rs{d}_q <= 1'b0;\n    else rs{d}_q <= 1'b1;\n  wire r{d}_n = rs{d}_q;")
        else:
            stages = rnd.choice([2, 2, 3])
            lines.append(f"  wire r{d}_n;\n  rst_sync #(.STAGES({stages})) rs{d} (.clk(clk{d}), .arst_n(rst_n), "
                         f".rst_n(r{d}_n));")
    lines += blocks
    lines.append("endmodule")
    return "\n".join(lines) + "\n", {"design": name, "bug": bug, "rule": BUGS.get(bug), "where": where}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clean", type=int, default=300)
    ap.add_argument("--per-bug", type=int, default=30)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    rnd = random.Random(a.seed)
    os.makedirs(a.out, exist_ok=True)
    truth = []
    jobs = [None] * a.clean + [b for b in BUGS for _ in range(a.per_bug)]
    for i, bug in enumerate(jobs):
        name = f"d{i:04d}_{bug or 'clean'}"
        text, t = design(name, rnd, bug)
        with open(os.path.join(a.out, name + ".v"), "w") as f:
            f.write(text)
        truth.append(t)
    with open(os.path.join(a.out, "truth.json"), "w") as f:
        json.dump(truth, f, indent=1)
    print(f"{len(truth)} designs ({a.clean} clean, {len(truth) - a.clean} with one bug) in {a.out}")


if __name__ == "__main__":
    main()
