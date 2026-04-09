#!/usr/bin/env bash
# Mix simulation with LEZ-backed RLN spam protection.
# Combines the mix sim (4 logoscore nodes + chat2mix) with LEZ infrastructure
# (LSSA sequencer, on-chain RLN registration, LEZ root/proof polling).
#
# Requires logos-lez-rln repo as a sibling or parent for:
#   - LSSA sequencer (lssa/)
#   - lez-rln programs (lez-rln/)
#   - RLN module (logos-rln-module/result-rln/)
#   - Wallet module (logos-rln-module/result-wallet/)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DELIVERY_MODULE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DELIVERY_DIR="$DELIVERY_MODULE_DIR/vendor/logos-delivery"

# Auto-detect logos-lez-rln repo
LEZ_RLN_DIR="${LEZ_RLN_DIR:-}"
for candidate in "$DELIVERY_MODULE_DIR/.." "$DELIVERY_MODULE_DIR/../logos-lez-rln" "$HOME/Waku/Logos/rln-zkvm"; do
    [ -d "$candidate/lez-rln" ] && [ -d "$candidate/lssa" ] && LEZ_RLN_DIR="$(cd "$candidate" && pwd)" && break
done
[ -z "$LEZ_RLN_DIR" ] && { echo "FATAL: Cannot find logos-lez-rln repo. Set LEZ_RLN_DIR."; exit 1; }

export RISC0_DEV_MODE=1
export TMPDIR=/tmp

die() { echo "  FATAL: $*" >&2; exit 1; }
log() { echo "[$(date '+%H:%M:%S')] $*"; }

# --- Node identity constants (4 mix nodes) ---
NODEKEYS=(
    "f98e3fba96c32e8d1967d460f1b79457380e1a895f7971cecc8528abe733781a"
    "09e9d134331953357bd38bbfce8edb377f4b6308b4f3bfbe85c610497053d684"
    "ed54db994682e857d77cd6fb81be697382dc43aa5cd78e16b0ec8098549f860e"
    "42f96f29f2d6670938b0864aced65a332dcf5774103b4c44ec4d0ea4ef3c47d6"
)
PEER_IDS=(
    "16Uiu2HAmPiEs2ozjjJF2iN2Pe2FYeMC9w4caRHKYdLdAfjgbWM6o"
    "16Uiu2HAmLtKaFaSWDohToWhWUZFLtqzYZGPFuXwKrojFVF6az5UF"
    "16Uiu2HAmTEDHwAziWUSz6ZE23h5vxG2o4Nn7GazhMor4bVuMXTrA"
    "16Uiu2HAmPwRKZajXtfb1Qsv45VVfRZgK3ENdfmnqzSrVm3BczF6f"
)
MIXKEYS=(
    "c86029e02c05a7e25182974b519d0d52fcbafeca6fe191fbb64857fb05be1a53"
    "b858ac16bbb551c4b2973313b1c8c8f7ea469fca03f1608d200bbf58d388ec7f"
    "d8bd379bb394b0f22dd236d63af9f1a9bc45266beffc3fbbe19e8b6575f2535b"
    "780fff09e51e98df574e266bf3266ec6a3a1ddfcf7da826a349a29c137009d49"
)
NUM_NODES=4
BASE_TCP_PORT=60001
BASE_DISC_PORT=9001
CLUSTER_ID=2
NUM_SHARDS=1
CONTENT_TOPIC="/chat2mix/2/0/proto"
TEST_MESSAGE_PREFIX="mixleztestmsg"
NUM_TEST_MESSAGES=5

CHAT_RECEIVER_NODEKEY="cb6fe589db0e5d5b48f7e82d33093e4d9d35456f4aaffc2322c473a173b2ac49"
CHAT_SENDER_NODEKEY="35eace7ccb246f20c487e05015ca77273d8ecaed0ed683de3d39bf4f69336feb"

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLATFORM="darwin-arm64-dev"; EXT="dylib";;
  Linux-x86_64) PLATFORM="linux-x86_64-dev"; EXT="so";;
  Linux-aarch64) PLATFORM="linux-aarch64-dev"; EXT="so";;
  *) die "Unsupported platform";;
esac

