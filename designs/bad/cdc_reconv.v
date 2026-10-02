// expect: CDC_RECONV
// valid and mode change together in clk_a but are synchronized separately;
// clk_b can see the new valid with the old mode for a cycle.
module cdc_reconv (
  input      clk_a,
  input      clk_b,
  input      rst_n,
  input      v,
  input      m,
  output reg fire
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg valid_a, mode_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) begin
      valid_a <= 1'b0;
      mode_a <= 1'b0;
    end else begin
      valid_a <= v;
      mode_a <= m;
    end

  wire valid_b, mode_b;
  sync2 u_sv (.clk(clk_b), .rst_n(rst_b_n), .d(valid_a), .q(valid_b));
  sync2 u_sm (.clk(clk_b), .rst_n(rst_b_n), .d(mode_a), .q(mode_b));

  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) fire <= 1'b0;
    else fire <= valid_b & mode_b;
endmodule
