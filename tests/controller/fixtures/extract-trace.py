#!/usr/bin/env python3
"""Condense a cake-autorate log into a controller replay trace.

The log must come from a cake-autorate copy whose DATA record carries
t_start_us in the PROC_TIME_US column (one changed printf argument), so the
replay uses the same decision times as cake-autorate's refractory periods.

usage: extract-trace.py LOG DESCRIPTION_FILE > TRACE
"""
import hashlib
import sys

COLUMNS = (
    "DL_ACHIEVED_RATE_KBPS", "UL_ACHIEVED_RATE_KBPS", "DL_OWD_DELTA_US", "UL_OWD_DELTA_US",
    "DL_ADJ_DELAY_THR", "DL_ADJ_MAX_ADJUST_UP_THR_US", "DL_ADJ_MAX_ADJUST_DOWN_THR_US",
    "UL_ADJ_DELAY_THR", "UL_ADJ_MAX_ADJUST_UP_THR_US", "UL_ADJ_MAX_ADJUST_DOWN_THR_US",
    "DL_SUM_DELAYS", "DL_AVG_OWD_DELTA_US", "UL_SUM_DELAYS", "UL_AVG_OWD_DELTA_US",
)

raw = open(sys.argv[1], "rb").read()
header = None
events = []
data = loads = 0
for line in raw.decode().splitlines():
    fields = [field.strip() for field in line.split(";")]
    if fields[0] == "DATA_HEADER":
        header = fields
    elif fields[0] == "LOAD":
        events.append("L")
        loads += 1
    elif fields[0] == "DATA":
        record = dict(zip(header, fields))
        if "." in record["PROC_TIME_US"]:
            sys.exit("PROC_TIME_US is not t_start_us; use the instrumented cake-autorate copy")
        values = [record["PROC_TIME_US"]] + [record[column] for column in COLUMNS] + [
            str(int(record["DL_LOAD_CONDITION"].endswith("_bb"))),
            str(int(record["UL_LOAD_CONDITION"].endswith("_bb"))),
            record["CAKE_DL_RATE_KBPS"],
            record["CAKE_UL_RATE_KBPS"],
        ]
        events.append("D " + " ".join(values))
        data += 1

for line in open(sys.argv[2]).read().splitlines():
    print(f"# {line}")
print(f"# Source log sha256 {hashlib.sha256(raw).hexdigest()}; {data} DATA and {loads} LOAD records.")
print("# L: an achieved-rate sample (SARS) arrived.")
print("# D t_start_us dl_kbps ul_kbps dl_delta_us ul_delta_us")
print("#   dl_delay_thr dl_up_thr dl_down_thr ul_delay_thr ul_up_thr ul_down_thr")
print("#   dl_sum_delays dl_avg_delta ul_sum_delays ul_avg_delta dl_bb ul_bb dl_rate_kbps ul_rate_kbps")
print("\n".join(events))
