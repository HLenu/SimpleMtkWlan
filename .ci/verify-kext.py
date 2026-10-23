#!/usr/bin/env python3
"""Static acceptance only. No hardware/Recovery loading claim.
Usage: verify-kext.py KEXT MTKWIFI --source-root PINNED_CHECKOUT [--archive TAR]
Requires SHA256SUMS.txt and SOURCE-REVISION.txt in KEXT's parent directory.
Only thin generic x86_64 is accepted, as explicitly built by build-macos.sh.
"""
import argparse
import hashlib
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import tarfile

SOURCE = '01365ae048dd96839316000285ef941b93a5e59b'
SDK = '3f750085caa17ec3a7880f11c11bf4f48cd6a164'
FIRMWARE = ('mwx_mt7922_patch_mcu_1_1_hdr', 'mwx_mt7922_ram_code_1')


def require(condition, message):
    if not condition:
        raise ValueError(message)
    print('OK   ' + message)


def macho(data, filetype, label):
    require(len(data) >= 32 and data[:4] == b'\xcf\xfa\xed\xfe', label + ': thin LE Mach-O 64')
    _, cpu, subtype, kind, count, size, _, _ = struct.unpack_from('<8I', data)
    require(cpu == 0x01000007 and subtype & 0xffffff == 3, label + ': generic x86_64')
    require(kind == filetype, label + ': filetype ' + hex(filetype))
    require(count > 0 and 32 + size <= len(data), label + ': bounded load commands')
    pos, symbols, executable_segment = 32, set(), False
    for _ in range(count):
        if pos + 8 > 32 + size:
            raise ValueError('load-command header out of bounds')
        cmd, length = struct.unpack_from('<2I', data, pos)
        if length < 8 or length % 8 or pos + length > 32 + size:
            raise ValueError('invalid load command')
        if cmd == 0x19:
            if length < 72:
                raise ValueError('truncated LC_SEGMENT_64')
            offset, filesize = struct.unpack_from('<2Q', data, pos + 40)
            if offset + filesize > len(data):
                raise ValueError('segment bytes out of bounds')
            executable_segment |= bool(struct.unpack_from('<I', data, pos + 60)[0] & 4)
        if cmd == 2:
            if length < 24:
                raise ValueError('truncated LC_SYMTAB')
            so, number, st, ss = struct.unpack_from('<4I', data, pos + 8)
            if so + number * 16 > len(data) or st + ss > len(data):
                raise ValueError('symbol table out of bounds')
            for n in range(number):
                ix, typ, section, _, _ = struct.unpack_from('<IBBHQ', data, so + n * 16)
                if ix >= ss:
                    raise ValueError('symbol string index out of bounds')
                end = data.find(b'\0', st + ix, st + ss)
                if end < 0:
                    raise ValueError('unterminated symbol')
                if typ & 0x0e == 0x0e and section:
                    symbols.add(data[st + ix:end])
        pos += length
    require(pos == 32 + size and executable_segment, label + ': complete commands and executable segment')
    return symbols


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args])


