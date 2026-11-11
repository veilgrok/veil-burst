#!/usr/bin/env bash
# One matrix job: specs + CPU bench + batch share, then Veil workers via the relay.
set -u
echo "VEIL_JSON {\"kind\":\"jobstart\",\"t\":$(date +%s.%N),\"gidx\":$GIDX}"
python3 gha/bench.py
if [[ ",$PHASES," == *",veil,"* && -n "${VEIL_RELAY_URL:-}" ]]; then
  export RELAY_RUNNER="r$GIDX"
  python3 gha/relay_client.py > relay.log 2>&1 &
  RP=$!
  sleep 1
  N=$(nproc); pids=()
  for i in $(seq 1 $N); do
    WORKER_POOL="gha-${GITHUB_REPOSITORY_OWNER}" python3 gha/worker.py --listen 127.0.0.1:$((27700+i)) \
      --controller 127.0.0.1:7700 --name "r${GIDX}w${i}" --work-ms 0 & pids+=($!)
  done
  t0=$(date +%s.%N)
  ( sleep ${VEIL_TIMEOUT:-120}; kill ${pids[@]} 2>/dev/null ) & KP=$!
  wait ${pids[@]}
  kill $KP 2>/dev/null
  echo "VEIL_JSON {\"kind\":\"veil\",\"t\":$(date +%s.%N),\"t0\":$t0,\"gidx\":$GIDX,\"workers\":$N}"
  tail -n 3 relay.log
  kill $RP
fi