STATE_DIR="$SCRIPT_DIR/.sim_state_lez"
FRESH=0
for arg in "$@"; do [ "$arg" = "--fresh" ] && FRESH=1; done
[ "$FRESH" -eq 1 ] && rm -rf "$STATE_DIR"
mkdir -p "$STATE_DIR"

SEQUENCER_PID=""
OWN_SEQUENCER=0
INSTANCE_PIDS=()
MODULES_DIRS=()
SENDER_PID=""
RECEIVER_PID=""
EXIT_CODE=1

cleanup() {
    set +u
    echo ""; echo "=== Shutting down ==="
    [ -n "$SENDER_PID" ] && kill "$SENDER_PID" 2>/dev/null || true
    [ -n "$RECEIVER_PID" ] && kill "$RECEIVER_PID" 2>/dev/null || true
    for pid in "${INSTANCE_PIDS[@]+"${INSTANCE_PIDS[@]}"}"; do [ -n "$pid" ] && kill "$pid" 2>/dev/null || true; done
    pkill -f 'logos_host' 2>/dev/null || true
    pkill -f 'chat2mix' 2>/dev/null || true
    [ "$OWN_SEQUENCER" -eq 1 ] && [ -n "$SEQUENCER_PID" ] && kill "$SEQUENCER_PID" 2>/dev/null || true
    for mdir in "${MODULES_DIRS[@]+"${MODULES_DIRS[@]}"}"; do [ -n "$mdir" ] && rm -rf "$mdir"; done
    echo "  Logs: $STATE_DIR"; echo "Done."; exit "$EXIT_CODE"
}
trap cleanup EXIT

echo "=== Mix + LEZ RLN Simulation ($NUM_NODES nodes) ==="
echo "  LEZ repo:       $LEZ_RLN_DIR"
echo "  Delivery module: $DELIVERY_MODULE_DIR"
echo ""

pkill -f 'logos_host' 2>/dev/null || true; pkill -f 'chat2mix' 2>/dev/null || true; sleep 1

# ---------- Phase 1: Sequencer ----------
echo "[1/6] Sequencer..."
if nc -z 127.0.0.1 3040 2>/dev/null && [ "$FRESH" -eq 0 ]; then
    SEQUENCER_PID=$(lsof -ti tcp:3040 2>/dev/null || true)
    echo "  Already running (PID $SEQUENCER_PID)"
else
    [ "$(nc -z 127.0.0.1 3040 2>/dev/null; echo $?)" = "0" ] && kill "$(lsof -ti tcp:3040 2>/dev/null)" 2>/dev/null || true; sleep 1
    rm -rf "$LEZ_RLN_DIR/lssa/rocksdb"
    log "  Building sequencer..."
    if (cd "$LEZ_RLN_DIR/lssa" && cargo build --features standalone -p sequencer_service 2>&1 | tail -3); then
        SEQ_BIN="./target/debug/sequencer_service"; SEQ_CFG="sequencer/service/configs/debug/sequencer_config.json"
    elif (cd "$LEZ_RLN_DIR/lssa" && cargo build --features standalone -p sequencer_runner 2>&1 | tail -3); then
        SEQ_BIN="./target/debug/sequencer_runner"; SEQ_CFG="sequencer_runner/configs/debug"
    else die "sequencer build failed"; fi
    (cd "$LEZ_RLN_DIR/lssa" && env RUST_LOG=info "$SEQ_BIN" "$SEQ_CFG") >/dev/null 2>&1 &
    SEQUENCER_PID=$!; OWN_SEQUENCER=1; echo "  PID: $SEQUENCER_PID"
    for _ in $(seq 1 60); do nc -z 127.0.0.1 3040 2>/dev/null && break; sleep 1; done
    nc -z 127.0.0.1 3040 2>/dev/null || die "Sequencer failed to start"
    log "  Ready."
fi

# ---------- Phase 2: Deploy programs ----------
echo "[2/6] Deploying programs..."
export NSSA_WALLET_HOME_DIR="$LEZ_RLN_DIR/dev"
export WALLET_CONFIG="$NSSA_WALLET_HOME_DIR/wallet_config.json"
export WALLET_STORAGE="$NSSA_WALLET_HOME_DIR/storage.json"
TREE_ID_HEX="000102030405060708090a0b0c0d0e0f1011121314151617"
GIFTER_ACCOUNT_FILE="$HOME/.logos-lez-rln/payment_account_${TREE_ID_HEX}.txt"

