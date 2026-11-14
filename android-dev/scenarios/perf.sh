# perf.sh: from the installed save: start, title, continue, 8 s after spawn a 10 s simpleperf profile of the whole app (all
# threads, symbols per thread), reported on the device into out/perf-*.txt.
. /data/local/tmp/pzx/lib.sh
log "start $(temps)"
tsampler 5 &
SP=$!
am start -n $PKG/.ZARecompActivity >/dev/null
sleep 6; input tap 200 525; log tap-play
sleep 20
for i in 1 2 3 4 5 6; do btn $BA; sleep 4; done
sleep 8; log "perf"
simpleperf record --app $PKG --duration 10 -f 1000 -o $OUT/perf.data > $OUT/perf-rec.txt 2>&1
log "perf done app $(pidof_app)"
simpleperf report -i $OUT/perf.data --sort comm -n > $OUT/perf-comm.txt 2>&1
simpleperf report -i $OUT/perf.data --sort comm,dso -n > $OUT/perf-dso.txt 2>&1
simpleperf report -i $OUT/perf.data --sort comm,symbol -n --comms za-render,za-render2,za-game > $OUT/perf-sym.txt 2>&1
rm -f $OUT/perf.data
kill $SP 2>/dev/null
exit 0
