// expect: RST_COMB
// The synchronized reset is ANDed with a register bit after the
// synchronizer; any glitch on the AND resets the flops behind it.
module rst_comb (
  input            clk,
  input            rst_n,
  input            clear,
  input      [7:0] d,
  output reg [7:0] q
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg keep;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) keep <= 1'b1;
    else keep <= ~clear;

  wire data_rst_n = rst_s_n & keep;
  always @(posedge clk or negedge data_rst_n)
    if (!data_rst_n) q <= 8'd0;
    else q <= d;
endmodule
