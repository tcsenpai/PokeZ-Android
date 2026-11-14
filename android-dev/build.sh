#!/bin/bash
# build.sh <apk-name>: an APK of the current checkout, HEAD plus the uncommitted changes under runtime/ (tracked
# diffs and new untracked files; uncommitted edits elsewhere stay out). libmain.so is compiled in the build root
# (build/android-final, VOLK=1: Vulkan through volk, so libadrenotools can load a custom driver) and packaged with the
# game code library from build/android-za (lasgame.so, built once by build_za.sh: a runtime change never needs it
# rebuilt). Holds PZ_BUILD_LOCK for the whole build.
# Output: PZ_APK_DIR/<apk-name>.apk, plus <apk-name>.patch (the runtime diff it was built with).
# The APK carries code generated from your own game dump: never share it.
#   FORCE=1                           overwrite an existing <apk-name>.apk
#   EXTRA_PATCHES="a.patch b.patch"   experiment patches applied on top (-p1, repo-relative paths inside)
set -euo pipefail
# shellcheck source=lib/common.sh
. "$(dirname "$0")/lib/common.sh" "$@"
NAME=${1:-}
[ -n "$NAME" ] || die "usage: build.sh <apk-name> (--help)"
[[ $NAME =~ ^[A-Za-z0-9._-]+$ ]] || die "apk name: letters, digits, . _ - only"
APK=$PZ_APK_DIR/$NAME.apk
[ ! -e "$APK" ] || [ -n "${FORCE:-}" ] || die "$APK exists (FORCE=1 to overwrite)"
W=$PZ_BUILD_ROOT/lasrecomp
for f in "$W/build/android-za/lasgame.so" "$W/build/android/deps/lib" "$PZ_ADRENOTOOLS/build/libadrenotools.a"; do
    [ -e "$f" ] || die "missing $f (README: Setup)"
done

exec 8>"$PZ_BUILD_LOCK"
say "waiting for $PZ_BUILD_LOCK"
flock 8
say "building $NAME from $(git -C "$PZ_REPO" rev-parse --short HEAD)"

mkdir -p "$PZ_APK_DIR" "$PZ_TMP"
S=$PZ_TMP/build-src
rm -rf "$S"; mkdir -p "$S"
git -C "$PZ_REPO" archive HEAD | tar -x -C "$S"
git -C "$PZ_REPO" diff HEAD -- runtime > "$PZ_APK_DIR/$NAME.patch"
[ -s "$PZ_APK_DIR/$NAME.patch" ] && patch -s -d "$S" -p1 --no-backup-if-mismatch < "$PZ_APK_DIR/$NAME.patch"
mapfile -t NEW < <(git -C "$PZ_REPO" ls-files --others --exclude-standard -- runtime)
[ ${#NEW[@]} -eq 0 ] || { (cd "$PZ_REPO" && cp --parents -t "$S" "${NEW[@]}"); echo "untracked runtime files: ${NEW[*]}"; }
for p in ${EXTRA_PATCHES:-}; do patch -s -d "$S" -p1 --no-backup-if-mismatch < "$p"; done
echo "runtime diff: $(grep -c '^+++ ' "$PZ_APK_DIR/$NAME.patch" || true) file(s)"

"$PZ_DEV/setup/pz.sh" bash "$PZ_DEV/lib/build-inner.sh" "$S" "$APK" "$PZ_TMP/build-$NAME.log"
say "done: $APK"
