# Sourced helper: form runner groups from whatever GitHub scheduled, then bring up a WireGuard mesh inside the group.
# GitHub spreads one matrix over many Azure regions, so a fixed "K runners in one region" pick often fails. Instead:
#   1. every runner publishes region|vendor|cpu|nproc and waits for all NN;
#   2. runners in each region (MATCH=vendor: region+CPU vendor) are split into groups of up to KN, keeping groups of >= MINK;
#   3. if no same-region group forms anywhere and XREG is not 0, one cross-region group of up to KN runners is formed
#      (labelled xreg=1: RTT is tens of ms, so its numbers answer "does it work", not "does it scale");
#   4. runner 0 publishes the grouping and everyone follows it, so all runners agree.
# Inputs: PFX (key prefix, e.g. pl-$RUN), KN (wanted group size), MINK (smallest useful group, default 2), MATCH (region|vendor),
#         XREG (1: cross-region fallback, default 1), NN (matrix size), GIDX, K (kv function), MESH (1: bring up wg0, default 1).
# Outputs: G (index in group), KN (actual group size), GRP (group number), XR (1 if cross-region), GPFX (per-group key prefix),
#          MESHIP <i> (WireGuard address of group member i). Not picked: exit 0. No group anywhere: exit 1 with NOGROUP.
MINK=${MINK:-2}; MATCH=${MATCH:-region}; XREG=${XREG:-1}
MREG=$(curl -s -m 5 -H Metadata:true 'http://169.254.169.254/metadata/instance/compute/location?api-version=2021-02-01&format=text'); MREG=${MREG:-unknown}
MV=$(grep -qw vmx /proc/cpuinfo && echo intel || echo amd); MCPU=$(lscpu | sed -n 's/Model name: *//p' | head -1 | tr -cd 'A-Za-z0-9')
K put $PFX-reg-$GIDX "$MREG|$MV|$MCPU|$(nproc)" >/dev/null
MALL=$(K wait $PFX-reg- $NN 900) || MALL=$(K wait $PFX-reg- 1 5)
MSEL=$(echo "$MALL" | KN=$KN MINK=$MINK MATCH=$MATCH XREG=$XREG python3 -c "
import json,sys,os; kn=int(os.environ['KN']); mk=int(os.environ['MINK']); vend=os.environ['MATCH']=='vendor'
d={int(k.rsplit('-',1)[1]):v.lower().split('|') for k,v in json.load(sys.stdin).items()}; g={}
for i in sorted(d): g.setdefault((d[i][0],d[i][1]) if vend else d[i][0],[]).append(i)
gs=[]
for v in g.values():
  for s in range(0,len(v),kn):
    c=v[s:s+kn]
    if len(c)>=mk: gs.append(c)
gs.sort(key=lambda c:(-len(c),c[0])); xr=0
if not gs and os.environ['XREG']!='0' and len(d)>=mk: gs=[sorted(d)[:kn]]; xr=1
print(';'.join(','.join(map(str,c)) for c in gs) + '|%d' % xr if gs else 'none')")
if [ "$GIDX" = 0 ]; then K put $PFX-sel-0 "$MSEL" >/dev/null; else S0=$(K wait $PFX-sel- 1 180 2>/dev/null | python3 -c "import json,sys;print(list(json.load(sys.stdin).values())[0])" 2>/dev/null); [ -n "$S0" ] && MSEL=$S0; fi
echo "VEIL_JSON {\"kind\":\"group\",\"gidx\":$GIDX,\"region\":\"$MREG\",\"vendor\":\"$MV\",\"groups\":\"$MSEL\"}"
if [ "$MSEL" = none ]; then echo "NOGROUP: no $MINK runners in one region and no cross-region fallback; regions: $(echo $MALL | tr -d '{}')"; exit 1; fi
XR=${MSEL##*|}; GRP=-1; G=-1; gi=0
for grp in $(echo "${MSEL%|*}" | tr ';' ' '); do i=0; for m in $(echo $grp | tr , ' '); do [ "$m" = "$GIDX" ] && { GRP=$gi; G=$i; KN=$(echo $grp | tr , ' ' | wc -w); }; i=$((i+1)); done; gi=$((gi+1)); done
[ $GRP -lt 0 ] && { echo "not picked (groups: $MSEL)"; exit 0; }
GPFX=$PFX-g$GRP; MESHIP() { echo 10.88.$GRP.$(($1 + 1)); }
if [ "${MESH:-1}" = 1 ]; then
  mkdir -p /etc/wireguard; (umask 077; wg genkey > /etc/wireguard/k); MPUB=$(wg pubkey < /etc/wireguard/k)
  read MIP MPORT < <(K stun 51820); K put $GPFX-wg-$G "{\"ip\":\"$MIP\",\"port\":$MPORT,\"pub\":\"$MPUB\"}" >/dev/null
  MPEERS=$(K wait $GPFX-wg- $KN 900) || { echo "NOMESH: peers never arrived"; exit 1; }
  ip link add wg0 type wireguard; wg set wg0 listen-port 51820 private-key /etc/wireguard/k; ip addr add $(MESHIP $G)/24 dev wg0; ip link set wg0 mtu 1420 up
  echo "$MPEERS" | python3 -c "
import json,sys
for k,v in json.load(sys.stdin).items():
  i=int(k.rsplit('-',1)[1]); p=json.loads(v)
  if i!=$G: print(i,p['ip'],p['port'],p['pub'])" | while read i ip port pub; do wg set wg0 peer $pub endpoint $ip:$port allowed-ips $(MESHIP $i)/32 persistent-keepalive 1; done
  sleep 4; MBAD=0; for i in $(seq 0 $((KN-1))); do [ $i = $G ] && continue; ping -c 3 -W 2 $(MESHIP $i) >/dev/null 2>&1 || { sleep 5; ping -c 3 -W 2 $(MESHIP $i) >/dev/null 2>&1; } || MBAD=1; done
  K put $GPFX-mok-$G $MBAD >/dev/null; MOK=$(K wait $GPFX-mok- $KN 300) || MOK='{"x":"1"}'
  echo "$MOK" | grep -q '"1"' && { echo "NOMESH: a WireGuard link in group $GRP is down: $MOK"; exit 1; }
fi
