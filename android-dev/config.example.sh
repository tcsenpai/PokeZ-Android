# shellcheck shell=bash disable=SC2034
# android-dev configuration. Copy to config.sh (gitignored) and edit: every android-dev script reads its paths here.
# Nothing personal goes into the tracked scripts; it all lives in config.sh.

# Work directory outside the repo: build root, APKs, run outputs, temp files.
PZ_WORK=$HOME/pz-work

# The build root: mounted as /root inside setup/pz.sh, holding lasrecomp/ (the build copy of the checkout with build/
# and generated-android-za/) and za-extracted/ (import_za.sh's exefs + romfs.bin).
PZ_BUILD_ROOT=$PZ_WORK/root

PZ_APK_DIR=$PZ_WORK/apk            # build.sh writes <name>.apk (+ <name>.patch) here
PZ_RUNS=$PZ_WORK/runs              # device/run.sh writes <outdir>/ here
PZ_TMP=$PZ_WORK/tmp                # TMPDIR for Gradle and the build

# Toolchain (setup/pz.sh mounts them where the upstream scripts expect: /opt/android-ndk, /opt/android-sdk)
PZ_SDK=$HOME/Android/Sdk
PZ_NDK=$PZ_SDK/ndk/29.0.14206865
PZ_JAVA_HOME=$HOME/.local/opt/jdk21
PZ_ADRENOTOOLS=$PZ_WORK/adrenotools   # libadrenotools source with build/ (setup/adrenotools.sh)
PZ_JOBS=16

# Device
ADB_SERIAL=XXXXXXXX                # adb devices -l
PZ_PLAYABLE_APK=pokez-final4       # the build device/restore.sh leaves installed (name in PZ_APK_DIR, no .apk)
PZ_USER_INI=                       # the player's larecomp.ini to put back; empty: presets/ini-thor.ini

# Saves
PZ_SAVE_BACKUPS=$PZ_WORK/saves     # device/save.sh backup writes <name>/ here
PZ_RUN_SAVE=                       # save tar (save/save_0/{main,backup}) a scenario run starts from; empty: the device's

# Locks: one build and one device user at a time, across every agent and shell on this machine
PZ_BUILD_LOCK=/tmp/pokez-build.lock
PZ_DEVICE_LOCK=/tmp/thor.lock
