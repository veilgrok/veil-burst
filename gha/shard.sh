#!/usr/bin/env bash
# Veil sharded job: calibrate, then WPR unmodified Veil workers pull chunks via the relay.
set -u
echo "VEIL_JSON {\"kind\":\"jobstart\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
[ "${CALIB_AFTER:-0}" = "1" ] || python3 gha/calib.py
export RELAY_RUNNER="r$GIDX"
python3 gha/relay_client.py > relay.log 2>&1 &
RP=$!; sleep 1; pids=()
for i in $(seq 1 $WPR); do
  python3 gha/worker.py --listen 127.0.0.1:$((27700+i)) --controller 127.0.0.1:7700 --name "r${GIDX}w${i}" --work-ms 0 & pids+=($!)
done
echo "VEIL_JSON {\"kind\":\"workers_up\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
( sleep 300; kill ${pids[@]} 2>/dev/null ) & KP=$!
wait ${pids[@]}; kill $KP 2>/dev/null
echo "VEIL_JSON {\"kind\":\"workers_done\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
kill $RP
[ "${CALIB_AFTER:-0}" = "1" ] && python3 gha/calib.py
tail -n 2 relay.log
