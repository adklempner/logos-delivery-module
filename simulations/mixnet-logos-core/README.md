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

## Status (2026-04-08)

✅ **ALL 7 CHECKS PASS** end-to-end:

- All 4 logos-core nodes mount mix successfully
- Inter-node kademlia discovery populates each node's mix pool
- chat2mix sender + receiver dial node 0, fill their local mix pools, and
  exchange test messages via mix → relay → receiver

### Bug found and fixed during initial development

The first iteration of this sim failed because the JSON config set
`listenAddress: "127.0.0.1"` but didn't pin the advertised addresses. With
default `nat: "any"`, libp2p NAT discovery picked up the LAN IP (e.g.
`192.168.x.x`) and advertised it through identify and kad-dht — even though
the node was only **bound** to localhost. Other peers (and chat2mix) then
tried to dial that LAN IP and got `connection refused`, never filling their
mix pools.

The fix is master mixnet's old trick: pin both ends.

```json
{
  "listenAddress": "127.0.0.1",
  "nat": "extip:127.0.0.1",
  "extMultiAddrs": ["/ip4/127.0.0.1/tcp/<port>"],
  "extMultiAddrsOnly": true
}
```

The diagnostic that uncovered this was reading `vendor/logos-delivery/chat2mix.log`
(chronicles writes there because chat2mix is built with
`-d:chronicles_sinks=textlines[file]`, NOT to stdout/stderr) and seeing
`TcpTransport dial error: (61) Connection refused` to LAN IP addresses for
peers that should have been on `127.0.0.1`. The kad routing table on node 0
contained both:
- The advertised LAN IP `/ip4/192.168.x.x/tcp/6000N`
- An "observed" address from incoming connections, with the **source TCP port**
  of the dialing peer (e.g. `/ip4/127.0.0.1/tcp/60062`) instead of its
  listening port

Both fail the dial. The fix above eliminates the LAN-IP advertisement.

### What this proves about logos-core + mix

- The logos-core IPC architecture does NOT block mix from working
- Master `WakuNodeConf` JSON parsing in `liblogosdelivery` handles `mix`,
  `mixkey`, `enableKadDiscovery`, `kadBootstrapNodes`, `nat`, `extMultiAddrs`,
  `extMultiAddrsOnly` etc. with zero code changes
- chat2mix kad discovery finds mix peers via DHT routing and adds them to
  the local mix pool correctly
- Mix message routing through 4 hops works end-to-end

## Cleanup

```bash
rm -rf simulations/mixnet-logos-core/.sim_state
pkill -f logos_host
pkill -f chat2mix
```