rm -f "$WALLET_CONFIG" "$WALLET_STORAGE"
SETUP_OUTPUT=$(cd "$LEZ_RLN_DIR/lez-rln" && cargo run --bin run_setup 2>&1) || die "run_setup failed"
echo "$SETUP_OUTPUT" | tail -4
CONFIG_ACCOUNT=$(echo "$SETUP_OUTPUT" | grep -oE 'Config account:\s+\S+' | awk '{print $NF}' || true)
[ -z "$CONFIG_ACCOUNT" ] && die "Failed to parse config account"
GIFTER_ACCOUNT=$(cat "$GIFTER_ACCOUNT_FILE" 2>/dev/null || true)
[ -z "$GIFTER_ACCOUNT" ] && die "Gifter account not found at $GIFTER_ACCOUNT_FILE"

# ---------- Phase 3: Prerequisites ----------
echo "[3/6] Verifying prerequisites..."
CHAT2MIX_BIN="$DELIVERY_DIR/build/chat2mix"
[ -x "$CHAT2MIX_BIN" ] || die "chat2mix not built"
LOGOSCORE="${LOGOSCORE:-$(nix build github:logos-co/logos-liblogos/7df6195 --override-input logos-cpp-sdk github:logos-co/logos-cpp-sdk/a4bd66c --no-link --print-out-paths)/bin/logoscore}"
RLN_MODULE="$LEZ_RLN_DIR/logos-rln-module/result-rln/lib"
WALLET_MODULE="$LEZ_RLN_DIR/logos-rln-module/result-wallet/lib"
DELIVERY_PLUGIN="$DELIVERY_MODULE_DIR/result/lib/delivery_module_plugin.$EXT"
for check in "$RLN_MODULE/liblogos_rln_module.$EXT" "$WALLET_MODULE/liblogos_execution_zone_wallet_module.$EXT" "$DELIVERY_PLUGIN"; do
    [ -f "$check" ] || die "Missing: $check"
done
log "  All modules present."

# ---------- Phase 4: Start mix nodes ----------
echo "[4/6] Starting $NUM_NODES mix+LEZ nodes..."
LOAD_ORDER="liblogos_execution_zone_wallet_module,liblogos_rln_module,delivery_module"
WALLET_CALL="liblogos_execution_zone_wallet_module.open($WALLET_CONFIG,$WALLET_STORAGE)"
BOOTSTRAP_PEER="/ip4/127.0.0.1/tcp/$BASE_TCP_PORT/p2p/${PEER_IDS[0]}"

