// expect: CLK_MUX
// A plain mux picks the clock; switching sel can produce a runt pulse.
module clk_mux (
  input            clk_a,
  input            clk_b,
  input            rst_n,
  input            sel,
  input      [7:0] d,
  output reg [7:0] q
);
  wire clk_m = sel ? clk_b : clk_a;
  wire rst_s_n;
  rst_sync u_r (.clk(clk_m), .arst_n(rst_n), .rst_n(rst_s_n));
  always @(posedge clk_m or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= d;
endmodule
