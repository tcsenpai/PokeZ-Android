#!/bin/bash
# run.sh <apk-name> <scenario> <outdir> <env|-> <ini|-> [keepcache]: one scripted run on the device, under the device
# lock. The APK is always named and installed first (never "whatever is installed": a peer may have changed it).
#  1. installs PZ_APK_DIR/<apk-name>.apk; keeps the device's save (save-before.tar) and ini (ini-before.ini);
#  2. pushes env.txt (<env>, or none with -) and larecomp.ini (<ini>, or the device's with -); installs the run's save;
#     clears files/build/frames and, unless keepcache, the pipeline cache;
#  3. runs scenarios/<scenario> on the device (with scenarios/lib.sh), then pulls its out/ (events.txt, ts.txt, ...),
#     lasrecomp.log, the ini, file listings and the in-app frames;
#  4. always, even on failure or Ctrl-C: puts the save and the ini back and removes env.txt.
# <scenario>: a file in scenarios/ (idle or idle.sh) or a path. <env>/<ini>: a path or a file in presets/.
# <outdir>: under PZ_RUNS (or an absolute path). Results: <outdir>/.
#   SAVE_IN=<tar>|new   the save the run starts from (default PZ_RUN_SAVE; empty: the device's own; new: no save)
#   SCEN_ENV="PLAY=60"  variables for the scenario       PULL_FILES="a b"   extra files under files/ to pull
#   SAVE_OUT=1          also keep the save as it was at the end of the run (save-after.tar)
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ $# -ge 5 ] || die "usage: run.sh <apk-name> <scenario> <outdir> <env|-> <ini|-> [keepcache] (--help)"
NAME=$1 SCEN=$2 OUTD=$3 ENVF=$4 INIF=$5 KEEP=${6:-}
APK=$(apk_file "$NAME")
preset() {  # <path or presets/ name> -> path
    if [ -f "$1" ]; then echo "$1"; elif [ -f "$PZ_DEV/presets/$1" ]; then echo "$PZ_DEV/presets/$1"
    else die "no file $1 (nor presets/$1)"; fi
}
if [ -f "$SCEN" ]; then :; elif [ -f "$PZ_DEV/scenarios/$SCEN" ]; then SCEN=$PZ_DEV/scenarios/$SCEN
elif [ -f "$PZ_DEV/scenarios/$SCEN.sh" ]; then SCEN=$PZ_DEV/scenarios/$SCEN.sh; else die "no scenario $SCEN"; fi
[ "$ENVF" = - ] || ENVF=$(preset "$ENVF")
[ "$INIF" = - ] || INIF=$(preset "$INIF")
SAVE_IN=${SAVE_IN-${PZ_RUN_SAVE:-}}
[ -z "$SAVE_IN" ] || [ "$SAVE_IN" = new ] || [ -f "$SAVE_IN" ] || die "no save tar $SAVE_IN"
case $OUTD in /*) L=$OUTD ;; *) L=$PZ_RUNS/$OUTD ;; esac
[ ! -e "$L" ] || die "$L exists"
mkdir -p "$L"
F=$APP_FILES D=$DEV_TMP

device_lock
say "$OUTD: $NAME, $(basename "$SCEN"), env ${ENVF##*/}, ini ${INIF##*/}, save ${SAVE_IN:-on the device}"
adb_ install -r "$APK" | tail -1
adb_ shell am force-stop "$PKG"
pull_save_tar "$L/save-before.tar" || die "could not back up the device's save: not touching it"
adb_ exec-out "run-as $PKG cat $F/larecomp.ini" > "$L/ini-before.ini" 2>/dev/null || true

put_back() {
    set +e
    adb_ shell am force-stop "$PKG"
    push_save_tar "$L/save-before.tar"
    [ -s "$L/ini-before.ini" ] && push_app_file "$L/ini-before.ini" larecomp.ini
    remove_env_txt
    adb_ shell rm -rf "$D/out" "$D/scen.sh"
    echo "put back: save $(device_save_md5)(before: $(tar_save_md5 "$L/save-before.tar"))"
}
trap put_back EXIT

adb_ shell rm -rf "$D/out"
adb_ shell mkdir -p "$D/out"
adb_ push -q "$PZ_DEV/scenarios/lib.sh" "$D/lib.sh"
adb_ push -q "$SCEN" "$D/scen.sh"
if [ "$ENVF" != - ]; then push_app_file "$ENVF" env.txt; cp "$ENVF" "$L/env.txt"; else remove_env_txt; fi
if [ "$INIF" != - ]; then push_app_file "$INIF" larecomp.ini; cp "$INIF" "$L/larecomp-in.ini"; fi
case $SAVE_IN in
    "") ;;
    new) as_app rm -rf "$F/save" ;;
    *) push_save_tar "$SAVE_IN" ;;
esac
as_app ls -laR "$F/save" > "$L/save-installed.txt" 2>&1 || true
as_app rm -rf "$F/build/frames"
as_app mkdir -p "$F/build/frames"
[ "$KEEP" = keepcache ] || as_app rm -rf "$F/cache"
as_app ls -la "$F/cache" > "$L/cache-before.txt" 2>&1 || true
adb_ shell "settings get system performance_mode; settings get system fan_mode; settings get system peak_refresh_rate; \
    cat /sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq /sys/class/kgsl/kgsl-3d0/gpuclk; true" > "$L/device-modes.txt"

say "$OUTD: scenario"
adb_ shell "${SCEN_ENV:-} sh $D/scen.sh" || echo "scenario exit $?"

set +e
adb_ shell am force-stop "$PKG"
adb_ pull -q "$D/out/." "$L/"
adb_ exec-out "run-as $PKG cat $F/lasrecomp.log" > "$L/lasrecomp.log"
adb_ exec-out "run-as $PKG cat $F/larecomp.ini" > "$L/larecomp.ini"
as_app ls -la "$F" "$F/cache" > "$L/files.txt" 2>&1
as_app ls -laR "$F/save" > "$L/save-after.txt" 2>&1
[ -z "${SAVE_OUT:-}" ] || pull_save_tar "$L/save-after.tar"
pull_frames "$L"
for f in ${PULL_FILES:-}; do adb_ exec-out "run-as $PKG cat $F/$f" > "$L/$(basename "$f")"; done
put_back
trap - EXIT
say "$OUTD: done, $L"
[ "$NAME" = "${PZ_PLAYABLE_APK:-}" ] || echo "note: $NAME is installed; device/restore.sh puts $PZ_PLAYABLE_APK back"
