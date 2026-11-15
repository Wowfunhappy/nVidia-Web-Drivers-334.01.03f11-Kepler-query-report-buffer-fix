#!/usr/bin/env python3
"""Build the GeForceGLDriverWeb query-buffer patch as Perl find/replace
commands, verify them against a copy of the driver, and write
install.command and uninstall.command.

usage: build_patch.py

Three edits to the x86_64 slice (the i386 slice is not touched):
  1. Remove LC_CODE_SIGNATURE: it is the last load command, so ncmds
     27 -> 26 and sizeofcmds 0xd10 -> 0xd00.  The command's bytes stay in
     place (after the shortened load command area), so this is reversible.
  2. Write querycave.bin into the zero padding immediately after the load
     commands, at slice offset 0xd30 (address 0x123440000d30).  The rest of
     the padding, up to __text at 0xeb0, stays free.
  3. Replace the first 6 bytes of the query-buffer-changed callback
     (0x123440309094: push rbp; mov rbp,rsp; push r15) with a jump to the
     code from step 2 plus a nop.

Each pattern includes enough surrounding bytes to match exactly once in
the stock driver; the script checks that, applies install.command's commands
to a copy, checks the result, applies uninstall.command's commands and checks the
copy is byte-identical to the stock driver again.
"""
import os, shutil, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = '/System/Library/Extensions/GeForceGLDriverWeb.bundle/Contents/MacOS/GeForceGLDriverWeb'
SLICE = 23154688                       # file offset of the x86_64 slice
CAVE_ADDR = 0x123440000d30
CAVE_OFF = 0xd30
HOOK_ADDR = 0x123440309094
HOOK_OFF = 0x309094
RELEASE = 0x1234402f711c
CALLBACK_CONT = HOOK_ADDR + 6


def esc(b):
    return ''.join('\\x%02x' % x for x in b)


def cave_bytes():
    code = bytearray(open(os.path.join(HERE, 'querycave.bin'), 'rb').read())
    for placeholder, target in ((0x11111111, RELEASE), (0x22222222, CALLBACK_CONT)):
        pat = struct.pack('<I', placeholder)
        i = code.find(pat)
        assert i > 0 and code.count(pat) == 1, 'placeholder %#x' % placeholder
        assert code[i - 1] in (0xe8, 0xe9)
        code[i:i + 4] = struct.pack('<i', target - (CAVE_ADDR + i + 4))
    return bytes(code)


