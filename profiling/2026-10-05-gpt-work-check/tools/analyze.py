#!/usr/bin/env python3
"""Validate raw case evidence, then summarize bounded phase windows."""
import bisect
import json
import re
import statistics
import struct
import sys
from pathlib import Path


def require(valid, message):
    if not valid:
        raise ValueError(message)


def percentile(values, fraction):
    require(values, "empty percentile")
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int((len(ordered) - 1) * fraction))]


def packets(path):
    result = []
    with path.open("rb") as file:
        header = file.read(24)
        require(len(header) == 24, "missing PCAP header")
        endian = {b"\xd4\xc3\xb2\xa1": "<", b"\xa1\xb2\xc3\xd4": ">"}.get(header[:4])
        require(endian is not None, "unsupported PCAP magic")
        require(struct.unpack(endian + "I", header[20:24])[0] == 1, "PCAP is not Ethernet")
        while raw := file.read(16):
            require(len(raw) == 16, "truncated PCAP record")
            seconds, micros, captured, original = struct.unpack(endian + "IIII", raw)
            frame = file.read(captured)
            require(len(frame) == captured and captured >= 14, "truncated Ethernet")
            ack = False
            if frame[12:14] == b"\x08\x00":
                require(len(frame) >= 34, "truncated IPv4")
                ihl = (frame[14] & 15) * 4
                length = int.from_bytes(frame[16:18], "big")
                if frame[23] == 6:
                    tcp = 14 + ihl
                    require(len(frame) >= tcp + 20, "truncated TCP")
                    doff = (frame[tcp + 12] >> 4) * 4
                    ack = length == ihl + doff and (frame[tcp + 13] & 0x17) == 0x10
            # This experiment uses non-RAW Ethernet overhead44 / MPU84 throughout.
            charge = max(original - 14 + 44, 84)
            result.append((seconds + micros / 1e6, ack, charge, original))
    require(result, "no captured packets")
    return result


