# pace.sh: frame pacing. Start, title,
# continue; then the speed test: a straight ${WALK:-4} s walk (left stick forward, no camera), still for 8 s (in-app
# shots, LARECOMP_SHOT_AT, show where it stopped); then ${STAND:-25} s standing still, then ${PLAY:-60} s of the street
# walk (idle.sh's loop). The compositor's present times every 4 s from the speed test on (sf_<n>.txt, 128 frames
# each: analysis/pace.py joins them). Each log line also carries /proc/uptime (= CLOCK_MONOTONIC: the sf times).
. /data/local/tmp/pzx/lib.sh
logu() { echo "$(el) $(cut -d' ' -f1 /proc/uptime) $*" >> $OUT/events.txt; }
cooldown_xo ${COOLXO:-55} ${COOLMAX:-90}
logu "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; logu tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 8
LAYER=""
( k=0; while :; do
    [ -z "$LAYER" ] && LAYER=$(dumpsys SurfaceFlinger --list | grep "^SurfaceView\[org.recomp.za" | grep "(BLAST)" | head -1)
    k=$((k+1)); { echo "layer $LAYER"; dumpsys SurfaceFlinger --latency "$LAYER"; } > $OUT/sf_$(printf %03d $k).txt
    sleep 4; done ) &
SF=$!
logu walktest; stick 0 -32767 ${WALK:-4}; logu walktest-end
sleep 8
logu stand; sleep ${STAND:-25}
logu walk
E=$(( $(el) + ${PLAY:-60} ))
while [ $(el) -lt $E ]; do stick 0 -32767 3; rstick 20000 0 0.8; stick 20000 -20000 2; rstick -20000 0 0.6; dead && break; done
logu "end $(temps) app $(pidof_app)"
kill $SF $SP 2>/dev/null
dumpsys display | grep -m1 mActiveSfDisplayMode > $OUT/display-mode.txt
exit 0
