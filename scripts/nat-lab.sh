#!/usr/bin/env bash
# nat-lab.sh — isolated UDP NAT laboratory for WUP ThinServer.
#
# Topology (all inside private network namespaces, host firewall untouched):
#
#   fake_pad (ns-cli 10.77.1.2) -- veth -- ns-nat (masquerade+filter)
#                                 -- veth -- lab-server (ns-srv 10.77.2.2)
#
# Every run uses a unique resource suffix; only resources created by this
# invocation are touched. The host network and firewall are never modified.
#
# Requires: root, iproute2, nftables, python3, a C compiler.
# Usage:   sudo scripts/nat-lab.sh [--keep] [--only TEST ...]
# Tests:   endpoint custom-port session strict-filter isolation multi expiry
set -euo pipefail

P="w${RANDOM:0:4}x${RANDOM:0:3}"   # unique per-run prefix (ifnames <=15 chars)
CLI_IP=10.77.1.2; NAT_C=10.77.1.1; NAT_S=10.77.2.1; SRV_IP=10.77.2.2
PORT=50001; CPORT=5080
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABBIN="$ROOT/build/nat-lab/$P"
SRVBIN="$LABBIN/lab-server"
PADPY="$ROOT/tools/nat-lab/fake_pad.py"
ART="$LABBIN"; mkdir -p "$ART"
KEEP=0; ONLY=""
while [ $# -gt 0 ]; do case "$1" in
    --keep) KEEP=1;; --only) shift; ONLY="$1";; *) echo "unknown arg $1" >&2; exit 2;;
esac; shift; done

SRV_PID=""
log() { printf '\033[1;36m[nat-lab %s]\033[0m %s\n' "$P" "$*"; }
die() { printf '\033[1;31m[nat-lab %s] FAIL:\033[0m %s\n' "$P" "$*" >&2; exit 1; }

# ---------- build -------------------------------------------------------------
build_lab() {
    local dep="$ROOT/build/_wupstream_source/build/host"
    [ -f "$dep/libwupstream.a" ] || { echo "run 'meson compile -C build' first" >&2; exit 1; }
    cc -O2 -Wall -I"$ROOT/build/_wupstream_source/host" -I"$dep" -I"$dep/x264" \
        "$ROOT/tools/nat-lab/lab-server.c" "$dep/libwupstream.a" \
        -lm -lpthread -ldl -o "$SRVBIN"
}

# ---------- namespace topology -------------------------------------------------
cleanup() {
    [ "$KEEP" = 1 ] && { log "keeping namespaces (--keep)"; return; }
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    # only the namespaces/veth pairs this run created (names carry $P)
    for n in "$P-cli" "$P-nat" "$P-srv"; do ip netns del "$n" 2>/dev/null || true; done
}
trap cleanup EXIT

