// expect: CDC_BUS
// A binary counter synchronized bit by bit: on 0111 -> 1000 every bit
// changes, and clk_b can capture any mix of the two values.
module cdc_bus (
  input        clk_a,
  input        clk_b,
  input        rst_n,
  output [3:0] count_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg [3:0] count_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) count_a <= 4'd0;
    else count_a <= count_a + 4'd1;

  sync2 #(.W(4)) u_sync (.clk(clk_b), .rst_n(rst_b_n), .d(count_a), .q(count_b));
endmodule
