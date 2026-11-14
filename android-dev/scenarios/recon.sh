# recon.sh: from the installed save: start, title, continue, then look around: walk, run, camera turns, menu
# open/close; samples every 5 s. Screenshots: in-app (env); SCREENCAP=1: screenrecord + screencap.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-55} ${COOLMAX:-60}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
( rec rec1; rec rec2 ) &
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 20
log "walk"
stick 0 -32767 4; rstick 32767 0 2; stick 0 -32767 4; rstick -32767 0 2
log "run"
sendevent $EV 1 $BB 1; syn; stick 0 -32767 4; sendevent $EV 1 $BB 0; syn
rstick 32767 0 3; stick 32767 0 3; stick 0 32767 3; rstick 0 20000 1; rstick 0 -20000 1
log "menu"
btn $BX; sleep 3; shot menu; btn 545; sleep 1; btn 545; sleep 1; shot menu2; btn $BB; sleep 3; btn $BB; sleep 2
log "map"
btn 315; sleep 3; shot map; btn $BB; sleep 3; btn $BB; sleep 2
log "walk2"
for k in 1 2 3 4; do stick 0 -32767 3; rstick 25000 0 1; stick 20000 -20000 2; done
sleep 5
log "end $(temps)"
kill $SP 2>/dev/null
rec_stop
