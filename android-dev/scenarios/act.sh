# act.sh: from the installed save: real gameplay actions, each logged as "ACT <name>" in events.txt (seconds since
# start; lasrecomp.log [slow]/[hitch]/[pace] lines carry the frame times). In-app shots: LARECOMP_SCREENSHOT_EVERY/FROM.
. /data/local/tmp/pzx/lib.sh
cooldown_xo ${COOLXO:-56} ${COOLMAX:-40}
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 8
act spawned; sleep 5
act camera-turn; rstick 32767 0 3; rstick -32767 0 3
act run; sendevent $EV 1 $BB 1; syn; stick 0 -32767 4; sendevent $EV 1 $BB 0; syn; dead && exit 0
act lock-on-zl; trig zl 1.5
act send-out-zl-zr; sendevent $EV 3 10 32767; sendevent $EV 1 312 1; syn; sleep 0.8; trig zr 0.3; sleep 2; sendevent $EV 3 10 0; sendevent $EV 1 312 0; syn; sleep 3
act zr-alone; trig zr 0.3; sleep 3
act r-button; btn 311; sleep 3
act l-button; btn 310; sleep 3
dead && exit 0
act interact-A; for k in 1 2 3 4; do btn $BA; sleep 2; done
act cancel-B; btn $BB; sleep 1; btn $BB; sleep 2
act menu-X; btn $BX; sleep 3; btn 546; sleep 1; btn 547; sleep 1; btn $BA; sleep 3; btn $BB; sleep 2; btn $BB; sleep 2
act map-plus; btn 315; sleep 4; btn $BB; sleep 3
act bag-minus; btn 314; sleep 3; btn $BB; sleep 3
dead && exit 0
act walk-explore; for k in 1 2 3; do stick 0 -32767 3; rstick 25000 0 1; stick 25000 -15000 2; btn $BA; sleep 1; dead && break; done
act battle-attempt; trig zl 0.3; sendevent $EV 3 10 32767; sendevent $EV 1 312 1; syn; sleep 0.5; btn $BA; sleep 1; btn $BX; sleep 1; btn $BY; sleep 1; sendevent $EV 3 10 0; sendevent $EV 1 312 0; syn; sleep 5
act cancel; btn $BB; sleep 2; btn $BB; sleep 2
act end
log "end $(temps) app $(pidof_app)"
kill $SP 2>/dev/null
exit 0
