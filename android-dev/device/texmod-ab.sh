#!/bin/bash
# texmod-ab.sh <apk-name> <pack dir> <outdir>: an A/B run of an HD texture pack. Pushes <pack dir> (DDS/PNG files named
# by texture hash) to files/textures/hd, runs scenarios/texmod.sh (still-camera holds) with presets/env-texab.txt
# (LARECOMP_TEX_HD_FLIP toggles HD/original every 120 frames, in-app frames every 60), then removes the pack and env.txt
# and leaves the device as device/restore.sh does. Compare with analysis/texmod/ab.py <run dir> <out dir>.
# Holds the device lock throughout.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ $# -ge 3 ] || die "usage: texmod-ab.sh <apk-name> <pack dir> <outdir> (--help)"
NAME=$1 PACK=$2 OUTD=$3
[ -d "$PACK" ] || die "no pack dir $PACK"
apk_file "$NAME" > /dev/null
device_lock
F=$APP_FILES
clean() {
    adb_ shell am force-stop "$PKG"
    adb_ shell rm -rf "$F/textures/hd"
    as_app rm -rf "$F/textures/hd"
}
trap clean EXIT
adb_ shell rm -rf "$F/textures/hd"
adb_ shell mkdir -p "$F/textures/hd"
adb_ push -q "$PACK/." "$F/textures/hd/"
echo "pack: $(adb_ shell ls "$F/textures/hd" | wc -l) files"
"$PZ_DEV/device/run.sh" "$NAME" texmod.sh "$OUTD" env-texab.txt ini-thor.ini keepcache || echo "run.sh exit $?"
clean
trap - EXIT
"$PZ_DEV/device/restore.sh"
