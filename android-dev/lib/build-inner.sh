#!/bin/bash
# build-inner.sh <source dir> <apk out> <log>: build.sh's part inside the build root's view (setup/pz.sh), under its
# build lock. Not meant to be run by hand.
set -euo pipefail
S=$1 APK=$2 LOG=$3
B=build/android-final GB=build/android-za
cd /root/lasrecomp
# No -t: a file whose content changed gets a fresh mtime and make rebuilds it; unchanged files keep theirs (git
# archive stamps the commit time, which would hide a change from make after building a newer commit).
rsync -rlpD --delete -c --exclude=.git --exclude=build --exclude=extracted --exclude=tools/venv \
    --exclude='generated-*' --exclude=eval "$S/" ./
mkdir -p "$B"
ln -f "$GB/lasgame.so" "$B/lasgame.so"
ln -f "$GB/lasgame.so.id" "$B/lasgame.so.id"
echo "libmain.so: make -j$PZ_JOBS (log $LOG)"
make -j"$PZ_JOBS" PLATFORM=android DEPS=/root/lasrecomp/build/android/deps GEN=generated-android-za B="$B" VOLK=1 \
    GENARCH="" ADRENOTOOLS="$PZ_ADRENOTOOLS" "$B/libmain.so" > "$LOG" 2>&1 \
    || { grep -E "error" "$LOG" | head -20; echo "make failed: $LOG"; exit 1; }
ls -l "$B/libmain.so"
GAME_BUILD=/root/lasrecomp/$B bash tools/linux/android-port/make_project.sh > /dev/null
bash tools/linux/android-port/build_apk.sh >> "$LOG" 2>&1 || { tail -20 "$LOG"; echo "build_apk.sh failed: $LOG"; exit 1; }
cp build/android/app/app/build/outputs/apk/debug/app-debug.apk "$APK"
ls -l "$APK"
