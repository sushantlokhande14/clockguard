# clockguard

Structural clock, reset and clock-domain-crossing (CDC) checks for Verilog,
with targeted testbenches and waveforms for every violation it finds.

```bash
cmake -S . -B build && cmake --build build
python3 clockguard.py designs/bad/cdc_bus.v designs/lib/blocks.v --top cdc_bus --sim out/
```

Needs Yosys (front end), Icarus Verilog (for `--sim`) and Python 3.
