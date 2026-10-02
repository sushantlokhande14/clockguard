// expect: RST_WRONG_DOMAIN
// clk_b flops use the reset that was synchronized to clk_a.
module rst_wrong_domain (
  input            clk_a,
  input            clk_b,
  input            rst_n,
  input            go,
  output reg [3:0] count_b,
  output           go_b
);
  wire rst_a_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));

  reg go_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) go_a <= 1'b0;
    else go_a <= go;

  sync2 u_sync (.clk(clk_b), .rst_n(rst_a_n), .d(go_a), .q(go_b));
  always @(posedge clk_b or negedge rst_a_n)
    if (!rst_a_n) count_b <= 4'd0;
    else if (go_b) count_b <= count_b + 4'd1;
endmodule
