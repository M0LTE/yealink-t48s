#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""usbreplay.py: ask a USB stick what U-Boot asks it, and show every answer.

Issue #7, docs/live-image.md "Why the sticks froze". Sends, in U-Boot
2.5.3.0-T48S's order and with its exact lengths: GET MAX LUN, INQUIRY (36
bytes), TEST UNIT READY (with REQUEST SENSE after a refusal, up to 11 tries),
READ CAPACITY (8 bytes) and a read of sector 0, each as a 12-byte command in a
bulk-only wrapper, which is how U-Boot's storage scan talks to a stick.

READS ONLY: nothing is written to the stick. The kernel's storage driver is
detached from the stick for the few milliseconds this takes, then put back.

  sudo usbreplay.py 0781:5583            (vendor:product from lsusb)
  sudo usbreplay.py 0781:5583 --reset    reset the stick's port first

Needs python3-usb (pyusb). Run on a PC with the stick plugged in and not mounted.
"""
import struct
import sys
import time

import usb.core
import usb.util

TIMEOUT_MS = 5000


def hexs(b):
    return ' '.join('%02x' % x for x in b)


def main():
    if len(sys.argv) < 2 or ':' not in sys.argv[1]:
        print(__doc__)
        return 2
    vid, pid = (int(x, 16) for x in sys.argv[1].split(':'))
    dev = usb.core.find(idVendor=vid, idProduct=pid)
    if dev is None:
        print('no such device')
        return 1
    intf = dev.get_active_configuration()[(0, 0)]
    ep_in = next(e.bEndpointAddress for e in intf if e.bEndpointAddress & 0x80 and e.bmAttributes & 3 == 2)
    ep_out = next(e.bEndpointAddress for e in intf if not e.bEndpointAddress & 0x80 and e.bmAttributes & 3 == 2)
    print('bulk endpoints: in %02x, out %02x' % (ep_in, ep_out))
    reattach = dev.is_kernel_driver_active(0)
    if reattach:
        dev.detach_kernel_driver(0)
    tag = [0]

    def command(name, cdb, datalen):
        cbw = struct.pack('<IIIBBB', 0x43425355, tag[0], datalen, 0x80, 0, 12) + bytes(cdb).ljust(16, b'\0')
        tag[0] += 1
        t0 = time.time()
        dev.write(ep_out, cbw, TIMEOUT_MS)
        data = b''
        if datalen:
            try:
                data = bytes(dev.read(ep_in, datalen, TIMEOUT_MS))
            except usb.core.USBError as e:
                print('  %s: data stage failed (%s); clearing the endpoint' % (name, e))
                dev.clear_halt(ep_in)
        try:
            csw = bytes(dev.read(ep_in, 13, TIMEOUT_MS))
        except usb.core.USBError as e:
            print('  %s: status stage failed (%s); clearing the endpoint and asking again' % (name, e))
            dev.clear_halt(ep_in)
            csw = bytes(dev.read(ep_in, 13, TIMEOUT_MS))
        sig, rtag, residue, status = struct.unpack('<IIIB', csw)
        print('%-16s %5.1f ms  data %d of %d bytes: %s' % (name, (time.time() - t0) * 1000, len(data), datalen, hexs(data[:36])))
        print('%-16s status block %d bytes: signature %s, tag %s, residue %d, status %d' % (
            '', len(csw), 'ok' if sig == 0x53425355 else 'BAD %08x' % sig,
            'ok' if rtag == tag[0] - 1 else 'BAD %d' % rtag, residue, status))
        return data, status

    try:
        if '--reset' in sys.argv:
            dev.reset()
            time.sleep(0.2)
        usb.util.claim_interface(dev, 0)
        try:
            r = dev.ctrl_transfer(0xA1, 0xFE, 0, 0, 1, TIMEOUT_MS)
            print('GET MAX LUN      -> %s (%d unit%s)' % (hexs(r), r[0] + 1, '' if r[0] == 0 else 's'))
        except usb.core.USBError as e:
            print('GET MAX LUN      refused (%s): U-Boot waits 1 s, prints a timeout and assumes one unit' % e)
        command('INQUIRY', [0x12, 0, 0, 0, 36, 0], 36)
        for _ in range(11):
            _, status = command('TEST UNIT READY', [0, 0, 0, 0, 0, 0], 0)
            if status == 0:
                break
            command('REQUEST SENSE', [3, 0, 0, 0, 18, 0], 18)
            time.sleep(0.1)
        data, _ = command('READ CAPACITY', [0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0], 8)
        if len(data) == 8:
            last, size = struct.unpack('>II', data)
            print('%-16s %d sectors of %d bytes' % ('', last + 1, size))
        data, _ = command('READ sector 0', [0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0], 512)
        if len(data) == 512:
            print('%-16s partition 1 entry: %s, signature %s' % ('', hexs(data[446:462]), hexs(data[510:512])))
    finally:
        usb.util.release_interface(dev, 0)
        if reattach:
            dev.attach_kernel_driver(0)
    return 0


if __name__ == '__main__':
    sys.exit(main())
