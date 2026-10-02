// expect: CDC_SYNC_FANOUT
// The first synchronizer stage feeds two second stages, which can resolve
// a metastable value differently and disagree for a cycle.
module cdc_sync_fanout (
  input  clk_a,
  input  clk_b,
  input  rst_n,
  input  go,
  output p,
  output q
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg go_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) go_a <= 1'b0;
    else go_a <= go;

  reg s1, s2p, s2q;
  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) begin
      s1 <= 1'b0;
      s2p <= 1'b0;
      s2q <= 1'b0;
    end else begin
      s1 <= go_a;
      s2p <= s1;
      s2q <= s1;
    end
  assign p = s2p;
  assign q = s2q;
endmodule
