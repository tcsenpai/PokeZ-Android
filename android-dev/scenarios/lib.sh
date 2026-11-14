# lib.sh: on-device helpers for the scenarios (Android's sh; device/run.sh pushes this to /data/local/tmp/pzx and runs
# the scenario there). Input goes straight into the Thor's built-in pad with sendevent; out/ holds what run.sh pulls:
# events.txt (seconds since start + event), ts.txt (tsampler), sf_*.txt (sflat), anything a scenario adds.
PKG=org.recomp.za
EV=${EV:-/dev/input/event9}   # "Odin Controller" (the Thor's built-in pad; getevent -lp to check)
OUT=${OUT:-/data/local/tmp/pzx/out}
mkdir -p $OUT
T0=$(date +%s)
el() { echo $(( $(date +%s) - T0 )); }
log() { echo "$(el) $*" >> $OUT/events.txt; }
log "epoch $T0"
syn() { sendevent $EV 0 0 0; }
# Key codes: BTN_SOUTH 304, EAST 305, NORTH 307, WEST 308, TL 310, TR 311, TL2 312, TR2 313, SELECT 314 (-),
# START 315 (+), DPAD up 544 down 545 left 546 right 547.
# Face buttons as the game reads them. presets/ini-thor.ini keeps the Nintendo layout (A east, B south); an ini with
# bind_pad_a = south / bind_pad_b = east (Xbox positions): SCEN_ENV="PAD=xbox".
if [ "${PAD:-nintendo}" = xbox ]; then BA=304 BB=305; else BA=305 BB=304; fi
BX=307 BY=308
btn() { sendevent $EV 1 $1 1 2>>$OUT/err.txt; syn; sleep ${2:-0.15}; sendevent $EV 1 $1 0; syn; log "btn $1"; }
stick() { sendevent $EV 3 0 $1; sendevent $EV 3 1 $2; syn; sleep $3; sendevent $EV 3 0 0; sendevent $EV 3 1 0; syn; log "stick $1 $2 $3"; }
rstick() { sendevent $EV 3 2 $1; sendevent $EV 3 5 $2; syn; sleep $3; sendevent $EV 3 2 0; sendevent $EV 3 5 0; syn; log "rstick $1 $2 $3"; }
# triggers: ZL = ABS_BRAKE (10) + BTN_TL2 312, ZR = ABS_GAS (9) + BTN_TR2 313; hold $2 s
trig() { a=$([ $1 = zl ] && echo 10 || echo 9); k=$([ $1 = zl ] && echo 312 || echo 313); sendevent $EV 3 $a 32767; sendevent $EV 1 $k 1; syn; sleep ${2:-0.3}; sendevent $EV 3 $a 0; sendevent $EV 1 $k 0; syn; log "trig $1 $2"; }
act() { log "ACT $*"; }
# Screenshots: adb screencap / screenrecord return black frames on the Thor (since a GPU timeout; a reboot may bring
# them back). Use the app's own capture (env LARECOMP_SHOT_AT / LARECOMP_SCREENSHOT_EVERY, pulled by run.sh). shot
# only logs a marker unless SCREENCAP=1; rec (screenrecord, 175 s max) does nothing unless SCREENCAP=1.
shot() { if [ -n "$SCREENCAP" ]; then screencap -p $OUT/shot_$(printf %04d $(el))_$1.png; fi; log "shot $1"; }
rec() { if [ -n "$SCREENCAP" ]; then screenrecord --time-limit 175 --size 1280x720 --bit-rate 8000000 $OUT/$1.mp4; fi; }
rec_stop() { if [ -n "$SCREENCAP" ]; then pkill -INT screenrecord; sleep 2; fi; }
pidof_app() { pidof $PKG; }
temps() {  # cpu max, gpu max, xo-therm, battery (deg C)
    c=0; g=0; x=0; b=0
    for z in /sys/class/thermal/thermal_zone*; do
        ty=$(cat $z/type); v=$(cat $z/temp 2>/dev/null); v=$(( ${v:-0} / 1000 ))
        case $ty in cpu-*) [ $v -gt $c ] && c=$v;; gpuss-*) [ $v -gt $g ] && g=$v;; xo-therm) x=$v;; battery) b=$v;; esac
    done
    echo "cpu=$c gpu=$g xo=$x batt=$b"
}
cooldown() {  # wait (max ${2:-240} s) until the hottest CPU zone is at or under $1 C
    lim=${1:-60}; mx=${2:-240}; n=0
    while :; do c=$(temps | sed 's/cpu=\([0-9]*\).*/\1/'); [ $c -le $lim ] && break; [ $n -ge $mx ] && break; sleep 10; n=$((n+10)); done
    log "cooldown done after ${n}s: $(temps)"
}
cooldown_xo() {  # wait (max ${2:-240} s) until the board (xo-therm, slow) is at or under $1 C: the same start for every run
    lim=${1:-53}; mx=${2:-240}; n=0
    while :; do x=$(temps | sed 's/.*xo=\([0-9]*\).*/\1/'); [ $x -le $lim ] && break; [ $n -ge $mx ] && break; sleep 10; n=$((n+10)); done
    log "cooldown done after ${n}s: $(temps)"
}
# light sampler: every $1 s the app's per-thread CPU ticks (/proc/<pid>/task/*/stat), RSS, temps, GPU busy/clock,
# battery current/voltage, whole-system CPU (/proc/stat cpu lines) -> ts.txt (analysis/samples.py)
tsample() {
    p=$(pidof_app); t=$(el)
    echo "== $t $(date +%s.%N) pid ${p:-none} $(temps) gpubusy=$(cat /sys/class/kgsl/kgsl-3d0/gpu_busy_percentage | tr -d ' %') gpuclk=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk) cur=$(cat /sys/class/power_supply/battery/current_now) volt=$(cat /sys/class/power_supply/battery/voltage_now)" >> $OUT/ts.txt
    grep -E "^cpu" /proc/stat >> $OUT/ts.txt
    [ -n "$p" ] || return
    grep -E "VmRSS|VmHWM" /proc/$p/status >> $OUT/ts.txt
    for s in /proc/$p/task/*/stat; do cat $s; done 2>/dev/null | sed 's/^/T /' >> $OUT/ts.txt
}
tsampler() { while true; do tsample; sleep ${1:-5}; done; }
# the compositor's own view of the last 128 frames of the game's surface: desired / actual present / ready (ns), plus
# the display mode at that moment -> sf_<name>.txt (analysis/sf.py)
sflat() {
    l=$(dumpsys SurfaceFlinger --list | grep "^SurfaceView\[$PKG" | grep "(BLAST)" | head -1)
    dumpsys SurfaceFlinger --list | grep -i recomp > $OUT/sf_$1_list.txt
    echo "layer $l" > $OUT/sf_$1.txt
    dumpsys SurfaceFlinger --latency "$l" >> $OUT/sf_$1.txt
    dumpsys display | grep -m1 mActiveSfDisplayMode >> $OUT/sf_$1.txt
    log "sflat $1"
}
# the game stopped (device lost / fatal error shown): true once lasrecomp.log has a FATAL line or the app is gone
LOGF=/storage/emulated/0/Android/data/$PKG/files/lasrecomp.log
dead() { [ -z "$(pidof_app)" ] && return 0; grep -q "^FATAL" $LOGF 2>/dev/null && { log "app stopped: $(grep -m1 '^FATAL' $LOGF)"; return 0; }; return 1; }