for i in $(seq 0 $((NUM_NODES - 1))); do
    TCP_PORT=$((BASE_TCP_PORT + i)); DISC_PORT=$((BASE_DISC_PORT + i))
    NODE_CONFIG="$STATE_DIR/node${i}_config.json"
    LOG_FILE="$STATE_DIR/node${i}.log"
    KAD_BOOTSTRAP="[]"; [ "$i" -gt 0 ] && KAD_BOOTSTRAP="[\"$BOOTSTRAP_PEER\"]"

    cat > "$NODE_CONFIG" <<EOF
{
  "clusterId": $CLUSTER_ID,
  "numShardsInNetwork": $NUM_SHARDS,
  "listenAddress": "127.0.0.1",
  "tcpPort": $TCP_PORT,
  "discv5UdpPort": $DISC_PORT,
  "nat": "extip:127.0.0.1",
  "extMultiAddrs": ["/ip4/127.0.0.1/tcp/$TCP_PORT"],
  "extMultiAddrsOnly": true,
  "nodekey": "${NODEKEYS[$i]}",
  "relay": true,
  "lightpush": true,
  "filter": true,
  "mix": true,
  "mixkey": "${MIXKEYS[$i]}",
  "mixOnchainLEZ": true,
  "enableKadDiscovery": true,
  "kadBootstrapNodes": $KAD_BOOTSTRAP,
  "peerExchange": false,
  "rendezvous": false,
  "logLevel": "TRACE"
}
EOF

    MDIR=$(mktemp -d); MODULES_DIRS+=("$MDIR")
    # Stage wallet module
    mkdir -p "$MDIR/liblogos_execution_zone_wallet_module"
    cp -L "$WALLET_MODULE/liblogos_execution_zone_wallet_module.$EXT" "$MDIR/liblogos_execution_zone_wallet_module/"
    [ -f "$WALLET_MODULE/libwallet_ffi.$EXT" ] && cp -L "$WALLET_MODULE/libwallet_ffi.$EXT" "$MDIR/liblogos_execution_zone_wallet_module/"
    echo "{\"name\":\"liblogos_execution_zone_wallet_module\",\"version\":\"1.0.0\",\"type\":\"core\",\"main\":{\"$PLATFORM\":\"liblogos_execution_zone_wallet_module.$EXT\"},\"dependencies\":[],\"capabilities\":[]}" > "$MDIR/liblogos_execution_zone_wallet_module/manifest.json"
    # Stage RLN module
    mkdir -p "$MDIR/liblogos_rln_module"
    cp -L "$RLN_MODULE/liblogos_rln_module.$EXT" "$MDIR/liblogos_rln_module/"
    cp -L "$RLN_MODULE/liblez_rln_ffi.$EXT" "$MDIR/liblogos_rln_module/" 2>/dev/null || true
    echo "{\"name\":\"liblogos_rln_module\",\"version\":\"1.0.0\",\"type\":\"core\",\"main\":{\"$PLATFORM\":\"liblogos_rln_module.$EXT\"},\"dependencies\":[\"liblogos_execution_zone_wallet_module\"],\"capabilities\":[]}" > "$MDIR/liblogos_rln_module/manifest.json"
    # Stage delivery module
    mkdir -p "$MDIR/delivery_module"
    cp -L "$DELIVERY_PLUGIN" "$MDIR/delivery_module/"
    cp -L "$DELIVERY_MODULE_DIR/result/lib/liblogosdelivery.$EXT" "$MDIR/delivery_module/" 2>/dev/null || true
    for pq in "$DELIVERY_MODULE_DIR"/result/lib/libpq*; do [ -f "$pq" ] && cp -L "$pq" "$MDIR/delivery_module/"; done
    echo "{\"name\":\"delivery_module\",\"version\":\"1.0.0\",\"type\":\"core\",\"main\":{\"$PLATFORM\":\"delivery_module_plugin.$EXT\"},\"dependencies\":[],\"capabilities\":[]}" > "$MDIR/delivery_module/manifest.json"

    log "  Starting node $i (port $TCP_PORT)..."
    if [ "$i" -eq 0 ]; then
        # Node 0: wallet + createNode + start + setRlnConfig + subscribe + selfRegisterRln
        TMPDIR=/tmp "$LOGOSCORE" -m "$MDIR" -l "$LOAD_ORDER" \
            -c "$WALLET_CALL" \
            -c "delivery_module.createNode(@$NODE_CONFIG)" \
            -c "delivery_module.start()" \
            -c "delivery_module.setRlnConfig($CONFIG_ACCOUNT,0)" \
            -c "delivery_module.subscribe($CONTENT_TOPIC)" \
            -c "delivery_module.selfRegisterRln($CONFIG_ACCOUNT,$GIFTER_ACCOUNT,100)" \
            </dev/null >"$LOG_FILE" 2>&1 &
        EXPECTED_CALLS=6
    else
        # Nodes 1-3: wallet + createNode + start + setRlnConfig + subscribe
        TMPDIR=/tmp "$LOGOSCORE" -m "$MDIR" -l "$LOAD_ORDER" \
            -c "$WALLET_CALL" \
            -c "delivery_module.createNode(@$NODE_CONFIG)" \
            -c "delivery_module.start()" \
            -c "delivery_module.setRlnConfig($CONFIG_ACCOUNT,0)" \
            -c "delivery_module.subscribe($CONTENT_TOPIC)" \
            </dev/null >"$LOG_FILE" 2>&1 &
        EXPECTED_CALLS=5
    fi
    NODE_PID=$!; INSTANCE_PIDS+=($NODE_PID)
    for t in $(seq 1 90); do
        N=$(grep -c '^Method call successful' "$LOG_FILE" 2>/dev/null || true); N=${N:-0}
        [ "$N" -ge "$EXPECTED_CALLS" ] && break; sleep 1
    done
    if [ "${N:-0}" -ge "$EXPECTED_CALLS" ]; then
        log "    Node $i ready ($N/$EXPECTED_CALLS calls) PID: $NODE_PID"
    else
        echo "  WARNING: Node $i: $N/$EXPECTED_CALLS calls"
    fi
    sleep 10
