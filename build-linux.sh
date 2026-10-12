#!/usr/bin/env bash
# Cross-builds SimpleMtkWlan.kext on Linux, mirroring SimpleMtkWlan.xcodeproj.
# usage: ./build-linux.sh [Debug|Release]
set -euo pipefail

config="${1:-Debug}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
version="${MODULE_VERSION:-1.0.20}"
commit="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)"

# Defaults match the hackintoshstuff layout (repo in src/); override for a standalone checkout.
kernelsdk="${MACKERNELSDK:-$root/src/deps/MacKernelSDK}"
toolchain="${TOOLCHAIN:-$root/toolchain}"
sdkhdr="$kernelsdk/Headers"
libkmod="$kernelsdk/Library/x86_64/libkmod.a"
clangxx="${CLANGXX:-$toolchain/bin/clang++}"
clang="${CLANG:-$toolchain/bin/clang}"
ld64="${LD64:-$toolchain/bin/ld}"
otool="$toolchain/bin/otool"
nm_tool="$toolchain/bin/nm"

case "$config" in
    Debug)   opt=(-O0 -DDEBUG=1) ;;
    Release) opt=(-O1) ;;
    *) echo "usage: $0 [Debug|Release]" >&2; exit 2 ;;
esac

out="$here/build/$config"
obj="$out/obj"
bundle="$out/SimpleMtkWlan.kext"
rm -rf "$out"
mkdir -p "$obj" "$bundle/Contents/MacOS"

# Xcode compiles every source (including .c) as C++ (GCC_INPUT_FILETYPE = sourcecode.cpp.cpp)
# and resolves project headers through header maps, so every header directory goes on the path.
sources=(
    include/FwBinary.cpp include/HAL/MtkHalService.cpp mtk80211/compat.cpp mtk80211/zutil.c
    mtk80211/openbsd/crypto/aes.c mtk80211/openbsd/crypto/arc4.c mtk80211/openbsd/crypto/blf.c
    mtk80211/openbsd/crypto/cast.c mtk80211/openbsd/crypto/chachapoly.c mtk80211/openbsd/crypto/cmac.c
    mtk80211/openbsd/crypto/ecb3_enc.c mtk80211/openbsd/crypto/ecb_enc.c mtk80211/openbsd/crypto/gmac.c
    mtk80211/openbsd/crypto/hmac.c mtk80211/openbsd/crypto/idgen.c mtk80211/openbsd/crypto/key_wrap.c
    mtk80211/openbsd/crypto/md5.c mtk80211/openbsd/crypto/michael.c mtk80211/openbsd/crypto/poly1305.c
    mtk80211/openbsd/crypto/rijndael.c mtk80211/openbsd/crypto/rmd160.c mtk80211/openbsd/crypto/set_key.c
    mtk80211/openbsd/crypto/sha1.c mtk80211/openbsd/crypto/sha1-pbkdf2.c mtk80211/openbsd/crypto/sha2.c
    mtk80211/openbsd/net80211/CTimeout.cpp mtk80211/openbsd/net80211/ieee80211_amrr.c
    mtk80211/openbsd/net80211/ieee80211.c mtk80211/openbsd/net80211/ieee80211_crypto_bip.c
    mtk80211/openbsd/net80211/ieee80211_crypto.c mtk80211/openbsd/net80211/ieee80211_crypto_ccmp.c
    mtk80211/openbsd/net80211/ieee80211_crypto_tkip.c mtk80211/openbsd/net80211/ieee80211_crypto_wep.c
    mtk80211/openbsd/net80211/ieee80211_input.c mtk80211/openbsd/net80211/ieee80211_ioctl.c
    mtk80211/openbsd/net80211/ieee80211_mira.c mtk80211/openbsd/net80211/ieee80211_node.c
    mtk80211/openbsd/net80211/ieee80211_output.c mtk80211/openbsd/net80211/ieee80211_pae_input.c
    mtk80211/openbsd/net80211/ieee80211_pae_output.c mtk80211/openbsd/net80211/ieee80211_proto.c
    mtk80211/openbsd/net80211/ieee80211_ra.c mtk80211/openbsd/net80211/ieee80211_regdomain.c
    mtk80211/openbsd/net80211/ieee80211_rssadapt.c mtk80211/openbsd/net80211/_string.c
    mtk80211/openbsd/net80211/timeout.c mtk80211/openbsd/sys/arp.c mtk80211/openbsd/sys/_ifq.cpp
    mtk80211/openbsd/sys/_mbuf.cpp mtk80211/openbsd/sys/_task.cpp
    SimpleMtkWlan/hal_mwx/MtkMwx.cpp SimpleMtkWlan/MtkWlanUserClient.cpp SimpleMtkWlan/pm.cpp
    SimpleMtkWlan/SimpleMtkWlan.cpp SimpleMtkWlan/MtkLog.cpp
)

