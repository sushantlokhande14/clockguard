// expect: clean
// Single-cycle pulses from clk_a become single-cycle pulses in clk_b: the
// pulse flips a toggle flop, the toggle crosses as a level, and an edge
// detector after the synchronizer turns it back into a pulse.
module pulse_sync (
  input  clk_a,
  input  clk_b,
  input  rst_n,
  input  pulse_a,
  output pulse_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg tog;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) tog <= 1'b0;
    else if (pulse_a) tog <= ~tog;

  wire tog_b;
  sync2 u_sync (.clk(clk_b), .rst_n(rst_b_n), .d(tog), .q(tog_b));

  reg tog_b_q;
  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) tog_b_q <= 1'b0;
    else tog_b_q <= tog_b;
  assign pulse_b = tog_b ^ tog_b_q;
endmodule