setup_ns() {
    command -v nft >/dev/null || die "nftables required"
    ip netns add "$P-cli"; ip netns add "$P-nat"; ip netns add "$P-srv"
    # client <-> nat
    ip link add "v-$P-c" type veth peer name "v-$P-cn"
    ip link set "v-$P-c" netns "$P-cli"
    ip link set "v-$P-cn" netns "$P-nat"
    ip netns exec "$P-cli" ip addr add "$CLI_IP/24" dev "v-$P-c"
    ip netns exec "$P-cli" ip link set "v-$P-c" up
    ip netns exec "$P-nat" ip addr add "$NAT_C/24" dev "v-$P-cn"
    ip netns exec "$P-nat" ip link set "v-$P-cn" up
    # nat <-> server
    ip link add "v-$P-s" type veth peer name "v-$P-sn"
    ip link set "v-$P-s" netns "$P-nat"
    ip link set "v-$P-sn" netns "$P-srv"
    ip netns exec "$P-nat" ip addr add "$NAT_S/24" dev "v-$P-s"
    ip netns exec "$P-nat" ip link set "v-$P-s" up
    ip netns exec "$P-srv" ip addr add "$SRV_IP/24" dev "v-$P-sn"
    ip netns exec "$P-srv" ip link set "v-$P-sn" up
    for n in "$P-cli" "$P-nat" "$P-srv"; do ip netns exec "$n" ip link set lo up; done
    ip netns exec "$P-cli" ip route add default via "$NAT_C"
    ip netns exec "$P-srv" ip route add default via "$NAT_S"
    ip netns exec "$P-nat" sysctl -qw net.ipv4.ip_forward=1
    ip netns exec "$P-nat" nft -f - <<EOF
table ip $P {
  chain postrouting { type nat hook postrouting priority 100;
    ip saddr 10.77.1.0/24 oifname "v-$P-s" masquerade; }
  chain forward { type filter hook forward priority 0;
    ct state established,related accept;
    ip saddr 10.77.1.0/24 ip daddr 10.77.2.0/24 udp dport { $PORT, $CPORT } accept;
    ip saddr 10.77.1.0/24 ip daddr 10.77.2.0/24 udp sport { $PORT, $CPORT } accept;
    drop; }
}
EOF
    ip netns exec "$P-cli" nft -f - <<EOF
table ip ${P}cli {
  chain input { type filter hook input priority 0; ct state established,related accept; ip protocol udp accept; drop; }
  chain output { type filter hook output priority 0; ip protocol udp accept; drop; }
}
EOF
}

# strict endpoint filter in the client namespace: only accept UDP replies
# whose source ip:port equals what we dialed (address-and-port-dependent
# filtering, the strictest common NAT behavior).
set_strict_filter() {  # $1 = required server port
    ip netns exec "$P-cli" nft -f - <<EOF
table ip ${P}cli {
  chain input { type filter hook input priority 0;
    ip saddr $SRV_IP udp sport $1 accept;
    ip protocol udp drop; }
}
EOF
}
clear_strict_filter() {
    ip netns exec "$P-cli" nft delete table ip ${P}cli
    ip netns exec "$P-cli" nft -f - <<EOF
table ip ${P}cli {
  chain input { type filter hook input priority 0; ct state established,related accept; ip protocol udp accept; drop; }
  chain output { type filter hook output priority 0; ip protocol udp accept; drop; }
}
EOF
}

# ---------- helpers -------------------------------------------------------------
start_server() {  # $1 port, $2 logfile — only one lab session at a time
    [ -n "$SRV_PID" ] && kill "$SRV_PID" 2>/dev/null || true
    sleep 0.3
    ip netns exec "$P-srv" "$SRVBIN" "$1" >"$2" 2>&1 &
    SRV_PID=$!
    sleep 0.4
    kill -0 "$SRV_PID" 2>/dev/null || { cat "$2" >&2; die "lab-server exited"; }
}

# run_pad ARGS... — runs fake_pad in the client namespace, propagates its
# real exit status, stores output in $PAD_OUT. Fails the test on nonzero
# exit or a missing/failed RESULT line.
run_pad() {
    set +e
    PAD_OUT="$(ip netns exec "$P-cli" python3 "$PADPY" --server "$SRV_IP" "$@" 2>&1)"
    PAD_RC=$?
    set -e
    [ "$PAD_RC" = 0 ] || { echo "$PAD_OUT" >&2; return "$PAD_RC"; }
    echo "$PAD_OUT" | grep -q '^RESULT ok=1' || { echo "$PAD_OUT" >&2; return 1; }
    return 0
}

want() {
    [ -z "$ONLY" ] && return 0
    case " $ONLY " in *" $1 "*) return 0;; *) return 1;; esac
}
pass() { log "PASS $1"; }
fail() { die "FAIL $1"; }

# ---------- tests -----------------------------------------------------------------
t_endpoint() {
    log "test endpoint: B -> H must come from the dialed port $PORT"
    start_server "$PORT" "$ART/srv-endpoint.log"
    run_pad --port "$PORT" --seconds 1.2 --probe --peer-port "$PORT" || fail endpoint
    pass endpoint
}

