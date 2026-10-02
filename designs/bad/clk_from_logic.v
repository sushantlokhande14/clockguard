// expect: CLK_FROM_LOGIC
// A flop clocked by the AND of two data registers.
module clk_from_logic (
  input            clk,
  input            rst_n,
  input            a,
  input            b,
  input      [7:0] d,
  output reg [7:0] q
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg ra, rb;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) begin
      ra <= 1'b0;
      rb <= 1'b0;
    end else begin
      ra <= a;
      rb <= b;
    end

  wire strobe = ra & rb;
  always @(posedge strobe)
    q <= d;
endmodule
