#!/usr/bin/env python3
import importlib.util
import pathlib
import tempfile

spec = importlib.util.spec_from_file_location("gate", pathlib.Path(__file__).with_name("analyze.py"))
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    timeline = "batch,index,backend,start_cs,end_cs,status\n"
    turns = [("fping-before", "fping"), ("interleaved", "native"),
             ("interleaved", "fping"), ("fping-after", "fping")]
    for index, (batch, backend) in enumerate(turns):
        timeline += f"{batch},0,{backend},{index * 20},{index * 20 + 3},0\n"
    (root / "timeline.csv").write_text(timeline)
    line = "[1000.000000] 10.99.0.11 : [0], 64 bytes, 20.000 ms\n"
    for batch in ("fping-before", "interleaved", "fping-after"):
        (root / f"{batch}.fping").write_text(line)
    native = "serial,rtt_us,kernel_timestamp\n0,20001,1\n"
    (root / "interleaved.native").write_text(native)
    result = gate.analyze(root, 1)
    assert result["gate"]["median_gap_us"] == 1
    assert result["gate"]["baseline_median_spread_us"] == 0
    assert not result["gate"]["passed"]
    (root / "fping-after.fping").write_text(line.replace("20.000", "20.002"))
    assert gate.analyze(root, 1)["gate"]["passed"]
    for invalid in (native.replace(",1\n", ",0\n"), "serial,rtt_us,kernel_timestamp\n"):
        (root / "interleaved.native").write_text(invalid)
        try:
            gate.analyze(root, 1)
        except AssertionError:
            pass
        else:
            raise AssertionError("bad/missing kernel timestamp accepted")
    (root / "interleaved.native").write_text(native)
    (root / "timeline.csv").write_text(timeline.replace("20,23", "2,23"))
    try:
        gate.analyze(root, 1)
    except AssertionError:
        pass
    else:
        raise AssertionError("overlap accepted")
print("analysis tests: strict gate, missing replies/timestamps and overlap checks pass")
