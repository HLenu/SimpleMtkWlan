<!--
  README.md
  SimpleMtkWlan

  Created by laobamac (王孝慈) on 2026/5/5.
  Copyright © 2026 laobamac (王孝慈). All rights reserved.
-->

# SimpleMtkWlan

SimpleMtkWlan is an experimental macOS kernel extension for MediaTek MT792x PCIe
wireless adapters, based on OpenBSD's `if_mwx` driver and net80211.

Original author: laobamac (王孝慈). This fork (pengubow) brings it up on an
MT7922 (AMD RZ616) in an ASUS ROG Strix G17 G713PV, using Linux `mt7921e` as the
reference for the firmware command sequence.

## Status

Working on the MT7922 / RZ616 (`14c3:0616`), tested in the macOS 15 installer:

- firmware load, hardware scan, scanning while connected
- WPA2-PSK (CCMP) with hardware encryption/decryption, DHCP
- 802.11n/ac/ax with 40/80 MHz channels, 2 spatial streams, A-MPDU (Block Ack)
  and A-MSDU; download speed matches Linux on the same connection
- clean shutdown: the radio is stopped and the AP deauthenticated, so the next
  OS (e.g. Linux) can bring the card up normally

Still experimental. Known gaps:

- no native Wi-Fi menu with this kext: use HeliPort or the `mtkwifi` command
  (below), or try the experimental `AirportMtkWlan.kext`
- no roaming on beacon loss yet; walking out of range needs a manual rejoin
- no 6 GHz, no 160 MHz

## Supported Hardware

- MediaTek MT7921, PCI ID `14c3:7961`
- MediaTek MT7921K/RZ608, PCI ID `14c3:0608`
- MediaTek MT7922, PCI ID `14c3:7922`
- MediaTek MT7922/RZ616, PCI ID `14c3:0616` (the only one tested by this fork)

## Usage

The kext loads from a local recovery/installer too (`OSBundleRequired` is
`Root`). Its IOClass is `itlwm` and it speaks HeliPort's ClientKit ioctls, so
[HeliPort](https://github.com/OpenIntelWireless/HeliPort) should be able to
control it (not tested yet).

`mtkwifi` is a small command line tool (built next to the kext) that works in
the installer's Terminal:

```sh
mtkwifi                       # status: network, channel/width, signal, rate
mtkwifi scan                  # list networks
mtkwifi join "SSID" password  # connect, then DHCP
mtkwifi disconnect
mtkwifi power on|off
mtkwifi phy [a|n|ac|ax]       # show or change the highest Wi-Fi mode (reconnects)
```

Boot arguments:

- `-mtkwlanoff` - do not load
- `mtkphy=a|n|ac|ax` - highest Wi-Fi mode offered to access points (default `ax`)
- `-novht`, `-noht40` - disable 802.11ac / 40 MHz

### Diagnostics

The driver keeps a 64 KB log ring and publishes it, with its attach stage and
version, on the PCI device:

```sh
ioreg -l -w0 | grep -E 'SimpleMtkWlan(Log|Stage|Error|Version)'
```

## AirportMtkWlan (experimental): the native Wi-Fi menu

`AirportMtkWlan.kext` is the same MediaTek driver behind macOS's own Wi-Fi
interface (IO80211/Skywalk), so the Wi-Fi menu, System Settings and the
installer/Recovery Wi-Fi work without HeliPort. It is laobamac's
[AirportItlwm](https://github.com/laobamac/itlwm) Sequoia (WCL) layer on this
repository's HAL and net80211; the upstream commit is in `AirportMtkWlan/UPSTREAM`.

- macOS 15.2 to 15.x only (Darwin 24.2+); in OpenCore set MinKernel 24.2.0,
  MaxKernel 24.99.99
- use it **instead of** SimpleMtkWlan (both match the card)
- 802.11ax is opt-in with `itlwm_he=1`, as upstream; `mtkphy=` still applies
- upstream limits apply: no AirDrop/AWDL, no WPA3-only or mandatory-PMF networks
- logs: `log show --last boot --predicate 'eventMessage CONTAINS "AirportMtkWlan"'`

Working on the G713PV (MT7922/RZ616, macOS 15.8.1): Wi-Fi menu, scanning
(~1.2 s per macOS scan request), WPA2-Personal join with macOS's private Wi-Fi
address, DHCP; 802.11ac 80 MHz 2x2, about 83 Mbps down / 44 Mbps up
(networkQuality). Not yet tried: sleep/wake, Recovery/installer, `itlwm_he=1`.

Two macOS behaviours this kext works around:

- `IO80211SkywalkInterface::setInterfaceRole()` never manages to store the
  `IO80211InterfaceRole` property here, and airportd (Apple80211's
  `_isInfraInterface`) ignores an interface without it, so the kext sets it
- macOS scans a few channels per request; net80211 must not keep sweeping on its
  own, or every request waits for a full sweep

## Building

### On Linux (cross build, used for all testing in this fork)

```sh
./build-linux.sh [Debug|Release]   # -> build/<config>/SimpleMtkWlan.kext and mtkwifi
```

Requirements:

- Apple clang 13 and ld64 in `$TOOLCHAIN/bin` (`clang`, `clang++`, `ld`,
  `otool`, `nm`). Newer clang lowers static destructors in kexts to
  `__cxa_atexit`, which the kernel does not export.
- [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) in `$MACKERNELSDK`
- for `mtkwifi`: a macOS SDK in `$MACOS_SDK` and a clang that targets macOS in
  `$CLANG_USER`

Defaults assume the repository sits in `src/` of a larger tree with
`src/deps/MacKernelSDK` and `toolchain/` two levels up; set `TOOLCHAIN`,
`MACKERNELSDK`, `CLANG`, `CLANGXX`, `LD64`, `MACOS_SDK` and `CLANG_USER` to
override. `MODULE_VERSION` sets the kext version.

### On macOS

```sh
./build-macos.sh [Debug|Release] [SimpleMtkWlan|AirportMtkWlan|all]
```

Uses Xcode's toolchain (`xcrun`) and MacKernelSDK in `$MACKERNELSDK` (default
`../deps/MacKernelSDK`). The Xcode project is older:

```sh
xcodebuild -project SimpleMtkWlan.xcodeproj -scheme SimpleMtkWlan -configuration Debug build
```

The Xcode project has not been updated for this fork's new sources
(`SimpleMtkWlan/MtkLog.cpp`, `mtkwifi/`); add them to the target before
building with Xcode. `build-linux.sh` lists every source file.

## Repository Layout

- `SimpleMtkWlan/` - kernel extension entry point, user client and log ring
- `SimpleMtkWlan/hal_mwx/` - MT7921/MT7922 driver implementation
- `mtk80211/` - compatibility layer, OpenBSD net80211 and support code
- `include/` - shared driver interfaces, HeliPort ClientKit ioctls and the
  embedded firmware table
- `mtkwifi/` - command line control tool
- `AirportMtkWlan/` - native Wi-Fi interface (from laobamac's AirportItlwm, Sequoia)
- `include/Airport/` - IO80211/Skywalk headers for it
- `build-linux.sh` - Linux cross build; `build-macos.sh` - macOS build of both kexts

## License

SimpleMtkWlan is distributed under GPLv2. See `LICENSE`.

Some source files include additional or more specific license notices inherited
from upstream GPL/BSD/OpenBSD/NetBSD/FreeBSD components. Those notices are
preserved in place and apply to their respective files.

## Credits

See `CREDITS.md`.
