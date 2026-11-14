#!/bin/bash
# pull-frames.sh <outdir> [--clear]: the in-app captures from the device into <outdir>/frames (under PZ_RUNS unless
# absolute). adb screencap and screenrecord return black frames on the Thor, so screenshots come from the app itself:
# put LARECOMP_SHOT_AT=<frame,frame,...> or LARECOMP_SCREENSHOT_EVERY=<n> (+ LARECOMP_SCREENSHOT_FROM=<frame>) in
# env.txt (presets/env-shots.txt), play, then pull. --clear empties files/build/frames afterwards. Holds the device lock.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ -n "${1:-}" ] || die "usage: pull-frames.sh <outdir> [--clear] (--help)"
case $1 in /*) L=$1 ;; *) L=$PZ_RUNS/$1 ;; esac
device_lock
pull_frames "$L"
[ "${2:-}" != --clear ] || { as_app rm -rf "$APP_FILES/build/frames"; as_app mkdir -p "$APP_FILES/build/frames"; echo cleared; }
