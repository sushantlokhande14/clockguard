// expect: clean
// A level from clk_a, synchronized into clk_b with two flops. Each domain
// has its own reset synchronizer.
module level_sync (
  input  clk_a,
  input  clk_b,
  input  rst_n,
  input  start,
  output busy_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg run_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) run_a <= 1'b0;
    else if (start) run_a <= ~run_a;

  sync2 u_sync (.clk(clk_b), .rst_n(rst_b_n), .d(run_a), .q(busy_b));
endmodule
