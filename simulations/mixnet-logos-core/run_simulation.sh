#!/usr/bin/env bash
# 4-node mix simulation with delivery_module mounted in logos-core (logoscore).
#
# Reproduces the master mixnet sim as faithfully as possible, but with the wakunode2
# instances replaced by logoscore instances loading the delivery_module plugin. No RLN,
# no spam protection, no custom orchestrator. The sender + receiver are master's stock
# chat2mix binary built from vendor/logos-delivery.
#
# If this minimal setup works, logos-core+mix is viable. If it fails, the first failing
# step IS the bug we want to find.
#
# Exit code 0 = test passes (chat2mix receiver got the test messages).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DELIVERY_MODULE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
DELIVERY_DIR="$DELIVERY_MODULE_DIR/vendor/logos-delivery"

export TMPDIR=/tmp

die() { echo "  FATAL: $*" >&2; exit 1; }
log() { echo "[$(date '+%H:%M:%S')] $*"; }

# --- Node identity constants (4 mix nodes) ---
# Nodekeys + peerIds copied from master mixnet config1-4.toml + a fifth bootstrap key.
# Mixkeys copied from master mixnet config1-4.toml.
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
# chat2mix uses contentTopic /chat2mix/2/$shard/proto by default (auto-sharding picks shard 0)
CONTENT_TOPIC="/chat2mix/2/0/proto"
TEST_MESSAGE_PREFIX="mixsimtestmsg"
NUM_TEST_MESSAGES=5

case "$(uname -s)-$(uname -m)" in
  Darwin-arm64) PLATFORM="darwin-arm64-dev"; EXT="dylib";;
  Linux-x86_64) PLATFORM="linux-x86_64-dev"; EXT="so";;
  Linux-aarch64) PLATFORM="linux-aarch64-dev"; EXT="so";;
  *) die "Unsupported platform";;
esac

# --- State ---
STATE_DIR="$SCRIPT_DIR/.sim_state"
FRESH=0
for arg in "$@"; do [ "$arg" = "--fresh" ] && FRESH=1; done
if [ "$FRESH" -eq 1 ]; then
    # Preserve RLN credentials across runs (expensive to regenerate)
    rm -f "$STATE_DIR"/node*.log "$STATE_DIR"/node*_config.json \
          "$STATE_DIR"/chat2mix_*.log
fi
mkdir -p "$STATE_DIR"

INSTANCE_PIDS=()
MODULES_DIRS=()
SENDER_PID=""
RECEIVER_PID=""
EXIT_CODE=1

cleanup() {
    set +u
    echo ""
    echo "=== Shutting down ==="
    [ -n "$SENDER_PID" ] && kill "$SENDER_PID" 2>/dev/null || true
    [ -n "$RECEIVER_PID" ] && kill "$RECEIVER_PID" 2>/dev/null || true
    for pid in "${INSTANCE_PIDS[@]+"${INSTANCE_PIDS[@]}"}"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    pkill -f 'logos_host' 2>/dev/null || true
    pkill -f 'chat2mix' 2>/dev/null || true
    for mdir in "${MODULES_DIRS[@]+"${MODULES_DIRS[@]}"}"; do
        [ -n "$mdir" ] && rm -rf "$mdir"
    done
    echo "  Logs: $STATE_DIR"
    echo "Done."
    exit "$EXIT_CODE"
}
trap cleanup EXIT

echo "=== Mixnet logos-core Simulation ($NUM_NODES nodes) ==="
echo "  Module repo:    $DELIVERY_MODULE_DIR"
echo "  Delivery (vendor): $DELIVERY_DIR"
echo "  Cluster ID:     $CLUSTER_ID"
echo "  Content topic:  $CONTENT_TOPIC"
echo ""

pkill -f 'logos_host' 2>/dev/null || true
pkill -f 'chat2mix' 2>/dev/null || true
sleep 1