def analyze(path):
    variant = path.name
    require((path / "daemon-exit-status").read_text().strip() == "0 0", "daemon did not stop gracefully")
    require((path / "daemon-affinity").read_text().split()[-1] == "0", "daemon affinity")
    require("dev cwan" in (path / "route").read_text(), "wrong route")
    require("0 packets dropped by kernel" in (path / "tcpdump.log").read_text(), "PCAP capture loss")
    log = (path / "cake-adapt.log").read_text()
    require("measurement degraded" not in log and "ACK ceiling disabled" not in log, "degraded measurement")
    load = []
    for line in log.splitlines():
        fields = [field.strip() for field in line.split(";")]
        if fields[0] == "LOAD":
            timestamp, download, upload = float(fields[2]), float(fields[6]), float(fields[7])
            require(5000 <= download <= 33000 and 1000 <= upload <= 3000, "shaper bounds")
            load.append((timestamp, download, upload))
    require(load and "DATA;" in log and "SHAPER;" in log, "missing controller records")
    samples = []
    text = (path / "samples").read_text()
    for line in text.splitlines():
        if line and line[0].isdigit():
            samples.append([int(value) for value in line.split()])
    require(len(samples) >= 10 and all(len(row) == 18 for row in samples), "missing samples")
    hz = int(re.search(r"hz=(\d+)", text)[1])
    page = int(re.search(r"page_bytes=(\d+)", text)[1])
    if variant != "off":
        require("TCP measurement started" in log and "TCP_QUEUE;" in log, "missing TCP integration")
        require(int(re.search(r"jit_bytes=(\d+)", text)[1]) > 0, "interpreted BPF")
        require(samples[-1][4] > samples[0][4], "BPF did not run")
    else:
        require(all(row[4] == 0 for row in samples), "unexpected filter in off variant")
    require(all(row[6] == 0 and row[9] == 0 for row in samples), "ring loss or unaccounted packets")
    for previous, current in zip(samples, samples[1:]):
        require(current[1] > previous[1] and all(current[i] >= previous[i] for i in (2, 4, 5, 6, 7, 8, 9)), "counter/time reset")
    throughputs = {}
    for file in sorted(path.glob("*-*.json")):
        data = json.loads(file.read_text())
        require("error" not in data, f"iperf error in {file.name}")
        receiver = data["end"]["sum_received"]
        require(receiver["bytes"] > 0 and receiver["seconds"] > 0, "no received payload")
        phase, direction = file.stem.rsplit("-", 1)
        throughputs.setdefault(phase, {})[direction] = {
            "mbps": receiver["bits_per_second"] / 1e6,
            "bytes": receiver["bytes"], "seconds": receiver["seconds"],
        }
    require("mixed" in throughputs and set(throughputs["mixed"]) == {"upload", "download"}, "missing mixed payloads")
    rtt = []
    for line in (path / "icmp").read_text().splitlines():
        match = re.match(r"\[(\d+\.\d+)\].*bytes, ([\d.]+) ms", line)
        if match:
            rtt.append((float(match[1]), float(match[2])))
    require(rtt, "no fping replies")
    udp = []
    udp_text = (path / "udp64").read_text()
    for line in udp_text.splitlines():
        if line and line[0].isdigit():
            _, timestamp, microseconds = map(int, line.split())
            udp.append((timestamp / 1e6, microseconds / 1000))
    received = re.search(r"# sent (\d+) received (\d+)", udp_text)
    require(received and int(received[2]) > 0 and int(received[1]) >= int(received[2]), "invalid UDP receiver summary")
    packet = packets(path / "out.pcap")
    phases = [(float(time), name) for time, name in (line.split() for line in (path / "phases").read_text().splitlines())]
    idle = next((time, phases[i + 1][0]) for i, (time, name) in enumerate(phases) if name == "idle")
    baseline = statistics.median(value for time, value in rtt if idle[0] + 1 <= time < idle[1])
    udp_baseline = statistics.median(value for time, value in udp if idle[0] + 1 <= time < idle[1])
    result = {"variant": variant, "idle_rtt_ms": baseline, "udp_idle_rtt_ms": udp_baseline, "throughput": throughputs,
              "udp_sent": int(received[1]), "udp_received": int(received[2]),
              "max_rss_kib": max(row[3] for row in samples) * page / 1024,
              "phases": {}}
    load_times = [row[0] for row in load]
    for index, (start, name) in enumerate(phases[:-1]):
        if name not in ("upload", "download", "mixed", "recovery"):
            continue
        low, high = start + 2, phases[index + 1][0] - 1
        # Preflight's5s load has a shorter, but still nonempty steady window.
        require(high > low, "empty steady phase") if name != "recovery" else None
        if high <= low:
            continue
        latency = [max(0, value - baseline) for time, value in rtt if low <= time < high]
        udp_latency = [max(0, value - udp_baseline) for time, value in udp if low <= time < high]
        steady = [row for row in samples if low <= row[0] / 1e6 < high]
        require(latency and udp_latency and len(steady) >= 2, f"missing steady evidence {name}")
        duration = (steady[-1][1] - steady[0][1]) / 1e6
        runs, runtime = steady[-1][4] - steady[0][4], steady[-1][5] - steady[0][5]
        busy = sum(steady[-1][i] - steady[0][i] for i in (10, 11, 12, 15, 16, 17))
        total = sum(steady[-1][i] - steady[0][i] for i in range(10, 18))
        wire_ack = sum(charge for time, ack, charge, _ in packet if low <= time < high and ack) * 8 / (high - low)
        uplink = statistics.mean(row[2] * 1000 for row in load if low <= row[0] < high)
        shares = []
        for offset in range(int((high - low) * 2)):
            begin, end = low + offset / 2, low + (offset + 1) / 2
            ack_bits = sum(charge for time, ack, charge, _ in packet if begin <= time < end and ack) * 16
            position = bisect.bisect_right(load_times, begin) - 1
            if position >= 0:
                shares.append(ack_bits / (load[position][2] * 1000))
        result["phases"][name] = {
            "added_p50_ms": percentile(latency, .5), "added_p95_ms": percentile(latency, .95),
            "added_p99_ms": percentile(latency, .99), "above30ms_percent": 100 * sum(value > 30 for value in latency) / len(latency),
            "udp_added_p95_ms": percentile(udp_latency, .95), "ack_wire_kbps": wire_ack / 1000,
            "ul_shaper_mean_kbps": uplink / 1000, "ack_share_mean_percent": 100 * wire_ack / uplink,
            "ack_share_p95_percent": 100 * percentile(shares, .95),
            "headroom_kbps": (uplink - wire_ack) / 1000,
            "daemon_cpu_percent_one_core": 100 * (steady[-1][2] - steady[0][2]) / hz / duration,
            "bpf_runs": runs, "bpf_ns_per_run": runtime / runs if runs else 0,
            "bpf_cpu_percent_one_core": runtime / (duration * 1e9) * 100,
            "host_busy_percent_all_cores": 100 * busy / total,
            "window_start": low, "window_end": high, "icmp_samples": len(latency),
        }
    require("mixed" in result["phases"], "missing measured mixed phase")
    # Independent bytes in the sampler's exact realtime window. Small boundary
    # differences are expected because map reads and PCAP timestamps are not atomic.
    if variant != "off":
        low, high = samples[0][0] / 1e6, samples[-1][0] / 1e6
        selected = [row for row in packet if low <= row[0] < high]
        expected = sum(row[3] if (path / "variant").read_text().split()[1] == "before" else row[2] for row in selected if row[1])
        measured = samples[-1][7] - samples[0][7]
        require(abs(expected - measured) <= max(500, expected * .01), "BPF ACK bytes disagree with PCAP")
        result["ack_counter_crosscheck"] = {"pcap_bytes": expected, "map_bytes": measured}
    return result


def main():
    root = Path(sys.argv[1])
    results = [analyze(path) for path in sorted(root.iterdir()) if path.is_dir()]
    require(results, "no cases")
    output = root.parent / (root.name + "-analysis.json")
    output.write_text(json.dumps(results, indent=2) + "\n")
    print("Validated", ", ".join(result["variant"] for result in results))
    for result in results:
        for name, values in result["phases"].items():
            throughput = result["throughput"].get(name, {})
            rate = lambda direction: throughput.get(direction, {}).get("mbps", 0)
            print(f"{result['variant']:6} {name:8} p95={values['added_p95_ms']:.2f}ms "
                  f"UL/DL={rate('upload'):.3f}/{rate('download'):.3f}Mb/s "
                  f"ACK={values['ack_share_mean_percent']:.1f}% spare={values['headroom_kbps']:.0f}kb/s "
                  f"daemon={values['daemon_cpu_percent_one_core']:.2f}% BPF={values['bpf_ns_per_run']:.0f}ns/run")


if __name__ == "__main__":
    main()
