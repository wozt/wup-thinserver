#!/usr/bin/env bash
# nat-lab.sh — isolated UDP NAT laboratory for WUP ThinServer.
#
# Topology (all inside private network namespaces, host firewall untouched):
#
#   fake_pad (nl-cli 10.77.1.2) -- veth -- nl-nat (masquerade+filter)
#                                -- veth -- lab-server (nl-srv 10.77.2.2)
#
# The client always dials the SERVER-side address:port; the NAT rewrites the
# source. A strict client-side input filter can emulate address-and-port-
# dependent filtering (replies must come from the exact ip:port dialed).
#
# Requires: root, iproute2, nftables, python3, a C compiler.
# Usage:   sudo scripts/nat-lab.sh [--keep] [--only TEST ...]
# Tests:   endpoint, custom-port, session, strict-filter, multi-client
set -euo pipefail

P=wl                          # namespace/veth prefix
CLI_IP=10.77.1.2; NAT_C=10.77.1.1; NAT_S=10.77.2.1; SRV_IP=10.77.2.2
PORT=50001; CPORT=5080
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABBIN="$ROOT/build/nat-lab"
SRVBIN="$LABBIN/lab-server"
PADPY="$ROOT/tools/nat-lab/fake_pad.py"
KEEP=0; ONLY=""
while [ $# -gt 0 ]; do case "$1" in
    --keep) KEEP=1;; --only) shift; ONLY="$1";; *) echo "unknown arg $1" >&2; exit 2;;
esac; shift; done

log() { printf '\033[1;36m[nat-lab]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[nat-lab] FAIL:\033[0m %s\n' "$*" >&2; exit 1; }

# ---------- build ------------------------------------------------------------
build_lab() {
    mkdir -p "$LABBIN"
    local dep="$ROOT/build/_wupstream_source/build/host"
    [ -f "$dep/libwupstream.a" ] || { echo "run 'meson compile -C build' first" >&2; exit 1; }
    cc -O2 -Wall -I"$ROOT/build/_wupstream_source/host" -I"$dep" -I"$dep/x264" \
        "$ROOT/tools/nat-lab/lab-server.c" "$dep/libwupstream.a" \
        -lm -lpthread -ldl -o "$SRVBIN"
}

# ---------- namespace topology -------------------------------------------------
cleanup() {
    for p in $(pgrep -f "$LABBIN/lab-server|fake_pad.py" 2>/dev/null); do
        kill "$p" 2>/dev/null || true
    done
    ip netns del "$P-cli" 2>/dev/null || true
    ip netns del "$P-nat" 2>/dev/null || true
    ip netns del "$P-srv" 2>/dev/null || true
    ip link del "v-${P}c" 2>/dev/null || true
    ip link del "v-${P}s" 2>/dev/null || true
    [ "$KEEP" = 1 ] && log "namespaces kept (--keep)" || true
}
trap cleanup EXIT

