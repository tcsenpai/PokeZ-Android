#!/bin/bash
# restore.sh: leave the device playable, as the player expects it after a device session:
# PZ_PLAYABLE_APK installed, the player's ini (PZ_USER_INI; empty: presets/ini-thor.ini), no env.txt, app stopped.
# The save and files/textures (the player's own packs) are not touched: it prints the save's md5 to check against your
# backup (device/save.sh verify). Holds the device lock.
#   NOINSTALL=1   keep the installed APK
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
INI=${PZ_USER_INI:-$PZ_DEV/presets/ini-thor.ini}
[ -f "$INI" ] || die "no ini $INI"
[ -n "${NOINSTALL:-}" ] || APK=$(apk_file "${PZ_PLAYABLE_APK:?config.sh: PZ_PLAYABLE_APK is not set}")
device_lock
adb_ shell am force-stop "$PKG"
[ -n "${NOINSTALL:-}" ] || { echo "install $PZ_PLAYABLE_APK"; adb_ install -r "$APK" | tail -1; }
push_app_file "$INI" larecomp.ini
remove_env_txt
adb_ shell am force-stop "$PKG"
echo "ini md5:  $(as_app md5sum "$APP_FILES/larecomp.ini" | cut -d' ' -f1) (want $(md5sum < "$INI" | cut -d' ' -f1))"
echo "save md5: $(save_md5 | tr '\n' ' ')"
echo "env.txt:  $(as_app ls "$APP_FILES/env.txt" 2>&1 | tr -d '\r')"
echo "app pid:  '$(adb_ shell pidof "$PKG" || true)' (want empty)"
adb_ shell dumpsys package "$PKG" | grep -m1 lastUpdateTime
