# OpenWrt for EnGenius IPQ807x access points, with NSS offload

OpenWrt for four EnGenius IPQ807x APs — including the **ECW336**, a tri-band
Wi-Fi 6E AP with no OpenWrt support anywhere else — built on OpenWrt main with
Qualcomm NSS hardware offload on kernel 6.18.

> Unofficial. Not affiliated with, endorsed by, or supported by EnGenius or
> Senao; those names identify the affected products only. For interoperability
> and self-hosting **on hardware you own**.

Branch: **`engenius-ipq807x`**. Releases: [this repo's releases
page](https://github.com/ParkWardRR/openwrt-engenius-nss-edma/releases).

## Supported boards

| SKU | `product_id` | Board | Radios | Uplink | Hardware-tested |
|---|---|---|---|---|---|
| **ECW336** (Wi-Fi 6E) | 291 (`0x0123`) | AP-HK01-C6 | QCN5024 2.4 + QCN5054 5 + **QCN9074 6 GHz on PCIe** | 5 GbE AQR114C | **Yes** |
| EWS377AP v3 | 282 (`0x011a`) | AP-HK07 | QCN5024 + QCN5054 | 2.5 GbE QCA8081 | No |
| ECW230v3 | 284 (`0x011c`) | AP-HK07 | QCN5024 + QCN5054 | 2.5 GbE QCA8081 | No |
| EWS377-FIT | 300 (`0x012c`) | AP-HK07 | QCN5024 + QCN5054 | 2.5 GbE QCA8081 | No |

**Only the ECW336 has been booted from these images.** The three AP-HK07 boards
are published because they build from the same tree and share the platform
plumbing — not because anyone has run them. Each release's notes carry a
per-board verification table; read it before flashing.

### Board identity: trust `compatible`, not the labels

Worth knowing if you touch the DTS. These images select a device tree by the
config name u-boot asks for, and **the names lie**:

- The ECW336's stock DTB sits in the slot named `hk07` and even carries
  `model = "…IPQ8074/AP-HK07"`, but its `compatible` is
  `qcom,ipq8074-ap-hk01-c6`. The board is **AP-HK01-C6** — an HK01 host plus a
  Pine-class QCN9074 6 GHz card. `C6` is board revision 6; it has nothing to do
  with 6 GHz.
- The stock kernel is a **multi-board blob** carrying ~25 reference DTBs
  (`hk01`…`hk14`, `ac0x`, `oak0x`), and its FIT `configurations/default` points
  at `config@hk08`, which matches neither the board nor the DTB that boots. The
  default is a build artifact.
- `machid` is a **slot selector, not a product ID**: an EWS377-FIT reports the
  same `machid=0x8010006` as the ECW336, differing only in `hw_id`. Each
  vendor's firmware writes its own board content into the slot its hardware
  matches.

So read the DTB's `compatible`; ignore the slot name, the `model` string and the
FIT default. See
[`ews377ap-v3-port/sibling-sku-firmware-analysis.md`](ews377ap-v3-port/sibling-sku-firmware-analysis.md).

## Installing

**Write the INACTIVE A/B slot and keep a UART adapter attached.** These units
keep `rootfs` at `0x1000000` and `rootfs_1` at `0x87c0000`, selected by the
u-boot env `active_fw`, so a bad image is undone by flipping one variable.

Two cautions, both learned the hard way:

- **The bootloader env is append-only — never erase or rebuild it.** On the
  ECW336 the unit's only real MAC lives in the env (u-boot reports the ART copy
  invalid and falls back to a Qualcomm dummy), so `env default -a` is permanent
  identity loss. A *valid-but-incomplete* env is the one thing that hard-bricks
  these boards.
- **ECW230v3: check `cat /proc/mtd` first.** Units have been observed with the
  `rootfs` / `rootfs_1` labels **reversed**, so an installer assuming offsets
  can overwrite the running slot. Resolve partitions by name, never by index.