incs=(-I "$sdkhdr")
for d in mtk80211/openbsd mtk80211 include; do incs+=(-isystem "$here/$d"); done
while IFS= read -r d; do incs+=(-I "$d"); done < <(find "$here/SimpleMtkWlan" "$here/include" "$here/mtk80211" -type d)

flags=(
    -target x86_64-apple-macos10.12
    -x c++ -std=gnu++14
    -mkernel -fapple-kext -fno-rtti -fno-exceptions -fno-builtin -fno-common -nostdlib
    -fno-use-cxa-atexit -fno-asynchronous-unwind-tables
    -DKERNEL -DIEEE80211_STA_ONLY -D__PRIVATE_SPI__
    "-DSMTW_VERSION=\"$version\"" "-DGIT_COMMIT=\"$commit\""
    -include "$here/SimpleMtkWlan/PrivateSPI.pch"
    -Wno-deprecated-register -Wno-everything
    # A non-void function without a return compiles to ud2 at -O0 (a kernel panic), so make it a build error.
    -Wsystem-headers -Werror=return-type
)

objects=()
for src in "${sources[@]}"; do
    o="$obj/$(echo "$src" | tr '/' '_').o"
    "$clangxx" "${flags[@]}" "${opt[@]}" "${incs[@]}" -c "$here/$src" -o "$o" || { echo "FAILED: $src" >&2; exit 1; }
    objects+=("$o")
done
echo "compiled ${#objects[@]} sources"

# IOKit-only kext: no kmod start/stop routines (Xcode's default MODULE_START = 0).
cat > "$obj/kmod_info.c" <<EOF
#include <mach/kmod.h>
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) kmod_start_func_t *_realmain = 0;
__attribute__((visibility("default"))) kmod_stop_func_t *_antimain = 0;
__attribute__((visibility("default"))) kmod_info_t kmod_info = {
    0, KMOD_INFO_VERSION, -1U, "com.laobamac.SimpleMtkWlan", "$version", -1, 0, 0, 0, 0, _start, _stop,
};
EOF
"$clang" -target x86_64-apple-macos10.12 -mkernel -fno-builtin -fno-common -nostdlib -DKERNEL \
    -I "$sdkhdr" -c "$obj/kmod_info.c" -o "$obj/kmod_info.o"
objects+=("$obj/kmod_info.o")

"$ld64" -arch x86_64 -kext -static -macosx_version_min 10.12 -undefined dynamic_lookup \
    -o "$bundle/Contents/MacOS/SimpleMtkWlan" "${objects[@]}" "$libkmod"

python3 -I - "$here/SimpleMtkWlan/Info.plist" "$bundle/Contents/Info.plist" "$version" <<'PY'
import sys
src, dst, ver = sys.argv[1:4]
s = open(src).read()
for k, v in {"$(DEVELOPMENT_LANGUAGE)": "en", "$(EXECUTABLE_NAME)": "SimpleMtkWlan",
             "$(PRODUCT_BUNDLE_IDENTIFIER)": "com.laobamac.SimpleMtkWlan", "$(PRODUCT_NAME)": "SimpleMtkWlan",
             "$(PRODUCT_BUNDLE_PACKAGE_TYPE)": "KEXT", "$(MODULE_VERSION)": ver}.items():
    s = s.replace(k, v)
open(dst, "w").write(s)
PY

"$otool" -hv "$bundle/Contents/MacOS/SimpleMtkWlan" | grep -q KEXTBUNDLE || { echo "not a kext bundle" >&2; exit 1; }
for sym in _kmod_info __realmain __antimain; do
    "$nm_tool" "$bundle/Contents/MacOS/SimpleMtkWlan" | grep -q " $sym\$" || { echo "missing $sym" >&2; exit 1; }
done
echo "built $bundle"

# mtkwifi: command line Wi-Fi control (also usable in the installer's Terminal)
sdk="${MACOS_SDK:-$HOME/.swiftpm/swift-sdks/darwin.artifactbundle/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk}"
clang_user="${CLANG_USER:-$HOME/.local/share/swiftly/bin/clang}"
"$clang_user" -target x86_64-apple-macos11 -isysroot "$sdk" -O2 -Wall -fuse-ld="$ld64" \
    -framework IOKit -framework CoreFoundation -o "$out/mtkwifi" "$here/mtkwifi/mtkwifi.c"
[ -x "$out/mtkwifi" ] || { echo "mtkwifi build failed" >&2; exit 1; }
echo "built $out/mtkwifi"
