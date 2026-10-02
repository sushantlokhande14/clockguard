// expect: CDC_UNSYNC
// A clk_a counter is read directly by clk_b logic.
module cdc_unsync (
  input            clk_a,
  input            clk_b,
  input            rst_n,
  output reg [3:0] seen_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg [3:0] cnt_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) cnt_a <= 4'd0;
    else cnt_a <= cnt_a + 4'd1;

  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) seen_b <= 4'd0;
    else seen_b <= seen_b ^ cnt_a;
endmodule
