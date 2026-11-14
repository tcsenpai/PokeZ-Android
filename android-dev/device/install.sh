#!/bin/bash
# install.sh <apk-name>: adb install -r PZ_APK_DIR/<apk-name>.apk (keeps the app's data: save, ini, game files), then
# stop the app. Holds the device lock. An experimental build stays installed until device/restore.sh puts the
# playable one back: do that before you finish a device session.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ -n "${1:-}" ] || die "usage: install.sh <apk-name> (--help)"
APK=$(apk_file "$1")
device_lock
adb_ install -r "$APK" | tail -1
adb_ shell am force-stop "$PKG"
adb_ shell dumpsys package "$PKG" | grep -m1 lastUpdateTime