def verify(kext, cli, source, archive=None):
    root = kext.parent
    require(kext.is_dir(), 'kext directory exists')
    info = plistlib.loads((kext / 'Contents/Info.plist').read_bytes())
    require(isinstance(info, dict), 'Info.plist parses')
    require(info.get('CFBundleExecutable') == 'SimpleMtkWlan', 'CFBundleExecutable == SimpleMtkWlan')
    require(info.get('CFBundlePackageType') == 'KEXT' and info.get('CFBundleIdentifier') == 'com.laobamac.SimpleMtkWlan', 'kext bundle identity/type')
    binary = kext / 'Contents/MacOS/SimpleMtkWlan'
    data = binary.read_bytes()
    syms = macho(data, 0x0b, 'kext')
    require({b'_kmod_info', b'__realmain', b'__antimain'} <= syms, 'defined kmod_info and lifecycle symbols')
    require(any(b'mwx_attach' in s for s in syms), 'compiled mwx_attach initialization symbol')
    require(any('0x061614c3' in p.get('IOPCIPrimaryMatch', '').lower().split() and p.get('IOProviderClass') == 'IOPCIDevice' for p in info.get('IOKitPersonalities', {}).values()), 'PCI personality matches 14C3:0616')
    require(info.get('OSBundleRequired') == 'Root', 'OSBundleRequired == Root (static prerequisite)')
    require(git(source, 'rev-parse', 'HEAD').decode().strip() == SOURCE, 'source checkout HEAD == expected commit')
    require(not git(source, 'status', '--porcelain', '--untracked-files=no').strip(), 'source tracked files unchanged')
    src = git(source, 'show', SOURCE + ':SimpleMtkWlan/hal_mwx/MtkMwx.cpp').decode()
    branch = r'if\s*\(PCI_PRODUCT\(pa->pa_id\) == PCI_PRODUCT_MEDIATEK_MT7922\s*\|\|\s*PCI_PRODUCT\(pa->pa_id\) == PCI_PRODUCT_MEDIATEK_MT7922_RZ616\)\s*sc->sc_hwtype = MWX_HW_MT7922;'
    require(bool(re.search(r'#define\s+PCI_PRODUCT_MEDIATEK_MT7922_RZ616\s+0x0616', src)) and '{ PCI_VENDOR_MEDIATEK, PCI_PRODUCT_MEDIATEK_MT7922_RZ616 }' in src and bool(re.search(branch, src)), 'pinned source: RZ616 device table and MT7922 initialization branch')
    fw = git(source, 'show', SOURCE + ':include/FwBinary.cpp').decode()
    for name in FIRMWARE:
        match = re.search(r'unsigned char\s+' + name + r'\[\]\s*=\s*\{(.*?)\};\s*unsigned int\s+' + name + r'_len\s*=\s*(\d+)', fw, re.S)
        require(match is not None, name + ': source array exists')
        blob = bytes(int(h, 16) for h in re.findall(r'0x([0-9a-fA-F]{2})', match[1]))
        require(len(blob) == int(match[2]) and len(blob) > 1000, name + ': firmware length agrees')
        descriptor = name.replace('mwx_', 'mwx-').replace('mt7922_', 'mt7922-')
        require(descriptor.encode() in data, name + ': descriptor embedded')
        require(blob in data, name + ': full pinned firmware embedded; sha256=' + hashlib.sha256(blob).hexdigest())
    require(SOURCE[:7].encode() in data, 'kext embeds pinned source short revision')
    macho(cli.read_bytes(), 2, 'mtkwifi')
    if os.name != 'nt':
        require(os.access(binary, os.X_OK) and os.access(cli, os.X_OK), 'kext and mtkwifi executable permissions')
    if archive:
        with tarfile.open(archive) as tf:
            modes = {m.name.removeprefix('./'): m.mode for m in tf.getmembers() if m.isfile()}
        require(all(modes.get(n, 0) & 0o111 for n in ('mtkwifi', 'SimpleMtkWlan.kext/Contents/MacOS/SimpleMtkWlan')), 'archive preserves executable permissions')
    revision = (root / 'SOURCE-REVISION.txt').read_text()
    require('commit ' + SOURCE in revision.splitlines() and 'MacKernelSDK ' + SDK in revision.splitlines(), 'source and SDK provenance match expected pins')
    expected = {p.relative_to(root).as_posix() for p in kext.rglob('*') if p.is_file()} | {'mtkwifi', 'SOURCE-REVISION.txt'}
    actual = {}
    for line in (root / 'SHA256SUMS.txt').read_text().splitlines():
        m = re.fullmatch(r'([a-f0-9]{64})  (.+)', line)
        if not m or m[2] in actual or m[2] not in expected:
            raise ValueError('invalid/duplicate/unexpected checksum entry: ' + line)
        actual[m[2]] = m[1]
    require(set(actual) == expected, 'manifest covers every required payload file')
    for name, digest in sorted(actual.items()):
        require(hashlib.sha256((root / name).read_bytes()).hexdigest() == digest, 'SHA256 ' + name)
    files = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
    require(files == expected | {'SHA256SUMS.txt'}, 'payload contains only required files')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kext', type=Path)
    parser.add_argument('mtkwifi', type=Path)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--archive', type=Path)
    a = parser.parse_args()
    try:
        require(a.mtkwifi.resolve() == (a.kext.parent / 'mtkwifi').resolve(), 'mtkwifi belongs to same artifact')
        verify(a.kext, a.mtkwifi, a.source_root, a.archive)
    except (OSError, ValueError, KeyError, TypeError, struct.error, subprocess.CalledProcessError, tarfile.TarError) as e:
        print('FAIL ' + str(e))
        print('RESULT: CHECKS FAILED')
        return 1
    print('RESULT: ALL STATIC CHECKS PASSED; hardware/Recovery not tested')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
