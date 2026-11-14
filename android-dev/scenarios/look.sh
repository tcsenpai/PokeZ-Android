# look.sh: from the installed save: start, continue, then stand ${IDLE:-40} s (in-app frames via env), slow camera
# pans, ${PLAY:-30} s walking. For image comparisons (shadows, faces): use LARECOMP_SHOT_AT / SCREENSHOT_EVERY.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-56} ${COOLMAX:-40}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
log "idle"
sleep ${IDLE:-40}
log "pan"
for k in 1 2 3; do rstick 12000 0 2; sleep 4; dead && break; done
log "walk"
E=$(( $(el) + ${PLAY:-30} ))
while [ $(el) -lt $E ]; do stick 0 -32767 3; rstick 20000 0 0.8; dead && break; done
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
exit 0