t_custom_port() {
    log "test custom-port: session on UDP $CPORT"
    start_server "$CPORT" "$ART/srv-custom.log"
    run_pad --port "$CPORT" --seconds 1.2 --probe --peer-port "$CPORT" || fail custom-port
    pass custom-port
}

t_session() {
    log "test session: full B/H/I/V/A + T + N through NAT"
    start_server "$PORT" "$ART/srv-session.log"
    run_pad --port "$PORT" --seconds 4 --send-t --send-n || fail session
    local line; line="$(echo "$PAD_OUT" | grep '^RESULT')"
    echo "$line" | grep -q 't_acks=[1-9]' || fail "no T ack ($line)"
    pass session
}

t_strict() {
    log "test strict-filter: address-and-port-dependent filtering, port $PORT"
    start_server "$PORT" "$ART/srv-strict.log"
    set_strict_filter "$PORT"
    run_pad --port "$PORT" --seconds 2 --peer-port "$PORT" || fail strict-filter
    clear_strict_filter
    pass strict-filter
}

t_isolation() {
    log "test isolation: client B packets must not disturb client A's session"
    start_server "$PORT" "$ART/srv-isolation.log"
    # A: full session for the whole window. B: valid beacons+reports from
    # another port (same NAT IP) — must be ignored while A is healthy.
    set +e
    ip netns exec "$P-cli" python3 "$PADPY" --server "$SRV_IP" --port "$PORT" \
        --sport 50000 --seconds 5 > "$ART/pad-a.log" 2>&1 &
    PA=$!
    sleep 1.0   # let A's session establish
    ip netns exec "$P-cli" python3 "$PADPY" --server "$SRV_IP" --port "$PORT" \
        --sport 50010 --seconds 3 --beacons 6 --spray > "$ART/pad-b.log" 2>&1 &
    PB=$!
    wait $PA; RA=$?; wait $PB; RB=$?
    set -e
    grep '^RESULT' "$ART/pad-a.log" || die "client A produced no RESULT"
    grep '^RESULT' "$ART/pad-b.log" || die "client B produced no RESULT"
    # A kept streaming through B's interference window.
    grep '^RESULT ok=1' "$ART/pad-a.log" || { cat "$ART/pad-a.log" >&2; fail "client A interrupted"; }
    # B never got the session while A was active.
    grep '^RESULT ok=0' "$ART/pad-b.log" || { cat "$ART/pad-b.log" >&2; fail "client B stole the session"; }
    # After A leaves, B can establish its own session.
    run_pad --port "$PORT" --sport 50010 --seconds 2.5 || fail "client B reconnect"
    pass isolation
}

t_multi() {
    log "test multi-client: two pads share the NAT external address (turn-taking)"
    start_server "$PORT" "$ART/srv-multi.log"
    run_pad --port "$PORT" --sport 50000 --seconds 2.5 || fail "client1"
    run_pad --port "$PORT" --sport 50010 --seconds 2.5 || fail "client2"
    pass multi-client
}

# ---------- regression: failing client must fail the lab -------------------------
t_regression() {
    log "test regression: a failing fake client must fail run_pad"
    set +e
    PAD_OUT="$(ip netns exec "$P-cli" python3 "$PADPY" --server "$SRV_IP" \
        --port 59999 --seconds 0.2 --beacons 2 2>&1)"; PAD_RC=$?
    set -e
    [ "$PAD_RC" != 0 ] || fail "unreachable server unexpectedly succeeded"
    pass regression
}

# ---------- main -------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "must run as root (uses network namespaces)"
build_lab
setup_ns

run_test() {
    case "$1" in
        endpoint) t_endpoint;;
        custom-port) t_custom_port;;
        session) t_session;;
        strict-filter) t_strict;;
        isolation) t_isolation;;
        multi) t_multi;;
        regression) t_regression;;
    esac
}
for t in endpoint custom-port session strict-filter isolation multi regression; do
    want "$t" || continue
    run_test "$t"
done
log "all selected tests passed (artifacts: $ART)"
