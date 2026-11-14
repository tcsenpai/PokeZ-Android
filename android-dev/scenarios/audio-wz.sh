# audio-wz.sh: start, continue, map (+), Y = destinations, pick entry ${PICK:-0} (DPAD down x PICK), A, A (confirm), wait for the
# fast travel, then lock on (ZL) + send out (ZR) + moves (ZL + A/X/Y/B) a few times: a wild-zone battle (loud).
# Per-thread schedstat every 2 s; audio_flinger dumps before/after.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-56} ${COOLMAX:-40}
log "start $(temps)"
ssample() {
    p=$(pidof_app); [ -n "$p" ] || return
    echo "== $(el) $(date +%s.%N)" >> $OUT/sched.txt
    for t in /proc/$p/task/*; do echo "S ${t##*/} $(cat $t/comm) $(cat $t/schedstat) | $(cat $t/stat)"; done >> $OUT/sched.txt 2>/dev/null
}
( while :; do ssample; sleep 2; done ) &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 8
dumpsys media.audio_flinger > $OUT/af_before.txt 2>&1
act map; btn 315; sleep 4
act destinations; btn $BY; sleep 3
i=0; while [ $i -lt ${PICK:-0} ]; do btn ${PICKKEY:-545}; sleep 0.6; i=$((i+1)); done  # (PICKKEY=544: up)
sleep 1; act pick; btn $BA; sleep 2; btn $BA; sleep 2; btn $BA
act travel; sleep ${TRAVEL:-15}
dead && exit 0
for r in 1 2 3; do
  act look-$r; rstick 25000 0 1.2; sleep 1
  act lockon-$r; sendevent $EV 3 10 32767; sendevent $EV 1 312 1; syn; sleep 1; trig zr 0.3; sleep 2
  for k in $BA $BX $BY $BB; do btn $k; sleep 2; done
  sendevent $EV 3 10 0; sendevent $EV 1 312 0; syn; sleep 1
  act walk-$r; stick 0 -32767 2; dead && break
done
act fight; E=$(( $(el) + ${FIGHT:-30} ))
while [ $(el) -lt $E ]; do sendevent $EV 3 10 32767; sendevent $EV 1 312 1; syn; sleep 0.5; btn $BA; sleep 1.5; btn $BX; sleep 1.5; sendevent $EV 3 10 0; sendevent $EV 1 312 0; syn; stick 20000 -20000 1; dead && break; done
dumpsys media.audio_flinger > $OUT/af_after.txt 2>&1
act end
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
exit 0
