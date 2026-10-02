// expect: CLK_CONST
// A register whose clock was tied off (a leftover from a test hookup).
module clk_const (
  input            clk,
  input            rst_n,
  input      [7:0] d,
  output reg [7:0] q,
  output reg [7:0] r
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= d;

  wire scan_clk = 1'b0;
  always @(posedge scan_clk)
    r <= q;
endmodule
