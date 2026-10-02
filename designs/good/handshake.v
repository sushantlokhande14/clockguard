// expect: clean
// A 16-bit word crosses with a four-phase req/ack handshake. Only req and
// ack are synchronized; the data bus is held still while req is up and
// is loaded in clk_b only on the synchronized req edge.
module handshake (
  input         clk_a,
  input         clk_b,
  input         rst_n,
  input         send,
  input  [15:0] din,
  output        busy,
  output reg [15:0] dout,
  output reg    valid_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  // clk_a side
  reg        req;
  reg [15:0] data_a;
  wire       ack_a;
  assign busy = req | ack_a;
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) begin
      req <= 1'b0;
      data_a <= 16'd0;
    end else if (send && !busy) begin
      req <= 1'b1;
      data_a <= din;
    end else if (ack_a) begin
      req <= 1'b0;
    end

  // clk_b side
  wire req_b;
  reg  req_b_q, ack;
  sync2 u_req (.clk(clk_b), .rst_n(rst_b_n), .d(req), .q(req_b));
  wire take = req_b & ~req_b_q;
  always @(posedge clk_b or negedge rst_b_n)
    if (!rst_b_n) begin
      req_b_q <= 1'b0;
      ack <= 1'b0;
      dout <= 16'd0;
      valid_b <= 1'b0;
    end else begin
      req_b_q <= req_b;
      ack <= req_b;
      valid_b <= take;
      if (take) dout <= data_a;
    end

  sync2 u_ack (.clk(clk_a), .rst_n(rst_a_n), .d(ack), .q(ack_a));
endmodule