def main():
    stock = open(DRIVER, 'rb').read()
    cave = cave_bytes()

    # 1. LC_CODE_SIGNATURE
    hdr_fixed = stock[SLICE:SLICE + 16]
    assert hdr_fixed == bytes.fromhex('cffaedfe070000010300000008000000')
    sig_on = struct.pack('<II', 27, 0xd10)
    sig_off = struct.pack('<II', 26, 0xd00)
    assert stock[SLICE + 16:SLICE + 24] == sig_on
    assert stock[SLICE + 0xd20:SLICE + 0xd30] == bytes.fromhex('1d000000100000001014b801504c0200')

    # 2. cave: anchored on the last two load commands (code signature DRs and
    #    code signature), followed by the zero run the code replaces
    anchor = stock[SLICE + 0xd10:SLICE + 0xd30]
    gap = CAVE_OFF - 0xd30
    zeros = b'\0' * len(cave)
    assert stock[SLICE + 0xd30:SLICE + CAVE_OFF + len(cave)] == b'\0' * (gap + len(cave))
    assert CAVE_OFF + len(cave) <= 0xeb0, 'cave overruns __text'

    # 3. hook
    hook_orig = stock[SLICE + HOOK_OFF:SLICE + HOOK_OFF + 24]
    assert hook_orig[:6] == bytes.fromhex('554889e54157')
    jump = b'\xe9' + struct.pack('<i', CAVE_ADDR - (HOOK_ADDR + 5)) + b'\x90'

    for name, pat in (('header', hdr_fixed + sig_on), ('cave', anchor + b'\0' * gap + zeros), ('hook', hook_orig)):
        n = stock.count(pat)
        print('%-6s pattern matches %d time(s) in the stock driver' % (name, n))
        assert n == 1

    q = '"%s"' % DRIVER
    install = [
        '# Remove LC_CODE_SIGNATURE from the x86_64 slice (only matches if it is present)',
        "sudo perl -0777 -pi -e 's/(%s)%s/$1%s/' %s" % (esc(hdr_fixed), esc(sig_on), esc(sig_off), q),
        '',
        '# Install the query report buffer fix: code in the padding after the load commands',
        "sudo perl -0777 -pi -e 's/(%s%s)%s/$1%s/' %s" % (esc(anchor), esc(b'\0' * gap), '\\x00{%d}' % len(cave), esc(cave), q),
        '',
        '# Install the query report buffer fix: jump from the query-buffer-changed callback to the new code',
        "sudo perl -0777 -pi -e 's/%s(%s)/%s$1/' %s" % (esc(hook_orig[:6]), esc(hook_orig[6:]), esc(jump), q),
    ]
    uninstall = [
        '# Uninstall the query report buffer fix: restore the start of the query-buffer-changed callback',
        "sudo perl -0777 -pi -e 's/%s(%s)/%s$1/' %s" % (esc(jump), esc(hook_orig[6:]), esc(hook_orig[:6]), q),
        '',
        '# Uninstall the query report buffer fix: clear the code from the padding after the load commands',
        "sudo perl -0777 -pi -e 's/(%s%s)%s/$1%s/' %s" % (esc(anchor), esc(b'\0' * gap), esc(cave), esc(zeros), q),
        '',
        '# Restore LC_CODE_SIGNATURE to the x86_64 slice',
        "sudo perl -0777 -pi -e 's/(%s)%s/$1%s/' %s" % (esc(hdr_fixed), esc(sig_off), esc(sig_on), q),
    ]

    # Verify on a copy, running the same commands without sudo.
    tmp = tempfile.mkdtemp()
    copy = os.path.join(tmp, 'GeForceGLDriverWeb')
    shutil.copyfile(DRIVER, copy)

    def run(lines):
        for line in lines:
            if line.startswith('sudo '):
                cmd = line[5:].replace(q, '"%s"' % copy)
                subprocess.run(['/bin/sh', '-c', cmd], check=True)

    run(install)
    patched = open(copy, 'rb').read()
    assert len(patched) == len(stock)
    diff = [i for i in range(len(stock)) if stock[i] != patched[i]]
    regions = []
    for i in diff:
        if regions and i <= regions[-1][1] + 16:
            regions[-1][1] = i
        else:
            regions.append([i, i])
    for a, b in regions:
        print('changed bytes: x86_64 slice offset %#x-%#x' % (a - SLICE, b - SLICE))
    assert patched[SLICE + 16:SLICE + 24] == sig_off
    assert patched[SLICE + CAVE_OFF:SLICE + CAVE_OFF + len(cave)] == cave
    assert patched[SLICE + HOOK_OFF:SLICE + HOOK_OFF + 6] == jump
    assert all(SLICE <= a for a, _ in regions), 'i386 slice changed'
    shutil.copyfile(copy, os.path.join(HERE, 'GeForceGLDriverWeb.patched'))

    run(install)                               # applying twice must change nothing
    assert open(copy, 'rb').read() == patched
    run(uninstall)
    assert open(copy, 'rb').read() == stock, 'uninstall did not restore the stock driver'
    print('install applied to a copy; reinstall is a no-op; uninstall restores the stock driver byte for byte')
    print('free padding after the code: %d bytes (slice offset %#x-%#x)' % (0xeb0 - CAVE_OFF - len(cave), CAVE_OFF + len(cave), 0xeb0))
    shutil.rmtree(tmp)

    for name, lines in (('install.command', install), ('uninstall.command', uninstall)):
        with open(os.path.join(HERE, name), 'w') as f:
            f.write('#!/bin/sh\n# GeForceGLDriverWeb 334.01.03f11: query report buffer fix\n\n')
            f.write('\n'.join(lines) + '\n')
        os.chmod(os.path.join(HERE, name), 0o755)
    print('wrote install.command and uninstall.command; patched copy at GeForceGLDriverWeb.patched')


main()