# ---------- Phase 1: Verify chat2mix + delivery_module are built ----------
echo "[1/5] Verifying prerequisites..."

# Initialize the vendored logos-delivery submodule if missing.
# Init each nested submodule individually so a single stale pinned subref doesn't
# cascade and leave later submodules empty (which would break nix's submodule fetcher).
if [ ! -f "$DELIVERY_DIR/Makefile" ] || [ ! -d "$DELIVERY_DIR/vendor/db_connector" ]; then
    log "  Initializing vendor/logos-delivery (and its vendor/ submodules)..."
    (cd "$DELIVERY_MODULE_DIR" && git submodule update --init vendor/logos-delivery)
    (cd "$DELIVERY_DIR" && \
        git submodule init && \
        for mod in $(git config --file .gitmodules --get-regexp path | awk '{print $2}'); do
            git submodule update --init --recursive "$mod" 2>/dev/null || \
                echo "  warn: $mod recursive init incomplete (likely stale subref)"
        done)
fi

CHAT2MIX_BIN="$DELIVERY_DIR/build/chat2mix"
if [ ! -x "$CHAT2MIX_BIN" ]; then
    log "  Building chat2mix from vendor/logos-delivery..."
    (cd "$DELIVERY_DIR" && make chat2mix) || die "make chat2mix failed"
fi
log "  chat2mix:        $CHAT2MIX_BIN"

LOGOSCORE="${LOGOSCORE:-$(nix build github:logos-co/logos-liblogos/7df6195 --override-input logos-cpp-sdk github:logos-co/logos-cpp-sdk/a4bd66c --no-link --print-out-paths)/bin/logoscore}"
[ -x "$LOGOSCORE" ] || die "logoscore not found at $LOGOSCORE"
log "  logoscore:       $LOGOSCORE"

DELIVERY_PLUGIN="$DELIVERY_MODULE_DIR/result/lib/delivery_module_plugin.$EXT"
if [ ! -f "$DELIVERY_PLUGIN" ]; then
    log "  Building delivery_module via nix..."
    (cd "$DELIVERY_MODULE_DIR" && \
        nix build --override-input logos-delivery "git+file://$(pwd)/vendor/logos-delivery?submodules=1") || \
        die "nix build delivery_module failed"
fi
log "  delivery_module: $DELIVERY_PLUGIN"
echo ""

# ---------- Phase 2: Stage + Start nodes ----------
echo "[2/5] Starting $NUM_NODES mix nodes..."

LOAD_ORDER="delivery_module"
BOOTSTRAP_PEER="/ip4/127.0.0.1/tcp/$BASE_TCP_PORT/p2p/${PEER_IDS[0]}"

