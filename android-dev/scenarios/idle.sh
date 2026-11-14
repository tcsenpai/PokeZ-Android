# idle.sh: from the installed save: start, title, continue, then ${IDLE:-60} s standing (camera turns only), then
# ${PLAY:-60} s walking. Screenshots: in-app (env LARECOMP_SCREENSHOT_EVERY); SCREENCAP=1: adb screenrecord too.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-55} ${COOLMAX:-45}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
rec rec1 &
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
log "idle"
E=$(( $(el) + ${IDLE:-60} ))
while [ $(el) -lt $E ]; do rstick 20000 0 1.5; sleep 3; rstick -20000 0 1.5; sleep 3; dead && break; done
log "walk"
E=$(( $(el) + ${PLAY:-60} ))
while [ $(el) -lt $E ]; do stick 0 -32767 3; rstick 20000 0 0.8; stick 20000 -20000 2; rstick -20000 0 0.6; dead && break; done
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
rec_stop
exit 0
