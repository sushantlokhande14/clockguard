// expect: CLK_AS_DATA
// The clock is sampled as a data bit.
module clk_as_data (
  input            clk,
  input            rst_n,
  input      [6:0] d,
  output reg [7:0] q
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= {clk, d};
endmodule