for i in $(seq 0 $((NUM_NODES - 1))); do
    TCP_PORT=$((BASE_TCP_PORT + i))
    DISC_PORT=$((BASE_DISC_PORT + i))
    NODE_CONFIG="$STATE_DIR/node${i}_config.json"
    LOG_FILE="$STATE_DIR/node${i}.log"

    # Node 0 is the kad bootstrap; nodes 1-3 connect to it via kadBootstrapNodes.
    KAD_BOOTSTRAP="[]"
    [ "$i" -gt 0 ] && KAD_BOOTSTRAP="[\"$BOOTSTRAP_PEER\"]"

    # Critical: pin both listen + advertised addrs to 127.0.0.1.
    # Without nat=extip:127.0.0.1 + extMultiAddrsOnly=true, libp2p NAT discovery
    # picks up the LAN IP (192.168.x.x) and advertises it through identify/kad,
    # even though listenAddress=127.0.0.1 means we only bind to localhost. Then
    # other nodes try to dial that LAN IP and get connection-refused, never
    # filling their mix pool. Master mixnet's config1.toml uses the same trick.
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
  "enableKadDiscovery": true,
  "kadBootstrapNodes": $KAD_BOOTSTRAP,
  "peerExchange": false,
  "rendezvous": false,
  "logLevel": "TRACE"
}
EOF

    # Stage delivery_module only
    MDIR=$(mktemp -d)
    MODULES_DIRS+=("$MDIR")

    mkdir -p "$MDIR/delivery_module"
    cp -L "$DELIVERY_PLUGIN" "$MDIR/delivery_module/"
    cp -L "$DELIVERY_MODULE_DIR/result/lib/liblogosdelivery.$EXT" "$MDIR/delivery_module/" 2>/dev/null || true
    for pq in "$DELIVERY_MODULE_DIR"/result/lib/libpq*; do
        [ -f "$pq" ] && cp -L "$pq" "$MDIR/delivery_module/"
    done
    echo "{\"name\":\"delivery_module\",\"version\":\"1.0.0\",\"type\":\"core\",\"main\":{\"$PLATFORM\":\"delivery_module_plugin.$EXT\"},\"dependencies\":[],\"capabilities\":[]}" > "$MDIR/delivery_module/manifest.json"

    log "  Starting node $i (port $TCP_PORT)..."
    # Run from STATE_DIR so WakuMix finds rln_keystore_<peerId>.json + rln_tree.db
    (cd "$STATE_DIR" && TMPDIR=/tmp "$LOGOSCORE" -m "$MDIR" -l "$LOAD_ORDER" \
        -c "delivery_module.createNode(@$NODE_CONFIG)" \
        -c "delivery_module.start()" \
        -c "delivery_module.subscribe($CONTENT_TOPIC)" \
        </dev/null >"$LOG_FILE" 2>&1) &
    NODE_PID=$!
    INSTANCE_PIDS+=($NODE_PID)

    # Wait for 3/3 method calls
    EXPECTED_CALLS=3
    for t in $(seq 1 60); do
        N=$(grep -c '^Method call successful' "$LOG_FILE" 2>/dev/null || true); N=${N:-0}
        [ "$N" -ge "$EXPECTED_CALLS" ] && break
        sleep 1
    done

    if [ "${N:-0}" -ge "$EXPECTED_CALLS" ]; then
        log "    Node $i ready ($N/$EXPECTED_CALLS calls) PID: $NODE_PID"
    else
        echo "  WARNING: Node $i: $N/$EXPECTED_CALLS calls"
    fi

    sleep 5
done
echo ""

# ---------- Phase 3: Wait for mix pool to fill ----------
echo "[3/5] Waiting 30s for mix pool to fill (kademlia discovery)..."
sleep 30
echo ""

# ---------- Phase 4: Launch chat2mix receiver + sender ----------
echo "[4/5] Launching chat2mix receiver and sender..."

RECEIVER_LOG="$STATE_DIR/chat2mix_receiver.log"
SENDER_LOG="$STATE_DIR/chat2mix_sender.log"

# Deterministic nodekeys for chat2mix so their peer IDs match the pre-generated RLN keystores.
# These match the poc/mix-spam-protection branch's setup_credentials.nim NodeConfigs.
CHAT_RECEIVER_NODEKEY="cb6fe589db0e5d5b48f7e82d33093e4d9d35456f4aaffc2322c473a173b2ac49"
CHAT_SENDER_NODEKEY="35eace7ccb246f20c487e05015ca77273d8ecaed0ed683de3d39bf4f69336feb"

# Receiver: long-running, just listens. Pipe a /nick command and then stay open.
# Run from STATE_DIR so WakuMix finds rln_keystore_<peerId>.json + rln_tree.db
(
  printf 'receiver\n'
  sleep 999
) | (cd "$STATE_DIR" && "$CHAT2MIX_BIN" \
    --cluster-id=$CLUSTER_ID \
    --num-shards-in-network=$NUM_SHARDS \
    --shard=0 \
    --nodekey=$CHAT_RECEIVER_NODEKEY \
    --servicenode="$BOOTSTRAP_PEER" \
    --kad-bootstrap-node="$BOOTSTRAP_PEER" \
    --log-level=TRACE \
    >"$RECEIVER_LOG" 2>&1) &
