#!/bin/sh
# Three rounds with upstream default logging, rotating the order to spread
# drift, then one round with every record output enabled.
P=/tmp/perfcmp
for spec in \
    "none autorate adapt" \
    "autorate adapt none" \
    "adapt none autorate"
do
    ROUND=$((${ROUND:-0} + 1))
    for n in $spec; do
        echo "$(date +%T) start defaults $n round $ROUND"
        sh "$P/run.sh" "$n" defaults "$ROUND" || echo "$(date +%T) FAILED defaults $n round $ROUND"
        sleep 10
    done
done
for n in autorate adapt; do
    echo "$(date +%T) start stats $n round 1"
    sh "$P/run.sh" "$n" stats 1 || echo "$(date +%T) FAILED stats $n round 1"
    sleep 10
done
echo "$(date +%T) done"
