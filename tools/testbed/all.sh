#!/bin/sh
# Runs every variant once, in order, on the namespace testbed.
T=/tmp/cake-adapt-test
OLD=/usr/sbin/cake-adapt          # committed c9d2de0: cake-autorate behavior
NEW=$T/bin/cake-adapt-wip         # drain + shared-delay attribution
echo "$(date +%T) begin; installed $(apk info -v 2>/dev/null | grep '^cake-adapt-')"
sh "$T/run.sh" V0-old-fping        $OLD fping    10 30 60
sh "$T/run.sh" V1-new-fping        $NEW fping    10 30 60 150
sh "$T/run.sh" V2-new-fping-low    $NEW fping     5 20 50 150
sh "$T/run.sh" V3-old-fpingts      $OLD fping-ts 10 30 60
sh "$T/run.sh" V4-new-fpingts      $NEW fping-ts 10 30 60 150
sh "$T/run.sh" V5-new-fpingts-low  $NEW fping-ts  5 20 50 150
echo "$(date +%T) done"
