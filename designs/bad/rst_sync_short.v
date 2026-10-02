// expect: RST_SYNC_SHORT
// A reset "synchronizer" with a single flop.
module rst_sync_short (
  input            clk,
  input            rst_n,
  input      [7:0] d,
  output reg [7:0] q
);
  reg rst_s_n;
  always @(posedge clk or negedge rst_n)
    if (!rst_n) rst_s_n <= 1'b0;
    else rst_s_n <= 1'b1;

  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) q <= 8'd0;
    else q <= d;
endmodule
