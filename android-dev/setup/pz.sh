#!/bin/bash
# pz.sh <command...>: run a command in the build root's view, so the upstream tools/linux/android-port scripts run
# unmodified, without root: a user namespace (bwrap) where /root = PZ_BUILD_ROOT, /opt/android-ndk = PZ_NDK and
# /opt/android-sdk = PZ_SDK. Starts in /root/lasrecomp. Everything else on the host is visible at its own path.
#   setup/pz.sh bash tools/linux/android-port/deps_android.sh all
#   setup/pz.sh bash -l                      (a shell inside, to look around)
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"
[ $# -gt 0 ] || die "usage: pz.sh <command...> (--help)"
command -v bwrap >/dev/null || die "bwrap not found (dnf install bubblewrap)"
for d in "$PZ_NDK" "$PZ_SDK"; do [ -d "$d" ] || die "missing $d (README: Setup)"; done
mkdir -p "$PZ_TMP" "$PZ_BUILD_ROOT/lasrecomp"
export SRC=$PZ_REPO OMP_NUM_THREADS=$PZ_JOBS JAVA_HOME=$PZ_JAVA_HOME DOTNET_ROLL_FORWARD=Major \
       DOTNET_CLI_TELEMETRY_OPTOUT=1 TMPDIR=$PZ_TMP JAVA_TOOL_OPTIONS=-Djava.io.tmpdir=$PZ_TMP
exec bwrap --bind / / --dev-bind /dev /dev --proc /proc --tmpfs /opt \
    --bind "$PZ_NDK" /opt/android-ndk --bind "$PZ_SDK" /opt/android-sdk \
    --bind "$PZ_BUILD_ROOT" /root --chdir /root/lasrecomp "$@"
