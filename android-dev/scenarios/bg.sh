# bg.sh: the background pause. Start, continue, ${PLAY:-20} s of
# street walk, HOME for ${BG:-120} s (the app's CPU time over it from /proc/<pid>/stat, plus top), back with am start,
# ${PLAY:-20} s more of walking. In-app shots (LARECOMP_SCREENSHOT_EVERY) before and after.
. /data/local/tmp/pzx/lib.sh
logu() { echo "$(el) $(cut -d' ' -f1 /proc/uptime) $*" >> $OUT/events.txt; }
cpu() { p=$(pidof_app); [ -n "$p" ] && awk '{print $14+$15}' /proc/$p/stat; }
walk() { E=$(( $(el) + $1 )); while [ $(el) -lt $E ]; do stick 0 -32767 2; rstick 20000 0 0.8; stick 20000 -20000 2; dead && break; done; }
cooldown_xo ${COOLXO:-56} ${COOLMAX:-20}
logu "start $(temps)"
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; logu tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 6
logu walk1; walk ${PLAY:-20}
c0=$(cpu); logu "cpu ticks before $c0"
top -b -n 1 -p $(pidof_app) | tail -2 >> $OUT/top-fg.txt
[ -n "$NOHOME" ] || input keyevent KEYCODE_HOME; logu home
sleep 10; c1=$(cpu); logu "cpu ticks 10 s into the background $c1"
sleep $(( ${BG:-120} - 10 ))
c2=$(cpu); logu "cpu ticks at the end of the background $c2 (ticks over the last $(( ${BG:-120} - 10 )) s: $((c2 - c1)))"
top -b -n 1 -p $(pidof_app) | tail -2 >> $OUT/top-bg.txt
dumpsys cpuinfo | grep -i recomp > $OUT/cpuinfo-bg.txt
[ -n "$NOHOME" ] || am start -n $PKG/.ZARecompActivity >/dev/null; logu back
sleep 8
logu walk2; walk ${PLAY:-20}
c3=$(cpu); logu "cpu ticks after return $c3"
logu "end $(temps) app $(pidof_app)"
exit 0