RECEIVER_PID=$!
log "  Receiver PID: $RECEIVER_PID"

# Give the receiver ~30s to come up and join the mix pool
sleep 30

# Sender: send NUM_TEST_MESSAGES messages then exit
(
  printf 'sender\n'
  for n in $(seq 1 $NUM_TEST_MESSAGES); do
    sleep 5
    printf '%s_%d\n' "$TEST_MESSAGE_PREFIX" "$n"
  done
  sleep 10
  printf '/exit\n'
) | (cd "$STATE_DIR" && "$CHAT2MIX_BIN" \
    --cluster-id=$CLUSTER_ID \
    --num-shards-in-network=$NUM_SHARDS \
    --shard=0 \
    --nodekey=$CHAT_SENDER_NODEKEY \
    --servicenode="$BOOTSTRAP_PEER" \
    --kad-bootstrap-node="$BOOTSTRAP_PEER" \
    --log-level=TRACE \
    >"$SENDER_LOG" 2>&1) &
SENDER_PID=$!
log "  Sender PID:   $SENDER_PID"

# Wait until sender exits (or 5 minutes max)
for t in $(seq 1 300); do
    if ! kill -0 "$SENDER_PID" 2>/dev/null; then
        break
    fi
    sleep 1
done

# Give the network another 20s to finish propagating
sleep 20
echo ""

# ---------- Phase 5: Verify ----------
echo "[5/5] Verification"
echo ""

PASS_COUNT=0
FAIL_COUNT=0
check() {
    local cond=$1; local desc=$2
    if eval "$cond"; then
        echo "  PASS: $desc"
        PASS_COUNT=$((PASS_COUNT + 1))
    else
        echo "  FAIL: $desc"
        FAIL_COUNT=$((FAIL_COUNT + 1))
    fi
}

# 1. Each logos-core node mounted mix successfully
echo "  --- logos-core nodes ---"
for i in $(seq 0 $((NUM_NODES - 1))); do
    LOG="$STATE_DIR/node${i}.log"
    M=$(grep -c "mounting mix protocol\|mountMix" "$LOG" 2>/dev/null || true); M=${M:-0}
    check "[ $M -ge 1 ]" "Node $i mounted mix protocol ($M)"
done

# 2. Receiver reached the mix pool min
echo ""
echo "  --- chat2mix receiver ---"
RECV_POOL=$(grep -c "ready to publish messages now" "$RECEIVER_LOG" 2>/dev/null || true); RECV_POOL=${RECV_POOL:-0}
check "[ $RECV_POOL -ge 1 ]" "Receiver reached min mix pool ($RECV_POOL)"

# 3. Sender published messages
echo ""
echo "  --- chat2mix sender ---"
SENT=$(grep -cE "lightpushPublish|publishing|pushed.*via mix" "$SENDER_LOG" 2>/dev/null || true); SENT=${SENT:-0}
check "[ $SENT -ge 1 ]" "Sender published via mix ($SENT log lines)"

# 4. Receiver got the test messages
echo ""
echo "  --- end-to-end ---"
RECEIVED=$(grep -c "$TEST_MESSAGE_PREFIX" "$RECEIVER_LOG" 2>/dev/null || true); RECEIVED=${RECEIVED:-0}
check "[ $RECEIVED -ge 1 ]" "Receiver received test messages ($RECEIVED matches)"

echo ""
echo "  =========================================="
if [ "$FAIL_COUNT" -eq 0 ]; then
    echo "  ALL $PASS_COUNT CHECKS PASSED"
    EXIT_CODE=0
else
    echo "  $FAIL_COUNT FAILED, $PASS_COUNT passed"
    EXIT_CODE=1
fi
echo "  =========================================="
