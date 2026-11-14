# shellcheck shell=bash
# common.sh: sourced by every android-dev script as `. lib/common.sh "$@"`.
# Handles -h/--help (prints the calling script's header comment), loads config.sh, and defines the adb, lock and
# device helpers. Nothing here touches the device by itself.

PZ_DEV=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
PZ_REPO=$(cd "$PZ_DEV/.." && pwd)
export PZ_DEV PZ_REPO

# The calling script's header: the comment block after the shebang.
pz_help() { awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"; }
case "${1:-}" in -h|--help) pz_help; exit 0 ;; esac

die() { echo "${0##*/}: $*" >&2; exit 1; }
say() { echo "[$(date +%T)] ${0##*/}: $*"; }

[ -f "$PZ_DEV/config.sh" ] || die "no android-dev/config.sh: cp android-dev/config.example.sh android-dev/config.sh, then edit it"
# shellcheck source=../config.example.sh
. "$PZ_DEV/config.sh"

: "${PZ_WORK:?config.sh: PZ_WORK is not set}"
: "${PZ_BUILD_ROOT:=$PZ_WORK/root}"
: "${PZ_APK_DIR:=$PZ_WORK/apk}"
: "${PZ_RUNS:=$PZ_WORK/runs}"
: "${PZ_TMP:=$PZ_WORK/tmp}"
: "${PZ_SAVE_BACKUPS:=$PZ_WORK/saves}"
: "${PZ_BUILD_LOCK:=/tmp/pokez-build.lock}"
: "${PZ_DEVICE_LOCK:=/tmp/thor.lock}"
: "${PZ_JOBS:=$(nproc)}"
: "${PZ_SDK:=$HOME/Android/Sdk}"
: "${PZ_ADRENOTOOLS:=$PZ_WORK/adrenotools}"
export PZ_WORK PZ_BUILD_ROOT PZ_APK_DIR PZ_RUNS PZ_TMP PZ_SAVE_BACKUPS PZ_BUILD_LOCK PZ_DEVICE_LOCK PZ_JOBS PZ_SDK PZ_NDK \
       PZ_JAVA_HOME PZ_ADRENOTOOLS

# The app and its folders on the device
PKG=org.recomp.za
APP_FILES=/storage/emulated/0/Android/data/$PKG/files   # external data folder: env.txt, larecomp.ini, save/, log
DEV_TMP=/data/local/tmp/pzx                             # staging: adb push lands here, then run-as copies it in

adb_() { adb -s "${ADB_SERIAL:?config.sh: ADB_SERIAL is not set}" "$@"; }
# A command as the app's user (the debug APK allows run-as): files it creates belong to the app.
as_app() { adb_ shell run-as "$PKG" "$@"; }

# One device user at a time. Held until the calling script exits; scripts started under it (run.sh from texmod-ab.sh)
# see PZ_DEVICE_LOCK_HELD and do not wait for themselves.
device_lock() {
    [ "${PZ_DEVICE_LOCK_HELD:-}" = 1 ] && return 0
    exec 9>"$PZ_DEVICE_LOCK"
    say "waiting for $PZ_DEVICE_LOCK"
    flock 9
    export PZ_DEVICE_LOCK_HELD=1
}

apk_file() {
    local f="$PZ_APK_DIR/$1.apk"
    [ -f "$f" ] || die "no APK $f (name without .apk, from $PZ_APK_DIR)"
    echo "$f"
}

# Copy a host file into the app's data folder: push to the staging dir, then copy as the app.
push_app_file() {  # <host file> <name under files/>
    adb_ shell mkdir -p "$DEV_TMP"
    adb_ push -q "$1" "$DEV_TMP/stage"
    adb_ shell chmod 644 "$DEV_TMP/stage"
    as_app cp "$DEV_TMP/stage" "$APP_FILES/$2"
    adb_ shell rm -f "$DEV_TMP/stage"
}

remove_env_txt() {
    adb_ shell rm -f "$APP_FILES/env.txt"
    as_app rm -f "$APP_FILES/env.txt"
}

save_md5() { as_app md5sum "$APP_FILES/save/save_0/main" "$APP_FILES/save/save_0/backup" 2>&1; }
device_save_md5() { save_md5 | awk '{ print $1 }' | tr '\n' ' '; }
tar_save_md5() {  # <save tar>: md5 of save_0/main and save_0/backup inside it
    local t; t=$(mktemp -d)
    tar -xf "$1" -C "$t" save/save_0/main save/save_0/backup 2>/dev/null || { rm -rf "$t"; echo "unreadable"; return; }
    (cd "$t/save/save_0" && md5sum main backup) | awk '{ print $1 }' | tr '\n' ' '
    rm -rf "$t"
}

# The device's files/save as a tar (save/save_0/{main,backup}); fails unless the tar holds a save.
pull_save_tar() {  # <out tar>
    adb_ exec-out "run-as $PKG tar -cf - -C $APP_FILES save" > "$1" || true
    tar -tf "$1" 2>/dev/null | grep -qx 'save/save_0/main' || { echo "no save in the device's files/save" >&2; return 1; }
}
# Replace the device's files/save with a save tar, extracted by the app's user so it owns the files.
push_save_tar() {  # <save tar>
    tar -tf "$1" 2>/dev/null | grep -qx 'save/save_0/main' || { echo "$1: no save/save_0/main inside" >&2; return 1; }
    adb_ shell mkdir -p "$DEV_TMP"
    adb_ push -q "$1" "$DEV_TMP/save.tar"
    adb_ shell chmod 644 "$DEV_TMP/save.tar"
    as_app sh -c "'rm -rf $APP_FILES/save && tar -xf $DEV_TMP/save.tar -C $APP_FILES'"
    adb_ shell rm -f "$DEV_TMP/save.tar"
}

# The in-app captures (LARECOMP_SHOT_AT / LARECOMP_SCREENSHOT_EVERY): files/build/frames -> <dir>/frames.
# adb screencap/screenrecord are black on the Thor: these are the only screenshots.
pull_frames() {
    local out=$1
    mkdir -p "$out"
    adb_ exec-out "run-as $PKG tar -cf - -C $APP_FILES/build frames" > "$out/frames.tar" 2>/dev/null || true
    if [ -s "$out/frames.tar" ] && tar -xf "$out/frames.tar" -C "$out"; then
        echo "$(find "$out/frames" -type f | wc -l) frames in $out/frames"
    else
        echo "no frames on the device"
    fi
    rm -f "$out/frames.tar"
}
