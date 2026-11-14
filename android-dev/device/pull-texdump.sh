#!/bin/bash
# pull-texdump.sh <dest dir>: the texture dump (files/textures/dump, written with LARECOMP_TEX_DUMP=1:
# presets/env-texdump.txt) -> <dest dir>/dump. The dump is the game's own art: keep it out of the repo.
# Holds the device lock.
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ -n "${1:-}" ] || die "usage: pull-texdump.sh <dest dir> (--help)"
mkdir -p "$1"
device_lock
adb_ exec-out "tar -cf - -C $APP_FILES/textures dump" | tar -xf - -C "$1"
adb_ shell "ls $APP_FILES/textures/dump | wc -l; du -sh $APP_FILES/textures/dump"
