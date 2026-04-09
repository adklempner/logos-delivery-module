# Mixnet logos-core simulation

A 4-node mix simulation that runs `delivery_module` (this Qt plugin) mounted in
`logoscore` instances. Reproduces the master `vendor/logos-delivery/simulations/mixnet/`
setup with logoscore replacing the standalone `wakunode2` binaries. Uses master's stock
`chat2mix` (built from the vendored `logos-delivery` submodule) as both sender and
receiver.

The goal of this sim is to **isolate the mix-via-logos-core failure mode** with the
smallest possible reproducer: no RLN, no spam protection, no custom orchestrator
plugin, no vendor/nim-libp2p patches, no fork of `logos-delivery`. Any failure observed
here is a problem with mix running through the logos-core IPC architecture itself.

## Prerequisites

- **Nix** (with flakes enabled) — to build `delivery_module` and fetch `logoscore`
- **make + nim toolchain** (the vendored `nimbus-build-system` brings its own) — to
  build `chat2mix` from `vendor/logos-delivery`
- **SSH access** to the GitHub repos
- ~5 GB free disk space

## Setup

```bash
git clone --recurse-submodules \
    -b feat/mixnet-logos-core-sim \
    git@github.com:adklempner/logos-delivery-module.git
cd logos-delivery-module
```

That's it — everything (the C++ plugin, the vendored `logos-delivery` Nim source for
`chat2mix`, the build glue) lives in this one repo.

## Run

```bash
bash simulations/mixnet-logos-core/run_simulation.sh --fresh
```

The script auto-builds anything that's missing on first run:

1. Initializes `vendor/logos-delivery` submodule if needed
2. Builds `chat2mix` from `vendor/logos-delivery` via `make chat2mix` (~3-5 min on
   first run, cached after)
3. Builds `delivery_module` via `nix build` with the local `vendor/logos-delivery`
   passed as the flake's `logos-delivery` input (~3-5 min on first run, cached after)
4. Stages each node's `delivery_module` plugin into a temp directory + manifest
5. Launches 4 `logoscore` instances, each with this JSON config passed to
   `delivery_module.createNode`:
   ```json
   {
     "clusterId": 2,
     "numShardsInNetwork": 1,
     "tcpPort": 6000{1..4},
     "discv5UdpPort": 900{1..4},
     "nodekey": "<deterministic hex>",
     "relay": true, "lightpush": true, "filter": true,
     "mix": true,
     "mixkey": "<deterministic hex>",
     "enableKadDiscovery": true,
     "kadBootstrapNodes": ["/ip4/127.0.0.1/tcp/60001/p2p/<node0_peer_id>"],
     "peerExchange": false,
     "rendezvous": false,
     "logLevel": "TRACE"
   }
   ```
   Node 0 is the kad bootstrap and lightpush service node; nodes 1-3 use it as their
   kad bootstrap.
6. Waits 30s for kademlia to populate the mix pool
7. Launches `chat2mix` receiver, then sender (both pointing at node 0)
8. Sends 5 test messages from sender → mix → receiver
9. Verifies receiver got the messages

`--fresh` wipes the per-run state directory under
`simulations/mixnet-logos-core/.sim_state/` and starts clean.

## What's wired (no code changes needed)

`vendor/logos-delivery` is pinned at upstream `master` (commit `0ad55159`), which
already has everything we need:

- `WakuNodeConf` in `tools/confutils/cli_args.nim` has `mix`, `mixkey`, `mixnodes`,
  `enableKadDiscovery`, `kadBootstrapNodes` fields, and `toWakuConf` already wires
  them through to `MixConfBuilder` and `kademliaDiscoveryConf`
- `liblogosdelivery/logos_delivery_api/node_api.nim`'s `CreateNodeRequest` parses
  JSON via `fieldPairs(WakuNodeConf)` + `parseCmdArg`, with a generic
  `parseCmdArg[T](type seq[T], s: string)` that handles both `seq[string]` and
  `seq[MixNodePubInfo]` as JSON arrays
- Master `delivery_module_plugin.cpp` already exposes `createNode`, `start`,
  `subscribe`, `unsubscribe`, `send` as `Q_INVOKABLE`

## Logs

After a run, inspect:

```
.sim_state/node{0..3}.log         # logoscore + delivery_module + waku output
.sim_state/node{0..3}_config.json # the JSON sent to createNode
.sim_state/chat2mix_receiver.log  # receiver echo + chronicles
.sim_state/chat2mix_sender.log    # sender echo + chronicles
```

Useful greps:

```bash
# Did each logos-core node mount mix?
grep -l "mounting mix protocol" .sim_state/node*.log

# Did kademlia discover the other mix peers? (should be >0 on each)
for f in .sim_state/node*.log; do
  echo "$f: $(grep -c 'found mix protocol service' $f)"
done

# Did chat2mix reach the mix pool minimum (4)?
grep "ready to publish messages now" .sim_state/chat2mix_*.log

# Did the receiver get the test messages?
grep "mixsimtestmsg" .sim_state/chat2mix_receiver.log
```

## Known status (2026-04-08)

✅ All 4 logos-core nodes mount mix and successfully discover each other via
kademlia (`found mix protocol service` is logged dozens of times across runs).

✅ `chat2mix` clients dial node 0 and appear in its kademlia peer list as
`protocols=[] hasMixPubKey=false` (correct — they run with `advertiseMix=false`).

❌ `chat2mix` waits indefinitely in
`while node.getMixNodePoolSize() < MinMixNodePoolSize=4` and never reaches the
publish loop. The 4 logos-core nodes ARE in node 0's DHT, but chat2mix's local
mix pool stays at 0.

Hypotheses to investigate:
1. chat2mix's `wakuKademlia.start(minMixPeers=...)` may not be running
   `findNode` queries that would discover the other mix nodes via DHT
2. Even if chat2mix learns of the 4 mix peers via kad, it may not be
   registering them with `wakuMix.addMixPeer`
3. chat2mix's chronicles output isn't showing up in the captured log file
   (only the few `echo` lines), making it hard to see what its kad client is
   actually doing — first concrete fix is to figure out where its log is going

## Cleanup

```bash
rm -rf simulations/mixnet-logos-core/.sim_state
pkill -f logos_host
pkill -f chat2mix
```
