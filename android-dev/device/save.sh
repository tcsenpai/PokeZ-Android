#!/bin/bash
# save.sh <command>: the player's save on the device (files/save/save_0/{main,backup}). Back it up before anything
# else touches the device. Holds the device lock.
#   save.sh md5                 the device save's md5 (main, backup)
#   save.sh backup [name]       device save -> PZ_SAVE_BACKUPS/<name>/save.tar + md5.txt (default name:
#                               save-<date>-<time>); checked: the tar's md5 must equal the device's
#   save.sh verify <backup>     does the device save equal <backup>?
#   save.sh restore <backup>    back up the device save first (pre-restore-<date>-<time>), then install <backup>
#                               and verify it
# <backup>: a backup's directory or name under PZ_SAVE_BACKUPS, or a save tar (save/save_0/{main,backup} inside).
set -euo pipefail
# shellcheck source=../lib/common.sh
. "$(dirname "$0")/../lib/common.sh" "$@"

backup_tar() {  # <backup> -> its tar path
    local b=$1
    [ -f "$b" ] && { echo "$b"; return; }
    [ -d "$b" ] || b=$PZ_SAVE_BACKUPS/$b
    [ -f "$b/save.tar" ] || die "no backup $1 (no $b/save.tar)"
    echo "$b/save.tar"
}

backup() {  # <name>
    local d=$PZ_SAVE_BACKUPS/$1 dev tar
    [ ! -e "$d" ] || die "$d exists"
    mkdir -p "$d"
    pull_save_tar "$d/save.tar" || { rm -rf "$d"; die "nothing backed up"; }
    dev=$(device_save_md5); tar=$(tar_save_md5 "$d/save.tar")
    printf 'main backup\n%s\n' "$tar" > "$d/md5.txt"
    [ "$dev" = "$tar" ] || die "backup differs from the device: device $dev, tar $tar ($d)"
    echo "backed up: $d (md5 $tar)"
}

verify() {  # <backup>
    local dev want
    dev=$(device_save_md5); want=$(tar_save_md5 "$(backup_tar "$1")")
    echo "device $dev"
    echo "backup $want"
    if [ "$dev" = "$want" ]; then echo "same save"; else echo "DIFFERENT"; return 1; fi
}

cmd=${1:-}
case $cmd in
    md5) device_lock; save_md5 ;;
    backup) device_lock; backup "${2:-save-$(date +%Y%m%d-%H%M)}" ;;
    verify) [ -n "${2:-}" ] || die "usage: save.sh verify <backup>"; device_lock; verify "$2" ;;
    restore)
        [ -n "${2:-}" ] || die "usage: save.sh restore <backup>"
        T=$(backup_tar "$2")
        device_lock
        adb_ shell am force-stop "$PKG"
        backup "pre-restore-$(date +%Y%m%d-%H%M%S)"
        push_save_tar "$T"
        verify "$T" ;;
    *) die "usage: save.sh md5|backup [name]|verify <backup>|restore <backup> (--help)" ;;
esac
