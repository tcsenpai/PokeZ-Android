#!/bin/bash
# adrenotools.sh [turnip.so]: libadrenotools for the VOLK=1 build (Makefile ADRENOTOOLS=), arm64 with the NDK.
#  - clones bylaws/libadrenotools (with its linkernsbypass submodule) into PZ_ADRENOTOOLS if it is not there;
#  - builds PZ_ADRENOTOOLS/build/{libadrenotools.a, lib/linkernsbypass/liblinkernsbypass.a, src/hook/lib*.so};
#  - stages the hook libraries in the build root's deps (build/android/deps/adrenotools/), where make_project.sh
#    packages them; with [turnip.so], that Turnip build too, as libvulkan_turnip.so (the APK's default driver).
# Needs cmake and ninja on the host (dnf install cmake ninja-build).
#   ADRENOTOOLS_REV=<commit>   the revision to check out on clone (default: 8fae8ce)
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
REV=${ADRENOTOOLS_REV:-8fae8ce}
if [ ! -f "$PZ_ADRENOTOOLS/CMakeLists.txt" ]; then
    git clone --recursive https://github.com/bylaws/libadrenotools "$PZ_ADRENOTOOLS"
    git -C "$PZ_ADRENOTOOLS" checkout -q "$REV"
    git -C "$PZ_ADRENOTOOLS" submodule update --init --recursive
fi
cmake -S "$PZ_ADRENOTOOLS" -B "$PZ_ADRENOTOOLS/build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$PZ_NDK/build/cmake/android.toolchain.cmake" -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-31 -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -Wno-dev > /dev/null
cmake --build "$PZ_ADRENOTOOLS/build" -j"$PZ_JOBS" \
    --target adrenotools main_hook hook_impl file_redirect_hook gsl_alloc_hook
D=$PZ_BUILD_ROOT/lasrecomp/build/android/deps/adrenotools
mkdir -p "$D"
for h in hook_impl main_hook file_redirect_hook gsl_alloc_hook; do cp "$PZ_ADRENOTOOLS/build/src/hook/lib$h.so" "$D/"; done
[ -z "${1:-}" ] || cp "$1" "$D/libvulkan_turnip.so"
ls -l "$D"
