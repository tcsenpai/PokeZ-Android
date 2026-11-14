# speed.sh: the game's speed against the wall clock. Start,
# title, continue, 8 s after spawn the right stick held at ${RS:-16000} for ${ROT:-3} s (the camera turns at the game's
# speed: the angle it reached says how much game time passed), then still. In-app shots (LARECOMP_SHOT_AT) after the
# turn show the final view; compare runs side by side (analysis/montage.py).
. /data/local/tmp/pzx/lib.sh
logu() { echo "$(el) $(cut -d' ' -f1 /proc/uptime) $*" >> $OUT/events.txt; }
cooldown_xo ${COOLXO:-55} ${COOLMAX:-60}
logu "start $(temps)"
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; logu tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 8
logu turn; rstick ${RS:-16000} 0 ${ROT:-3}; logu turn-end
sleep ${AFTER:-12}
logu "end $(temps) app $(pidof_app)"
exit 0
