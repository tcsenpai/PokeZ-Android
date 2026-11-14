# intro.sh: new save (device/run.sh with SAVE_IN=new): language, appearance, name, then the intro cutscenes (tower, train,
# first street: character close-ups) with A presses. Screenshots: in-app (env); SCREENCAP=1: screenrecord + screencap.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-55} ${COOLMAX:-60}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
( rec rec1; rec rec2 ) &
sleep 24; shot lang
btn $BA; sleep 6; shot appearance
stick 0 -32767 0.5; sleep 1; stick -20000 0 0.4; sleep 1
btn $BA; sleep 3; shot picked
btn $BA; sleep 0.6; btn $BA; sleep 0.6; btn $BA; sleep 1; shot named
btn 315; sleep 3; shot confirm
btn 545; sleep 1
btn 545; sleep 1
btn $BA; sleep 3; shot ready
for i in $(seq 1 ${NA:-55}); do btn $BA; sleep 4; dead && break; done
log "walk"
for k in 1 2 3 4 5; do stick 0 -32767 3; rstick 20000 0 0.8; stick 20000 -20000 2; rstick -20000 0 0.6; dead && break; done
log "end $(temps)"
kill $SP 2>/dev/null
rec_stop
