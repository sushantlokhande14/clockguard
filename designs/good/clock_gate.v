// expect: clean
// Two ways to gate a clock without glitches: a latch-based gate (the enable
// latch is closed while clk is high) and an AND gate whose enable comes
// from a negedge flop (it only changes while clk is low).
module clock_gate (
  input        clk,
  input        rst_n,
  input        en,
  input  [7:0] d,
  output reg [7:0] q1,
  output reg [7:0] q2
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg en_q;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) en_q <= 1'b0;
    else en_q <= en;

  wire gclk1;
  icg u_icg (.clk(clk), .en(en_q), .gclk(gclk1));
  always @(posedge gclk1 or negedge rst_s_n)
    if (!rst_s_n) q1 <= 8'd0;
    else q1 <= d;

  reg en_n;
  always @(negedge clk or negedge rst_s_n)
    if (!rst_s_n) en_n <= 1'b0;
    else en_n <= en_q;
  wire gclk2 = clk & en_n;
  always @(posedge gclk2 or negedge rst_s_n)
    if (!rst_s_n) q2 <= 8'd0;
    else q2 <= q1;
endmodule
