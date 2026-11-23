#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""ubootusbsim.py: run U-Boot's own USB stick scan on a PC, against a model stick.

Issue #7, docs/live-image.md "Why the sticks froze". U-Boot 2.5.3.0-T48S stops
dead at "scanning usb for storage devices..." on the real phone. This runs the
very same code (usb_init at 0x03B16BB8, then usb_stor_scan at 0x03B188D0, from
the phone's own U-Boot dump) in the Unicorn CPU emulator, with:

  - a small model of the USB 2.0 host controller (EHCI) and of one stick, which
    answers with whatever is given on the command line (the defaults are the
    answers a SanDisk Ultra Fit really gave, taken with tools/usbreplay.py);
  - the phone's fault rules: any unaligned 16 or 32-bit access, or any access
    outside memory, is reported as "the phone freezes here", because U-Boot runs
    with alignment checking on and has no working exception handlers.

Only U-Boot's printing, delays, clock and memory allocator are replaced; the USB
stack, the storage code and the partition check are the phone's own.

  ubootusbsim.py dumps/mtd1-uboot.bin                 the Ultra Fit's answers
  ubootusbsim.py dumps/mtd1-uboot.bin turfails=3      "not ready" three times
  ubootusbsim.py dumps/mtd1-uboot.bin maxlun=stall    refuses GET MAX LUN
  ubootusbsim.py dumps/mtd1-uboot.bin maxlun=3        a four-unit card reader
  ubootusbsim.py dumps/mtd1-uboot.bin nakin=2         first two answers never come
  ubootusbsim.py dumps/mtd1-uboot.bin dead=1          stops answering altogether
  ubootusbsim.py dumps/mtd1-uboot.bin cap=HEX inq=HEX other READ CAPACITY / INQUIRY data
  ubootusbsim.py dumps/mtd1-uboot.bin garbage=7       random leftovers in stack and heap
  ubootusbsim.py dumps/mtd1-uboot.bin hcs=12          controller reports two ports
  ubootusbsim.py dumps/mtd1-uboot.bin img=FILE        sectors come from a stick image
  ubootusbsim.py dumps/mtd1-uboot.bin nostick=1       nothing in the socket
  ubootusbsim.py selftest                             prove the fault check fires
  add "v" to list the control requests of the first phase as well

Needs: pip install unicorn. The dump is not in the repository (vendor code).

What this cannot show is anything the real controller, its PHY or the power
supply do: the model controller always behaves.
"""
import random
import struct
import sys

from unicorn import (Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_HOOK_MEM_UNMAPPED, UC_MEM_WRITE)
from unicorn.arm_const import (UC_CPU_ARM_1176, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP,
                               UC_ARM_REG_LR, UC_ARM_REG_PC)

BASE = 0x03B00000          # where U-Boot runs
HEADER = 0x40              # the dump's own header before the code
STACK_TOP = 0x03AD0000
HEAP_BASE = 0x03000000
STOP = 0x07F00000          # return address that ends a call
MMIO = 0x80000000
HCCR, HCOR = 0x80410800, 0x80410810      # controller 1, as ehci_hcd_init sets them

# Addresses in U-Boot 2.5.3.0-T48S (dumps/uboot.dis)
F_USB_INIT, F_STOR_SCAN = 0x03B16BB8, 0x03B188D0
F_PRINTF, F_PUTS, F_MEMALIGN, F_FREE = 0x03B102F0, 0x03B10294, 0x03B10F3C, 0x03B106DC
F_MDELAY, F_UDELAY, F_GET_TIMER, F_HCD_INIT = 0x03B1C740, 0x03B1C764, 0x03B202A4, 0x03B24EC4
V_MAX_DEVS, V_DEV_DESC = 0x03B57F00, 0x03B57F04

# The SanDisk Ultra Fit 32 GB (0781:5583) at high speed, as read on proxmox0
DEV_DESC = bytes.fromhex('120110020000004081078355000101020301')
CFG_DESC = bytes.fromhex('090220000101008070' '090400000208065000' '07058102000200' '07050202000200')
INQUIRY = bytes.fromhex('0080061243000000' '53616e4469736b00' '556c74726120466974000000000000' '00' '312e3030')
CAPACITY = bytes.fromhex('03bab05700000200')
SENSE = bytes.fromhex('700006000000000a00000000280000000000')
SECTOR0 = bytes(440) + bytes.fromhex('7ce3fad400000000' '01010c3fe0ff' '00080000' '58a8ba03') + bytes(48) + b'\x55\xaa'


def main():
    args = sys.argv[1:]
    if args == ['selftest']:
        return selftest()
    if not args or args[0] in ('-h', '--help'):
        print(__doc__)
        return 2
    opts = dict(a.split('=', 1) for a in args[1:] if '=' in a)
    sim = Sim(open(args[0], 'rb').read()[HEADER:], opts)
    r = sim.call(F_USB_INIT)
    print('usb_init returned %s after %d ms of U-Boot time' % (r, sim.now_ms))
    if 'v' in args[1:]:
        print('\n'.join(sim.log))
    sim.log.clear()
    if r != 0:
        return 1
    if 'nostick' in opts:
        print('(no stick: U-Boot goes on to the scan all the same)')
    r = sim.call(F_STOR_SCAN, 1)
    print('usb_stor_scan returned %s after %d ms of U-Boot time' % (None if r is None else r - (r >> 31 << 32), sim.now_ms))
    print('\n'.join(sim.log))
    if r is None:
        return 1
    d = V_DEV_DESC
    print('storage devices: %d' % sim.rd32(V_MAX_DEVS))
    print('first: unit %d, type %d, partition table %s, %d sectors of %d bytes, "%s" "%s"' % (
        sim.rd8(d + 10), sim.rd8(d + 11), {2: 'MBR', 0: 'none'}.get(sim.rd8(d + 8), '?'),
        sim.rd32(d + 16), sim.rd32(d + 20), sim.cstr(d + 28), sim.cstr(d + 69)))
    return 0


class Stick:
    """One bulk-only mass-storage stick, answering as the options say."""

    def __init__(self, opts, log):
        self.opts, self.log = opts, log
        self.addr, self.newaddr, self.ctrl_in = 0, None, b''
        self.data, self.csw, self.bulk_ins = b'', None, 0
        self.turfails = int(opts.get('turfails', '0'))
        self.strings = {0: bytes([4, 3, 9, 4])}
        for i, s in ((1, 'SanDisk'), (2, 'Ultra Fit'), (3, '4C531123391017121105')):
            e = s.encode('utf-16le')
            self.strings[i] = bytes([len(e) + 2, 3]) + e

    def setup(self, s):
        rt, rq, val, idx, ln = struct.unpack('<BBHHH', s)
        self.ctrl_in = b''
        self.log.append('  control: type %02x request %02x value %04x index %04x length %d' % (rt, rq, val, idx, ln))
        if (rt, rq) == (0x80, 6):                       # GET DESCRIPTOR
            kind, i = val >> 8, val & 0xff
            d = self.strings.get(i) if kind == 3 else {1: DEV_DESC, 2: CFG_DESC}.get(kind)
            if d is None:
                return 'stall'
            self.ctrl_in = d[:ln]
        elif (rt, rq) == (0x00, 5):                     # SET ADDRESS
            self.newaddr = val
        elif (rt, rq) == (0xA1, 0xFE):                  # GET MAX LUN
            if self.opts.get('maxlun') == 'stall':
                return 'stall'
            self.ctrl_in = bytes([int(self.opts.get('maxlun', '0'))])
        elif (rt, rq) in ((0x00, 9), (0x01, 11), (0x21, 0xFF), (0x02, 1)):
            pass                                        # SET CONFIGURATION / INTERFACE, storage reset, CLEAR HALT
        else:
            self.log.append('  (a request the model does not know: refused)')
            return 'stall'

    def bulk_out(self, d):
        if len(d) != 31:
            self.log.append('  bulk OUT of %d bytes, not a command' % len(d))
            return
        _, tag, dlen, _, lun, clen = struct.unpack('<IIIBBB', d[:15])
        cdb = d[15:15 + clen]
        op, status, data = cdb[0], 0, b''
        if op == 0x12:
            data = bytes.fromhex(self.opts['inq']) if 'inq' in self.opts else INQUIRY
            data = data[:cdb[4]]
        elif op == 0x00:
            if self.turfails:
                self.turfails -= 1
                status = 1
        elif op == 0x03:
            data = SENSE[:cdb[4]]
        elif op == 0x25:
            data = bytes.fromhex(self.opts['cap']) if 'cap' in self.opts else CAPACITY
        elif op == 0x28:
            lba, n = struct.unpack('>I', cdb[2:6])[0], struct.unpack('>H', cdb[7:9])[0]
            if 'img' in self.opts:
                with open(self.opts['img'], 'rb') as f:
                    f.seek(lba * 512)
                    data = f.read(n * 512).ljust(n * 512, b'\0')
            else:
                data = (SECTOR0 if lba == 0 else bytes(512)) + bytes(512 * (n - 1))
        else:
            status = 1
        name = {0x12: 'INQUIRY', 0x00: 'TEST UNIT READY', 0x03: 'REQUEST SENSE', 0x25: 'READ CAPACITY',
                0x28: 'READ(10)'}.get(op, 'command %02x' % op)
        self.log.append('  %s: unit %d, asks %d bytes, command %s -> %d bytes, status %d' % (
            name, lun, dlen, cdb.hex(), min(len(data), dlen), status))
        self.data = data[:dlen]
        self.csw = struct.pack('<IIIB', 0x53425355, tag, dlen - len(self.data), status)

    def bulk_in(self, n):
        self.bulk_ins += 1
        if self.bulk_ins <= int(self.opts.get('nakin', '0')):
            return None
        if 'dead' in self.opts and self.bulk_ins >= int(self.opts['dead']):
            return None
        if self.data:
            d, self.data = self.data[:n], self.data[n:]
            return d
        d, self.csw = self.csw, None
        return d


class Sim:
    def __init__(self, body, opts):
        self.opts, self.log, self.now_ms, self.heap = opts, [], 0, HEAP_BASE
        self.stick = Stick(opts, self.log)
        self.reg = {'cmd': 0x00080000, 'sts': 0x1000, 'async': 0, 'cfg': 0,
                    'portsc': 0x1000 if 'nostick' in opts else 0x1003}
        uc = self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        uc.ctl_set_cpu_model(UC_CPU_ARM_1176)
        uc.mem_map(0, 0x08000000)                       # the phone's 128 MB
        uc.mem_map(MMIO, 0x01000000)
        if body:
            uc.mem_write(BASE, body)
            bss0, bss1 = struct.unpack_from('<II', body, 0x48)
            uc.mem_write(bss0, bytes(bss1 - bss0))      # the dump is padded with FFs where start.S zeroes
        if 'garbage' in opts:
            random.seed(int(opts['garbage']))
            uc.mem_write(STACK_TOP - 0xD0000, random.randbytes(0xD0000))
            uc.mem_write(HEAP_BASE, random.randbytes(0x40000))
        uc.hook_add(UC_HOOK_MEM_READ, self.mmio_read, begin=MMIO, end=MMIO + 0xffffff)
        uc.hook_add(UC_HOOK_MEM_WRITE, self.mmio_write, begin=MMIO, end=MMIO + 0xffffff)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.check_align, begin=0, end=0x07ffffff)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.stray, begin=0, end=HEAP_BASE - 1)
        uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.stray, begin=0x03C00000, end=0x07ffffff)
        uc.hook_add(UC_HOOK_MEM_UNMAPPED, self.unmapped)
        self.hooks = {F_PRINTF: self.h_printf, F_PUTS: self.h_puts, F_MEMALIGN: self.h_memalign,
                      F_FREE: lambda: self.ret(0), F_MDELAY: self.h_mdelay, F_UDELAY: lambda: self.ret(0),
                      F_GET_TIMER: self.h_timer, F_HCD_INIT: self.h_hcd_init}
        for a in self.hooks:
            uc.hook_add(UC_HOOK_CODE, lambda u, addr, size, ud: self.hooks[addr](), begin=a, end=a)

    # memory helpers
    def rd8(self, a): return self.uc.mem_read(a, 1)[0]
    def rd32(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def wr32(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))

    def cstr(self, a):
        s = bytearray()
        while self.rd8(a):
            s.append(self.rd8(a))
            a += 1
        return s.decode('latin1')

    # the phone's fault rules
    def check_align(self, uc, access, a, size, value, ud):
        if size in (2, 4) and a % size:
            print('*** UNALIGNED %d-byte access to %08x at pc=%08x: the phone freezes here' % (
                size, a, uc.reg_read(UC_ARM_REG_PC)))
            uc.emu_stop()

    def stray(self, uc, access, a, size, value, ud):
        print('*** %s of %08x at pc=%08x, outside U-Boot, its stack and its heap' % (
            'write' if access == UC_MEM_WRITE else 'read', a, uc.reg_read(UC_ARM_REG_PC)))

    def unmapped(self, uc, access, a, size, value, ud):
        print('*** access to %08x at pc=%08x, where nothing is: the phone freezes here' % (
            a, uc.reg_read(UC_ARM_REG_PC)))
        return False

    # the model controller: registers
    def mmio_read(self, uc, access, a, size, value, ud):
        off = a - HCOR
        if a == HCCR: v = 0x01000010
        elif a == HCCR + 4: v = int(self.opts.get('hcs', '1'), 16)
        elif a == HCCR + 8: v = int(self.opts.get('hcc', '0'), 16)
        elif off == 0: v = self.reg['cmd']
        elif off == 4: v = self.reg['sts']
        elif off == 0x18: v = self.reg['async']
        elif off == 0x40: v = self.reg['cfg']
        elif off == 0x44: v = self.reg['portsc']
        else: v = 0
        self.wr32(a & ~3, v)

    def mmio_write(self, uc, access, a, size, value, ud):
        off, reg = a - HCOR, self.reg
        if off == 0:
            was, reg['cmd'] = reg['cmd'], value & ~2            # a controller reset finishes at once
            reg['sts'] = (reg['sts'] & ~0x1000) if value & 1 else (reg['sts'] | 0x1000)
            if value & 0x20 and not was & 0x20:                 # async schedule switched on: do the work now
                reg['sts'] |= 0x8000
                self.run_async()
            if not value & 0x20:
                reg['sts'] &= ~0x8000
        elif off == 0x18:
            reg['async'] = value
        elif off == 0x40:
            reg['cfg'] = value
        elif off == 0x44:
            p = reg['portsc'] & ~(value & 0x2a)                 # write 1 to clear the change bits
            if value & 0x100:
                p = (p | 0x100) & ~4                            # port reset begins
            elif p & 0x100:
                p = (p & ~0x100) | 4 | 8                        # reset ends: enabled, at high speed
            reg['portsc'] = p | 0x1000

    # the model controller: one pass over the queue U-Boot built
    def run_async(self):
        stick, head = self.stick, self.reg['async'] & ~31
        qh = self.rd32(head) & ~31
        for _ in range(8):
            if qh == head:
                break
            ep1 = self.rd32(qh + 4)
            addr, ep = ep1 & 0x7f, (ep1 >> 8) & 15
            qtd, last, halted = self.rd32(qh + 16), None, False
            while not qtd & 1:
                qtd &= ~31
                tok = self.rd32(qtd + 8)
                if not tok & 0x80:
                    break
                pid, n, buf = (tok >> 8) & 3, (tok >> 16) & 0x7fff, self.rd32(qtd + 12)
                if addr != stick.addr:
                    self.log.append('  (nothing at address %d: no answer)' % addr)
                    break
                done = n
                if pid == 2:                                    # SETUP
                    halted = stick.setup(bytes(self.uc.mem_read(buf, 8))) == 'stall'
                elif pid == 0:                                  # OUT
                    if ep:
                        stick.bulk_out(bytes(self.uc.mem_read(buf, n)))
                elif ep == 0:                                   # IN, control
                    if not halted:
                        d, stick.ctrl_in = stick.ctrl_in[:n], stick.ctrl_in[n:]
                        self.uc.mem_write(buf, d)
                        done = len(d)
                else:                                           # IN, bulk
                    d = stick.bulk_in(n)
                    if d is None:
                        self.log.append('  (the stick does not answer: NAK until U-Boot gives up)')
                        break
                    self.uc.mem_write(buf, d)
                    done = len(d)
                tok = (tok & ~0x7fff00ff) | ((n - done) << 16)
                if halted:
                    tok |= 0x40
                self.wr32(qtd + 8, tok)
                last = tok
                if halted:
                    break
                if done < n and ep:
                    break
                # A short control answer: U-Boot marks "no alternative", so the
                # controller carries on with the next step (EHCI 1.0, 4.10.2).
                qtd = self.rd32(qtd)
            if stick.newaddr is not None and last is not None and not last & 0x40:
                stick.addr, stick.newaddr = stick.newaddr, None
            if last is not None:
                self.wr32(qh + 24, last)
            qh = self.rd32(qh) & ~31

    # U-Boot functions replaced
    def ret(self, v=None):
        if v is not None:
            self.uc.reg_write(UC_ARM_REG_R0, v & 0xffffffff)
        self.uc.reg_write(UC_ARM_REG_PC, self.uc.reg_read(UC_ARM_REG_LR))

    def h_printf(self):
        uc = self.uc
        fmt = self.cstr(uc.reg_read(UC_ARM_REG_R0))
        regs = [uc.reg_read(r) for r in (UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        sp, used = uc.reg_read(UC_ARM_REG_SP), [0]

        def arg():
            i = used[0]
            used[0] += 1
            return regs[i] if i < 3 else self.rd32(sp + 4 * (i - 3))
        out, i = '', 0
        while i < len(fmt):
            if fmt[i] != '%':
                out += fmt[i]
                i += 1
                continue
            j = i + 1
            while fmt[j] in '#0123456789-lh.':
                j += 1
            k = fmt[j]
            if k == '%': out += '%'
            elif k == 's': out += self.cstr(arg())
            elif k in 'di':
                v = arg()
                out += str(v - (1 << 32) if v & 0x80000000 else v)
            elif k == 'u': out += str(arg())
            elif k == 'c': out += chr(arg() & 0xff)
            else: out += '%x' % arg()
            i = j + 1
        print('U-Boot: ' + out.rstrip('\n').replace('\r', ''))
        self.ret(0)

    def h_puts(self):
        print('U-Boot: ' + self.cstr(self.uc.reg_read(UC_ARM_REG_R0)).rstrip('\n').replace('\r', ''))
        self.ret(0)

    def h_memalign(self):
        al, size = self.uc.reg_read(UC_ARM_REG_R0), self.uc.reg_read(UC_ARM_REG_R1)
        p = (self.heap + al - 1) & ~(al - 1)
        self.heap = p + size + 64
        self.ret(p)

    def h_mdelay(self):
        self.now_ms += self.uc.reg_read(UC_ARM_REG_R0)
        self.ret(0)

    def h_timer(self):
        self.now_ms += 1
        self.ret(self.now_ms)

    def h_hcd_init(self):
        if self.uc.reg_read(UC_ARM_REG_R0) == 0:
            return self.ret(-19)                        # controller 0: "Port not available."
        self.wr32(self.uc.reg_read(UC_ARM_REG_R2), HCCR)
        self.wr32(self.uc.reg_read(UC_ARM_REG_R3), HCOR)
        self.ret(0)

    def call(self, addr, *args, limit=2_000_000_000):
        uc = self.uc
        for r, v in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3), args):
            uc.reg_write(r, v)
        uc.reg_write(UC_ARM_REG_LR, STOP)
        uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        try:
            uc.emu_start(addr, STOP, count=limit)
        except UcError as e:
            print('*** stopped: %s at pc=%08x' % (e, uc.reg_read(UC_ARM_REG_PC)))
            return None
        if uc.reg_read(UC_ARM_REG_PC) != STOP:
            print('*** did not return: pc=%08x (a fault, or it never ends)' % uc.reg_read(UC_ARM_REG_PC))
            return None
        return uc.reg_read(UC_ARM_REG_R0)


def selftest():
    """The fault check must fire on an unaligned load, or a clean run proves nothing."""
    sim = Sim(b'', {})
    code = struct.pack('<II', 0xE5910000, 0xE12FFF1E)   # ldr r0, [r1] ; bx lr
    sim.uc.mem_write(0x03B00000, code)
    ok = sim.call(0x03B00000, 0, 0x03A00000) is not None
    print('aligned load: %s' % ('ran' if ok else 'FAILED'))
    print('unaligned load, expecting a report:')
    bad = sim.call(0x03B00000, 0, 0x03A00001) is None
    print('selftest %s' % ('PASS' if ok and bad else 'FAIL'))
    return 0 if ok and bad else 1


if __name__ == '__main__':
    sys.exit(main())
