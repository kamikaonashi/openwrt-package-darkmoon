# darkmoon

A lightweight, CPU-efficient C implementation of [cake-autorate](https://github.com/lynxthecat/cake-autorate) for OpenWrt 24.10 and 25.12.

darkmoon continuously measures latency to a set of reflectors and adjusts the CAKE shaper bandwidth so queues never build up. It is meant for links whose capacity varies: LTE/5G, Starlink, busy cable segments, Wi-Fi backhauls, and DSL with fluctuating sync.

*All credit for the algorithm goes to [@lynxthecat](https://github.com/lynxthecat) and the cake-autorate contributors.*

---

## Features

- **Faithful cake-autorate algorithm**: per-reflector OWD baselines with asymmetric EWMA, bufferbloat detection window, load classification, interpolated rate increases and decreases, shaper-delay compensated thresholds, and decay toward a base rate.
- **Full reflector management**: replacement of unresponsive reflectors, periodic comparison (baseline and jitter outliers are rotated out), periodic random rotation, a spare queue, and retained statistics.
- **Idle sleep and stall handling**: pings pause after sustained idle and resume instantly on traffic. Includes connection-stall detection, a global ping timeout with pinger restart, and optional minimum-rate enforcement.
- **Two measurement modes**: ICMP echo (RTT/2, works everywhere) or ICMP timestamp (true one-way delay per direction; clock offsets and clock steps are handled).
- **Standalone shaping**: creates the IFB, CAKE qdiscs, ingress qdisc and matchall/mirred redirect itself via netlink. No sqm-scripts, no `tc`/`ip` binaries, no shell.
- **Self-healing**: waits for the WAN device at boot (PPPoE, LTE), reacts immediately to link changes, and rebuilds shaping if the device is recreated or the qdiscs are removed.
- **Efficient**: one single-threaded process on libubox `uloop`, kernel-side BPF filtering of ICMP replies, and no floating point. About 0.2 % of one Cortex-A53 core and under 1 MB RSS on a Filogic router.
- **Status over ubus**: `ubus call darkmoon.<instance> status`. Nothing is written to flash or tmpfs.
- **Optional LuCI app**: live dashboard with charts, a full settings page and an Overview widget.
- **Multiple instances** for multi-WAN setups.

---

## Installation

Download the packages for your architecture from the Releases page:

| OpenWrt | Daemon | Web interface (optional) |
| :-- | :-- | :-- |
| 25.12 | `darkmoon-<ver>.apk` | `luci-app-darkmoon-<ver>.apk` |
| 24.10 | `darkmoon_<ver>_<arch>.ipk` | `luci-app-darkmoon_<ver>_all.ipk` |

```sh
# OpenWrt 25.12
apk add --allow-untrusted darkmoon-*.apk luci-app-darkmoon-*.apk
# OpenWrt 24.10
opkg install darkmoon_*.ipk luci-app-darkmoon_*.ipk
```

Dependencies (`kmod-sched-cake`, `kmod-ifb`, `libubox`, `libubus`, `libuci`) are pulled in automatically when the package feeds are reachable.

### Building

With an OpenWrt SDK:

```sh
echo "src-link darkmoon /path/to/this/repo" >> feeds.conf.default
./scripts/feeds update -a && ./scripts/feeds install darkmoon
make package/darkmoon/compile V=s     # builds darkmoon and luci-app-darkmoon
```

---

## Quick start

1. **Disable hardware flow offloading** (Network → Firewall → *Hardware flow offloading*). Hardware-offloaded connections bypass the CPU and therefore CAKE. Software offloading is fine.
2. Make sure no SQM queue shapes the same interface.
3. Open **Services → Darkmoon → Settings** (or edit `/etc/config/darkmoon`):
   - **WAN device**: e.g. `wan`, `eth1`, `pppoe-wan`, `wwan0`
   - **Minimum / base / maximum** rates per direction, in kbit/s:
     - *maximum*: the most your line can ever do (usually your plan speed)
     - *base*: a rate the line reliably achieves; darkmoon settles here when idle
     - *minimum*: the lowest acceptable rate under heavy bufferbloat
4. Tick **Enable** and press **Save & Apply**.

Via SSH:

```sh
uci set darkmoon.primary.ul_if='wan'
uci set darkmoon.primary.min_dl_shaper_rate_kbps='20000'
uci set darkmoon.primary.base_dl_shaper_rate_kbps='80000'
uci set darkmoon.primary.max_dl_shaper_rate_kbps='120000'
uci set darkmoon.primary.min_ul_shaper_rate_kbps='5000'
uci set darkmoon.primary.base_ul_shaper_rate_kbps='20000'
uci set darkmoon.primary.max_ul_shaper_rate_kbps='40000'
uci set darkmoon.primary.enabled='1'
uci commit darkmoon && /etc/init.d/darkmoon restart
```

Configuration changes are picked up automatically: procd restarts the instance when `/etc/config/darkmoon` changes.

---

## How it works

Each reflector is pinged every `reflector_ping_interval_s` (default 0.3 s). With 6 reflectors that gives a sample roughly every 50 ms. For every reply darkmoon:

1. updates that reflector's **baseline** OWD (it follows decreases quickly and increases very slowly),
2. records the **delta** above baseline in a sliding window, counting a sample as *delayed* when the delta exceeds the delay threshold plus the serialisation time of one full packet at the current rate,
3. declares **bufferbloat** when at least `bufferbloat_detection_thr` of the last `bufferbloat_detection_window` samples are delayed, and
4. adjusts each direction:

| Condition | Action |
| :-- | :-- |
| Bufferbloat | Decrease by 1–25 %, harder the larger the average delta (with a refractory period) |
| High load (achieved > 75 % of shaper) | Increase by 0–4 %, slower as the delta approaches the threshold |
| Low load / idle | Decay 1 % per second toward the base rate |

Reflectors that stop answering, have a clearly worse baseline, or are noticeably more jittery than the best reflector are swapped for a spare. After `sustained_idle_sleep_thr_s` of idle the pingers sleep until traffic returns.

### Download shaping

By default darkmoon creates an IFB device (`dl_if`, default `ifb-wan`) and redirects all ingress traffic of the WAN device to it, where a CAKE qdisc in `ingress` mode shapes it. If `dl_if` names an existing non-IFB device such as `br-lan`, darkmoon instead puts CAKE on that device's egress. Traffic terminating on the router itself is then not shaped.

### Echo vs timestamp pings

- **echo** (default) works with any host, but only measures round-trip time. A delay in either direction counts against both.
- **timestamp** gives true per-direction delay. It needs reflectors that answer ICMP timestamp requests; most public DNS servers don't, and some carrier-grade NAT gateways drop these packets entirely. See [tievolu/timestamp-reflectors](https://github.com/tievolu/timestamp-reflectors). Those CSV lists can be used directly with `option reflectors_file`; a random subset of 128 addresses is taken from large files.

---

## Status and troubleshooting

```sh
/etc/init.d/darkmoon status            # all instances
ubus call darkmoon.primary status      # one instance, JSON
logread -e darkmoon                    # log (set option debug '1' for every decision)
tc -s qdisc show dev ifb-wan           # download CAKE
tc -s qdisc show dev wan               # upload CAKE
```

The `state` field is one of:

- `running`
- `idle`: pingers asleep
- `stall`: no reflector replies and no traffic
- `waiting`: the WAN device is missing or qdisc setup failed; the `error` field says why

---

## Configuration reference

All options with their defaults are documented in [`root/etc/config/darkmoon`](root/etc/config/darkmoon). Names and defaults follow upstream cake-autorate where an equivalent exists. darkmoon-specific options:

| Option | Default | Description |
| :-- | :-- | :-- |
| `link_layer` | `ethernet` | Overhead preset (`raw`, `ethernet`, `docsis`, `pppoe-ptm`, `bridged-ptm`, ATM variants, or `custom` with `cake_overhead`/`cake_mpu`/`cake_atm`) |
| `cake_diffserv` | `diffserv4` | CAKE tin structure |
| `cake_dl_flow_mode` / `cake_ul_flow_mode` | `dual-dsthost` / `dual-srchost` | Per-direction fairness |
| `cake_nat`, `cake_wash`, `cake_ack_filter`, `cake_split_gso` | `1`, `1`, `0`, `1` | CAKE flags (wash and ACK filter apply to upload only) |
| `cake_rtt_ms`, `cake_memlimit_kb` | `0` | 0 = CAKE defaults |
| `cake_mq` | `0` | Use `cake_mq` on upload when the kernel supports it (Linux 6.19+); falls back to `cake` |
| `ping_type` | `echo` | `echo` or `timestamp` |
| `reflectors_file` | empty | Plain list or CSV (first column), replaces `reflectors` |
| `bind_ping_to_ul_if` | `0` | Bind pings to the WAN device (multi-WAN) |
| `retain_reflector_stats` | `1` | Remember baselines of rotated-out reflectors |
| `debug` | `0` | Log every decision |

Configs from darkmoon 1.x (numeric `cake_*` values, `cake_flow_mode`, `cake_overhead`) are still accepted.

---

## Credits

Original algorithm and concept: [@lynxthecat](https://github.com/lynxthecat), [cake-autorate](https://github.com/lynxthecat/cake-autorate)

C implementation and OpenWrt integration: kamikaonashi

## License

This project is released under the MIT License.

The original cake-autorate project is licensed under its own terms. Please see the upstream repository for details.
