// expect: CLK_GATE_GLITCH
// A latch-based gate with the latch on the wrong phase: it is transparent
// while clk is high, exactly when the AND gate passes the clock.
module clk_gate_latch_phase (
  input            clk,
  input            rst_n,
  input            en,
  input      [7:0] d,
  output reg [7:0] q
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg en_q;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) en_q <= 1'b0;
    else en_q <= en;

  reg en_l;
  always @(*)
    if (clk) en_l = en_q;
  wire gclk = clk & en_l;
  always @(posedge gclk or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= d;
endmodule