done
echo ""

# ---------- Phase 5: chat2mix ----------
echo "[5/6] Waiting 30s for mix pool, then launching chat2mix..."
sleep 30

RECEIVER_LOG="$STATE_DIR/chat2mix_receiver.log"
SENDER_LOG="$STATE_DIR/chat2mix_sender.log"

(printf 'receiver\n'; sleep 999) | "$CHAT2MIX_BIN" \
    --cluster-id=$CLUSTER_ID --num-shards-in-network=$NUM_SHARDS --shard=0 \
    --nodekey=$CHAT_RECEIVER_NODEKEY \
    --servicenode="$BOOTSTRAP_PEER" --kad-bootstrap-node="$BOOTSTRAP_PEER" \
    --log-level=TRACE >"$RECEIVER_LOG" 2>&1 &
RECEIVER_PID=$!; log "  Receiver PID: $RECEIVER_PID"
sleep 30

(
  printf 'sender\n'
  for n in $(seq 1 $NUM_TEST_MESSAGES); do sleep 5; printf '%s_%d\n' "$TEST_MESSAGE_PREFIX" "$n"; done
  sleep 10; printf '/exit\n'
) | "$CHAT2MIX_BIN" \
    --cluster-id=$CLUSTER_ID --num-shards-in-network=$NUM_SHARDS --shard=0 \
    --nodekey=$CHAT_SENDER_NODEKEY \
    --servicenode="$BOOTSTRAP_PEER" --kad-bootstrap-node="$BOOTSTRAP_PEER" \
    --log-level=TRACE >"$SENDER_LOG" 2>&1 &
SENDER_PID=$!; log "  Sender PID: $SENDER_PID"

for t in $(seq 1 300); do kill -0 "$SENDER_PID" 2>/dev/null || break; sleep 1; done
sleep 20

# ---------- Phase 6: Verify ----------
echo "[6/6] Verification"; echo ""
PASS=0; FAIL=0
check() { local c=$1 d=$2; if eval "$c"; then echo "  PASS: $d"; PASS=$((PASS+1)); else echo "  FAIL: $d"; FAIL=$((FAIL+1)); fi; }

echo "  --- logos-core nodes ---"
for i in $(seq 0 $((NUM_NODES - 1))); do
    M=$(grep -c "mounting mix protocol" "$STATE_DIR/node${i}.log" 2>/dev/null || true)
    check "[ ${M:-0} -ge 1 ]" "Node $i mounted mix ($M)"
done
echo ""
echo "  --- LEZ RLN ---"
LEZ_ROOTS=0
for i in $(seq 0 $((NUM_NODES - 1))); do
    R=$(grep -c "Polled valid roots\|Fetched roots from\|valid_roots" "$STATE_DIR/node${i}.log" 2>/dev/null || true)
    LEZ_ROOTS=$((LEZ_ROOTS + R))
done
check "[ $LEZ_ROOTS -ge 1 ]" "LEZ root polling active ($LEZ_ROOTS events across nodes)"
echo ""
echo "  --- chat2mix ---"
RECV_POOL=$(grep -c "ready to publish messages now" "$RECEIVER_LOG" 2>/dev/null || true)
check "[ ${RECV_POOL:-0} -ge 1 ]" "Receiver reached min mix pool ($RECV_POOL)"
SENT=$(grep -cE "lightpushPublish|pushed.*via mix" "$SENDER_LOG" 2>/dev/null || true)
check "[ ${SENT:-0} -ge 1 ]" "Sender published via mix ($SENT)"
RECEIVED=$(grep -c "$TEST_MESSAGE_PREFIX" "$RECEIVER_LOG" 2>/dev/null || true)
check "[ ${RECEIVED:-0} -ge 1 ]" "Receiver received test messages ($RECEIVED)"

echo ""; echo "  =========================================="
if [ "$FAIL" -eq 0 ]; then echo "  ALL $PASS CHECKS PASSED"; EXIT_CODE=0
else echo "  $FAIL FAILED, $PASS passed"; EXIT_CODE=1; fi
echo "  =========================================="
