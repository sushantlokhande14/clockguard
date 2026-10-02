// expect: CDC_COMB_BEFORE_SYNC
// Two clk_a flops are ANDed and the AND output is synchronized: the gate
// can glitch while its inputs change, and the synchronizer can catch it.
module cdc_comb_before_sync (
  input  clk_a,
  input  clk_b,
  input  rst_n,
  input  x,
  input  y,
  output both_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg xa, ya;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) begin
      xa <= 1'b0;
      ya <= 1'b0;
    end else begin
      xa <= x;
      ya <= y;
    end

  sync2 u_sync (.clk(clk_b), .rst_n(rst_b_n), .d(xa & ya), .q(both_b));
endmodule
