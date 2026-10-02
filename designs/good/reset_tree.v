// expect: clean
// Two reset sources (power-on and a software reset) combined before a
// three-stage synchronizer, so the flops only ever see one clean reset.
module reset_tree (
  input        clk,
  input        por_n,
  input        sw_rst_n,
  input  [3:0] d,
  output reg [3:0] q
);
  wire arst_n = por_n & sw_rst_n;
  wire rst_s_n;
  rst_sync #(.STAGES(3)) u_r (.clk(clk), .arst_n(arst_n), .rst_n(rst_s_n));

  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) q <= 4'd0;
    else q <= q + d;
endmodule
