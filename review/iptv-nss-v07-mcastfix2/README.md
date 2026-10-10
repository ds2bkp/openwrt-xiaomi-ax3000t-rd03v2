# RD03v2 IPTV/NSS review snapshot

These are the relevant files from our V0.7-mcastfix2 build, shared for review against ADCDS v1.14. This is not a ready-to-apply series against v1.14.

Our build uses OpenWrt 25ee12629e (Linux 6.12.94) and nss_packages 0d970dbf0185e3f53709bd803e8a466598023c57. The source snapshot is from private build commit 422b720c9d5b7f2555a0d87d833f15914d190d24. It does not include local configuration, credentials, firmware binaries or unrelated LuCI/honeypot changes.

The five proposed changes:
1. ECM IPv4 multicast DSA/tag_8021q handling: correct the tag for the switch conduit, compact duplicate physical outputs, rebuild flows on membership changes, and keep ambiguous VLAN-aware mappings in software.
2. Event-driven nss-mcastd: update QCA MCS flood destinations on membership notifications; accept legacy IGMPv1/v2 and MLDv1 records with filter=0 and no source list.
3. Kernel multicast-offload callback safety: BH-safe locking, RCU protection, route add/flush notifications and notification before releasing the IPv4 cache reference.
4. QCA MCS safety: observe membership without consuming the normal bridge forwarding path, bounds/privilege checks on netlink requests, and no synchronous timer cancellation under the lock taken by that timer.
5. Watchdog recovery validation: inspect AP state, survey and new WMI failures after a crash; reset failure baselines after a successful recovery; refuse overlapping module unload while netifd teardown is incomplete; retain useful PHY/device mapping in incident evidence.

The 0604-1 patch is included only as dependency/context for item 3. It is an existing QCA MCS integration patch, not claimed as our original work. Existing attribution in the patches is preserved.

For upstream integration, preserve ADCDS's encrypted-mesh recovery. Our watchdog snapshot intentionally excludes mesh rejoin for our deployment; that removal is not part of the proposal. Extract/rebase the validation and teardown changes.

ADCDS v1.14 already uses ECM patch number 0030 for policy-rule invalidation. Rename/reorder our multicast patch when combining them. QCA MCS and the daemon are additional components; they should be reviewed together with the kernel and ECM changes rather than copied as standalone fixes.

Validation on an AX3000T v2 with AN8855 and a wired IPTV receiver: the build completed, IPTV worked, ECM reported seven active IPv4 multicast accelerated connections while the TV was on, and these returned to zero after it was turned off. Membership parsing was tested with ASan/UBSan and raw legacy-filter records. These are short functional checks, not a long-term stability claim or a per-flow offloaded-byte measurement. The lock-ordering fixes address problems found during source review; they do not establish the exact cause of every earlier freeze.

No default-build changes are requested by this snapshot.
