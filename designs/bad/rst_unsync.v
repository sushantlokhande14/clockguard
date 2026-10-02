// expect: RST_UNSYNC
// The reset input goes straight to the flops' async reset pins, so its
// release can land anywhere in the clock cycle.
module rst_unsync (
  input            clk,
  input            rst_n,
  input      [7:0] d,
  output reg [7:0] q
);
  always @(posedge clk or negedge rst_n)
    if (!rst_n) q <= 8'd0;
    else q <= q + d;
endmodule
