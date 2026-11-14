# texmod.sh: from the installed save: start, continue, then still camera holds for before/after
# texture comparisons (in-app frames via env: LARECOMP_SCREENSHOT_EVERY + _FROM; a hold's frames are the same picture):
# hold 1 at the spawn view, hold 2 camera tilted up (the buildings over the canal), hold 3 turned right, hold 4 turned
# further right. Holds ${HOLD:-15} s each.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-56} ${COOLMAX:-40}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 15
log "hold1"; sleep ${HOLD:-15}
rstick 0 ${TILTY:-20000} ${TILT:-1.0}; sleep 2
log "hold2"; sleep ${HOLD:-15}
rstick 20000 0 1.2; sleep 2
log "hold3"; sleep ${HOLD:-15}
rstick 20000 0 1.2; sleep 2
log "hold4"; sleep ${HOLD:-15}
if [ ${WALK:-0} -gt 0 ]; then  # (look.sh's walk: forward + camera right, ${WALK} s)
    log "walk"; E=$(( $(el) + WALK ))
    while [ $(el) -lt $E ]; do stick 0 -32767 3; rstick 20000 0 0.8; sleep 1; dead && break; done
fi
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
exit 0