setup_ns() {
    command -v nft >/dev/null || die "nftables required"
    for n in "$P-cli" "$P-nat" "$P-srv"; do ip netns del "$n" 2>/dev/null || true; done
    ip netns add "$P-cli"; ip netns add "$P-nat"; ip netns add "$P-srv"
    # client <-> nat
    ip link add "v-${P}c" type veth peer name "v-${P}cn"
    ip link set "v-${P}c" netns "$P-cli"
    ip link set "v-${P}cn" netns "$P-nat"
    ip netns exec "$P-cli" ip addr add "$CLI_IP/24" dev "v-${P}c"
    ip netns exec "$P-cli" ip link set "v-${P}c" up
    ip netns exec "$P-nat" ip addr add "$NAT_C/24" dev "v-${P}cn"
    ip netns exec "$P-nat" ip link set "v-${P}cn" up
    # nat <-> server
    ip link add "v-${P}s" type veth peer name "v-${P}sn"
    ip link set "v-${P}s" netns "$P-nat"
    ip link set "v-${P}sn" netns "$P-srv"
    ip netns exec "$P-nat" ip addr add "$NAT_S/24" dev "v-${P}s"
    ip netns exec "$P-nat" ip link set "v-${P}s" up
    ip netns exec "$P-srv" ip addr add "$SRV_IP/24" dev "v-${P}sn"
    ip netns exec "$P-srv" ip link set "v-${P}sn" up
    # loopback + routes
    for n in "$P-cli" "$P-nat" "$P-srv"; do ip netns exec "$n" ip link set lo up; done
    ip netns exec "$P-cli" ip route add default via "$NAT_C"
    ip netns exec "$P-srv" ip route add default via "$NAT_S"
    ip netns exec "$P-nat" sysctl -qw net.ipv4.ip_forward=1
    # NAT: masquerade client subnet out the server-facing veth; forward only
    # udp toward the lab ports (everything else dropped = isolated lab)
    ip netns exec "$P-nat" nft -f - <<EOF
table ip $P {
  chain postrouting { type nat hook postrouting priority 100;
    ip saddr 10.77.1.0/24 oifname "v-${P}s" masquerade; }
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
    ip netns exec "$P-cli" nft flush chain ip ${P}cli input 2>/dev/null || true
    ip netns exec "$P-cli" nft -f - <<EOF
table ip ${P}cli {
  chain input { type filter hook input priority 0;
    ct state established,related accept;
    ip saddr $SRV_IP udp sport $1 accept;
    ip protocol udp drop; }
}
EOF
}
clear_strict_filter() {
    ip netns exec "$P-cli" nft flush chain ip ${P}cli input 2>/dev/null || true
    ip netns exec "$P-cli" nft add rule ip ${P}cli input ct state established,related accept
    ip netns exec "$P-cli" nft add rule ip ${P}cli input ip protocol udp accept
}

# ---------- helpers -------------------------------------------------------------
start_server() {  # $1 port, $2 logfile
    pkill -f "$SRVBIN" 2>/dev/null || true   # one session at a time: stale hellos would pollute results
    sleep 0.3
    ip netns exec "$P-srv" "$SRVBIN" "$1" >"$2" 2>&1 &
    SRV_PID=$!
    sleep 0.4
    kill -0 "$SRV_PID" 2>/dev/null || { cat "$2" >&2; die "lab-server exited"; }
}
run_pad() {  # extra fake_pad args...; sets PAD_RC and PAD_OUT
    set +e
    PAD_OUT="$(ip netns exec "$P-cli" python3 "$PADPY" --server "$SRV_IP" "$@" 2>&1)"
    PAD_RC=$?
    set -e
    echo "$PAD_OUT" | grep '^RESULT' || { echo "$PAD_OUT" >&2; die "fake_pad produced no RESULT"; }
}
want() {  # TEST filtering
    [ -z "$ONLY" ] && return 0
    case " $ONLY " in *" $1 "*) return 0;; *) return 1;; esac
}

pass() { log "PASS $1"; }
fail() { die "FAIL $1"; }

# ---------- tests -----------------------------------------------------------------
t_endpoint() {
    log "test endpoint: B -> H must come from the dialed port $PORT"
    start_server "$PORT" /tmp/nat-lab-srv1.log
    run_pad --port "$PORT" --seconds 1.2 --peer-port "$PORT" || fail endpoint
    pass endpoint
}

t_custom_port() {
    log "test custom-port: session on UDP $CPORT"
    start_server "$CPORT" /tmp/nat-lab-srv2.log
    run_pad --port "$CPORT" --seconds 1.2 --peer-port "$CPORT" || fail custom-port
    pass custom-port
}

t_session() {
    log "test session: full B/H/I/V/A + T + N through NAT"
    start_server "$PORT" /tmp/nat-lab-srv3.log
    run_pad --port "$PORT" --seconds 4 --send-t --send-n \
        || fail session
    local line; line="$(echo "$PAD_OUT" | grep '^RESULT')"
    echo "$line" | grep -q 'videos=[1-9]' || fail "no video ($line)"
    echo "$line" | grep -q 'audios=[1-9]' || fail "no audio ($line)"
    echo "$line" | grep -q 't_acks=[1-9]' || fail "no T ack ($line)"
    pass session
}

t_strict() {
    log "test strict-filter: address-and-port-dependent filtering, port $PORT"
    start_server "$PORT" /tmp/nat-lab-srv4.log
    set_strict_filter "$PORT"
    run_pad --port "$PORT" --seconds 2 --peer-port "$PORT" || fail strict-filter
    clear_strict_filter
    pass strict-filter
}

t_multi_client() {
    log "test multi-client: two pads share the NAT external address (turn-taking)"
    start_server "$PORT" /tmp/nat-lab-srv5.log
    # The daemon serves one GamePad session at a time. Two clients behind the
    # same NAT external IP must each get a clean session in turn — a second
    # client's endpoint must never silently hijack the first's stream.
    run_pad --port "$PORT" --sport 50000 --seconds 2.5 || fail "client1"
    run_pad --port "$PORT" --sport 50010 --seconds 2.5 || fail "client2"
    pass multi-client
}

# ---------- main -------------------------------------------------------------------
[ "$(id -u)" = 0 ] || die "must run as root (uses network namespaces)"
build_lab
setup_ns

run_test() {  # $1 = test key
    case "$1" in
        endpoint) t_endpoint;;
        custom-port) t_custom_port;;
        session) t_session;;
        strict-filter) t_strict;;
        multi) t_multi_client;;
    esac
}
for t in endpoint custom-port session strict-filter multi; do
    want "$t" || continue
    run_test "$t"
done
log "all selected tests passed"
