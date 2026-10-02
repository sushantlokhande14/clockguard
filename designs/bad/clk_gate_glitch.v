// expect: CLK_GATE_GLITCH
// clk ANDed with an enable from a posedge flop: the enable changes right
// after the rising edge, while clk is high, and chops the pulse.
module clk_gate_glitch (
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

  wire gclk = clk & en_q;
  always @(posedge gclk or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= d;
endmodule
