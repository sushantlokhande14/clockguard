// expect: CDC_UNSYNC
// A one-flop "synchronizer" whose output goes straight into logic.
module cdc_single_flop (
  input      clk_a,
  input      clk_b,
  input      rst_n,
  input      go,
  output reg hits
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg flag_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) flag_a <= 1'b0;
    else flag_a <= go;

  reg flag_b;
  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) begin
      flag_b <= 1'b0;
      hits <= 1'b0;
    end else begin
      flag_b <= flag_a;
      hits <= hits | flag_b;
    end
endmodule
