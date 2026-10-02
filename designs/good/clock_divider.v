// expect: clean
// A divide-by-two clock made from a flop. Flops on the divided clock are
// synchronous to clk, so data moves between the two without synchronizers.
module clock_divider (
  input        clk,
  input        rst_n,
  input  [7:0] d,
  output reg [7:0] slow_q,
  output reg [7:0] fast_q
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg clk_div2;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) clk_div2 <= 1'b0;
    else clk_div2 <= ~clk_div2;

  reg [7:0] fast;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) fast <= 8'd0;
    else fast <= d;

  always @(posedge clk_div2 or negedge rst_s_n)
    if (!rst_s_n) slow_q <= 8'd0;
    else slow_q <= fast + 8'd1;

  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) fast_q <= 8'd0;
    else fast_q <= slow_q;
endmodule
