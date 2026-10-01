#!/usr/bin/env bash
# shellcheck disable=SC2034
# Every record output enabled; same limits and reflector order as cake-adapt.
dl_if=ifb4eth1
ul_if=eth1
adjust_dl_shaper_rate=1
adjust_ul_shaper_rate=1
min_dl_shaper_rate_kbps=10000
base_dl_shaper_rate_kbps=20000
max_dl_shaper_rate_kbps=50000
min_ul_shaper_rate_kbps=10000
base_ul_shaper_rate_kbps=20000
max_ul_shaper_rate_kbps=50000
randomize_reflectors=0
log_file_path_override=/tmp/perfcmp/autorate/logs
output_processing_stats=1
output_load_stats=1
output_reflector_stats=1
output_summary_stats=1
output_cake_changes=1
