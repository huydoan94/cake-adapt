#!/usr/bin/env bash
# Stands in for fping during trace capture: prints fping --timestamp reply lines
# with a scheduled RTT profile. Only the trailing reflector addresses are used.
reflectors=()
for argument in "$@"; do
    [[ ${argument} == *.*.*.* ]] && reflectors+=("${argument}")
done

# A read on a descriptor that never delivers data waits for its full timeout.
exec {wait_fd}<> <(:)
start_us=${EPOCHREALTIME/.}

# Extra RTT (ms) by seconds since start, aligned with the A/B workload phases.
extra_rtt() {
    case $1 in
        2[0-2]) echo 100 ;;   # download: owd delta +50 ms, a proportional cut
        3[5-7]) echo 200 ;;   # download: owd delta +100 ms, the maximum cut
        5[8-9]) echo 100 ;;   # idle: bufferbloat without load
        8[0-2]) echo 60 ;;    # upload: owd delta +30 ms, at the delay threshold
        9[0-1]) echo 160 ;;   # upload
        *) echo 0 ;;
    esac
}

sequence=0
while :; do
    for reflector in "${reflectors[@]}"; do
        now_us=${EPOCHREALTIME/.}
        rtt=$(( 20 + $(extra_rtt $(( (now_us - start_us) / 1000000 ))) + sequence % 3 ))
        # fping prints five decimal places.
        printf '[%d.%05d] %s : [%d], 64 bytes, %d.00 ms (%d.00 avg, 0%% loss)\n' \
            $((now_us / 1000000)) $((now_us % 1000000 / 10)) "${reflector}" "${sequence}" "${rtt}" "${rtt}"
        read -r -t 0.05 -u "${wait_fd}" _
    done
    sequence=$((sequence + 1))
done
