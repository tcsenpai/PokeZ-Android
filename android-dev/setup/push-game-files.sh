#!/bin/bash
# push-game-files.sh [dir]: copy the game files (exefs/ and romfs.bin, ~6 GB, import_za.sh's output; default
# PZ_BUILD_ROOT/za-extracted) into the app's files/extracted/ on the device. Once per device, after the first install.
# adb push straight into Android/data fails (secure_mkdirs): the files go to /data/local/tmp first, then the app's
# user copies them in. About 3 minutes over USB 3. Holds the device lock.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
SRC_DIR=${1:-$PZ_BUILD_ROOT/za-extracted}
[ -f "$SRC_DIR/exefs/main" ] && [ -f "$SRC_DIR/romfs.bin" ] || die "no exefs/main + romfs.bin in $SRC_DIR"
device_lock
T=/data/local/tmp/pz-game
adb_ shell am force-stop "$PKG"
adb_ shell mkdir -p "$T"
adb_ push "$SRC_DIR/exefs" "$T/" | tail -1
adb_ push "$SRC_DIR/romfs.bin" "$T/" | tail -1
adb_ shell chmod -R a+rX "$T"
as_app sh -c "'mkdir -p $APP_FILES/extracted && cp -r $T/exefs $T/romfs.bin $APP_FILES/extracted/'"
as_app ls -la "$APP_FILES/extracted"
adb_ shell rm -rf "$T"
