# WireGuard build reference

I enabled WireGuard on our RD03v2 build by selecting kmod-wireguard, wireguard-tools and luci-proto-wireguard, with their normal crypto and UDP-tunnel dependencies. The relevant selections are in enable-wireguard.config; wireguard-config.diff shows the WireGuard-related changes recorded on 2026-10-08.

There is no new WireGuard driver or cryptography patch in this contribution. The sources are the existing OpenWrt packages:

- [WireGuard kernel package](https://github.com/openwrt/openwrt/blob/25ee12629edcc38feffbd06255dd47840cd7af7e/package/kernel/linux/modules/netsupport.mk)
- [WireGuard tools and netifd helper](https://github.com/openwrt/openwrt/tree/25ee12629edcc38feffbd06255dd47840cd7af7e/package/network/utils/wireguard-tools)
- [LuCI protocol package](https://github.com/openwrt/luci/tree/7c91cb278d7cf19878069a44d5d1992f65c4e315/protocols/luci-proto-wireguard)
- [Linux 6.12.94 WireGuard driver](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/net/wireguard?h=v6.12.94)

The old running kernel was 6.12.94~5b011ac35e41cd7164f301f8e314c8b0-r1. Enabling the missing modules changed the build's kernel dependency to 6.12.94~d48f5cedc0894cabda641d4947dcee08-r1, so the resulting kmod APKs could not be installed onto the old image. The firmware and modules were built together instead.

The shared APKs are reference build artifacts, collected from our current build output on 2026-10-10. They are not presented as the exact original October 8 APK files. The WireGuard module payload hash still matches the October 8 verification record: dcfa8d1dfbb5d0171f4c83d892154dcc20255c07da99694eac26b54bc82e8796. Package metadata records each current APK's hash and dependencies.

Architecture: aarch64_cortex-a53. Every shared kmod requires kernel=6.12.94~d48f5cedc0894cabda641d4947dcee08-r1. ADCDS's default and NSS release images use different kernel build identifiers; these APKs are not drop-in packages for those images. The APKs are signed by our build key, not ADCDS's. No signature/dependency bypass is suggested.

The bundle includes the VPN packages and associated modules, plus luci-lib-uqr and resolveip. It assumes the normal firmware already supplies libc and ucode. It is not a complete independent package repository. It contains no tunnel configuration or keys, and excludes our peer-control/status UI changes.

For a common installation path, the image and optional modules can be built together, with the matching signed package index served from a persistent release-specific repository configured in LuCI. That lets users install the VPN later without rebuilding their own firmware. ADCDS already documents KMODS=1 release archives; the question is whether that package set could be made available directly through System -> Software.
