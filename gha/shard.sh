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
# fault injection (FAULT is JSON keyed by gidx; no secrets)
python3 - <<'PY' > fault.env
import json, os
f = json.loads(os.environ.get("FAULT") or "{}").get(os.environ["GIDX"], {})
print(f"KILL_AT={f.get('kill_at','')}"); print(f"NETEM='{f.get('netem','')}'"); print(f"PROBE={f.get('probe','')}")
PY
. ./fault.env
[ "$PROBE" = "1" ] && python3 gha/probe.py
if [ -n "$NETEM" ]; then DEV=$(ip route show default | awk '{print $5; exit}'); sudo tc qdisc add dev $DEV root netem $NETEM && echo "VEIL_JSON {\"kind\":\"netem\",\"gidx\":$GIDX,\"dev\":\"$DEV\",\"t\":$(date +%s.%N)}"; fi
if [ -n "$KILL_AT" ]; then ( sleep $KILL_AT; echo "VEIL_JSON {\"kind\":\"killed\",\"gidx\":$GIDX,\"t\":$(date +%s.%N)}"; kill -9 ${pids[@]} $RP 2>/dev/null ) & fi
echo "VEIL_JSON {\"kind\":\"workers_up\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
( sleep 300; kill ${pids[@]} 2>/dev/null ) & KP=$!
wait ${pids[@]}; kill $KP 2>/dev/null
echo "VEIL_JSON {\"kind\":\"workers_done\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
kill $RP
[ "${CALIB_AFTER:-0}" = "1" ] && python3 gha/calib.py
tail -n 2 relay.log
