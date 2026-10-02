// Building blocks shared by the example designs. Each one is the
// textbook-correct version; the bad designs break them on purpose.

// Asynchronous assert, synchronous release.
module rst_sync #(parameter STAGES = 2) (
  input  clk,
  input  arst_n,
  output rst_n
);
  reg [STAGES-1:0] sr;
  always @(posedge clk or negedge arst_n)
    if (!arst_n) sr <= {STAGES{1'b0}};
    else sr <= {sr[STAGES-2:0], 1'b1};
  assign rst_n = sr[STAGES-1];
endmodule

// Two-flop synchronizer for levels (or gray-coded buses).
module sync2 #(parameter W = 1) (
  input          clk,
  input          rst_n,
  input  [W-1:0] d,
  output [W-1:0] q
);
  reg [W-1:0] s1, s2;
  always @(posedge clk or negedge rst_n)
    if (!rst_n) begin
      s1 <= {W{1'b0}};
      s2 <= {W{1'b0}};
    end else begin
      s1 <= d;
      s2 <= s1;
    end
  assign q = s2;
endmodule

// Latch-based clock gate: the enable can only change while clk is low.
module icg (
  input  clk,
  input  en,
  output gclk
);
  reg en_l;
  always @(*)
    if (!clk) en_l = en;
  assign gclk = clk & en_l;
endmodule