Treat `0:ART` as read-only — it holds factory calibration and MACs.

## NSS offload

NSS runs on top of the upstream `qca_edma` and `qca_ppe` drivers on kernel 6.18;
it does not use the out-of-tree `qca-nss-dp` / `qca-ssdk` drivers. Offloaded:
IPv4 NAT and IPv6 routing via ECM, PPPoE, 802.1Q VLAN, bridging, multicast
snooping, the NSS qdiscs for SQM, and ath11k Wi-Fi offload.

Verified on the ECW336: `NSS.FW.12.5-210-HK.R`, offload active with ECM
acceleration, and 57% of received frames forwarded by the firmware without the
host seeing them. As far as we know this is the first time this NSS plane has
been demonstrated on anything other than a Xiaomi AX3600.

Images always boot on the host datapath first; the `nss` service then starts the
offload. `uci set nss.general.enabled=0` runs the AP as stock OpenWrt with no
NSS module loaded.

## Building

```sh
git clone -b engenius-ipq807x https://github.com/ParkWardRR/openwrt-engenius-nss-edma.git
cd openwrt-engenius-nss-edma
cp feeds.conf.default feeds.conf
echo "src-git nss https://github.com/JuliusBairaktaris/nss-packages.git;edma-nss" >> feeds.conf
./scripts/feeds update -a && ./scripts/feeds install -a
make menuconfig   # qualcommax/ipq807x, pick a DEVICE_engenius_* profile
make -j$(nproc)
```

Without the nss feed, `ATH11K_NSS_SUPPORT` has an unmet dependency and
menuconfig fails.

`ews377ap-v3-port/build-skus.sh` builds the AP-HK07 trio as separate
single-profile builds so each image ships only its own board data.

> **After any change that can move a kernel CONFIG symbol, delete the kernel
> build tree.** OpenWrt rebuilds the kernel without rebuilding already-built
> kmods, and a stale out-of-tree kmod does not fail to load — it *corrupts*.
> Selecting `nss-tools` turns on `CONFIG_NET_SCHED`, which inserts `tc_index`
> into `struct sk_buff` and shifts every later field by 4; that surfaced as a
> ~4 GiB DMA map and a `wiphy_register` oops that looked for all the world like
> NSS bugs. `tools/build-ecw336.sh --clean-kernel` in
> [ECW336-firmware-tools](https://github.com/ParkWardRR/ECW336-firmware-tools)
> does the clean and then asserts the kernel and `cfg80211.ko` agree on
> `skb->tail` before it finishes.

## Related repositories

- [ECW336-firmware-tools](https://github.com/ParkWardRR/ECW336-firmware-tools) —
  stock-firmware analysis, the no-brick flash procedure, board notes, build and
  bring-up runbooks
- [engenius-field-guide](https://github.com/ParkWardRR/engenius-field-guide) —
  Senao/EnGenius firmware format, model codes, cross-flashing, controller
  self-hosting
- [pelegrun-ap-hk07-firmware-tools](https://github.com/ParkWardRR/pelegrun-ap-hk07-firmware-tools) —
  no-brick cross-flash and recovery toolkit for the AP-HK07 SKUs

## Acknowledgements

This tree stands on other people's work:

- [Julius Bairaktaris](https://github.com/JuliusBairaktaris) — the
  `openwrt-nss-edma` tree and the `nss-packages` feed this fork is based on and
  still builds against.
- [Christian Marangi (Ansuel)](https://github.com/Ansuel) — the upstream EDMA
  and PPE drivers.
- [Robert Marko (robimarko)](https://github.com/robimarko) — maintaining the
  OpenWrt qualcommax target.
- [qosmio](https://github.com/qosmio/openwrt-ipq) — the NSS packaging and
  firmware tarballs the feed builds on.
- Qualcomm and CodeLinaro — the open-source NSS host components.

## License

GPL-2.0, see [LICENSE](LICENSE). The NSS components keep their upstream
licenses.
