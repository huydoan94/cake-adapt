#!/usr/bin/env bash
# shellcheck disable=SC2034
# Upstream default logging; same limits and reflector order as cake-adapt.
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
