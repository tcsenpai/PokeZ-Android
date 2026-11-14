# audio.sh: start, title, continue, then standing, running, menus and a send-out / move attempt (loud SFX). Every 2 s the app's
# per-thread schedstat (run ns, runqueue-wait ns, slices) + stat; audio_flinger dumps before/after (track underruns).
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
act stand; E=$(( $(el) + ${STAND:-20} ))
while [ $(el) -lt $E ]; do rstick 20000 0 1.5; sleep 2; rstick -20000 0 1.5; sleep 2; dead && break; done
act run; E=$(( $(el) + ${RUN:-20} ))
while [ $(el) -lt $E ]; do sendevent $EV 1 $BB 1; syn; stick 0 -32767 3; stick 20000 -20000 2; sendevent $EV 1 $BB 0; syn; rstick 25000 0 0.8; dead && break; done
dead && exit 0
act send-out; trig zr 0.3; sleep 3
act moves; sendevent $EV 3 10 32767; sendevent $EV 1 312 1; syn; sleep 0.8
for k in $BA $BX $BY $BB $BA $BX; do btn $k; sleep 1.5; done
sendevent $EV 3 10 0; sendevent $EV 1 312 0; syn; sleep 3
act menu; btn $BX; sleep 3; btn 547; sleep 1; btn 546; sleep 1; btn $BB; sleep 2; btn $BB; sleep 2
act map; btn 315; sleep 4; btn $BB; sleep 3
act walk2; E=$(( $(el) + ${WALK:-20} ))
while [ $(el) -lt $E ]; do stick 0 -32767 3; rstick 20000 0 0.8; stick -20000 -20000 2; dead && break; done
dumpsys media.audio_flinger > $OUT/af_after.txt 2>&1
act end
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
exit 0
