// expect: CLK_GATE_DOMAIN
// A proper latch-based gate on clk_b, but its enable is launched by clk_a.
module clk_gate_domain (
  input            clk_a,
  input            clk_b,
  input            rst_n,
  input            en,
  input      [7:0] d,
  output reg [7:0] q
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg en_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) en_a <= 1'b0;
    else en_a <= en;

  reg en_l;
  always @(*)
    if (!clk_a) en_l = en_a;
  wire gclk = clk_b & en_l;
  always @(posedge gclk or negedge rst_b_n)
    if (!rst_b_n) q <= 8'd0;
    else q <= d;
endmodule
