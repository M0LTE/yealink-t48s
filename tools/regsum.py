#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""regsum.py IMAGE KALLSYMS START STOP: list the hardware register reads and writes
each function makes in a raw ARM kernel image (crude constant tracking, straight-line
only: read the disassembly for the conditions). Used for docs/audio.md, for example
    tools/regsum.py dumps/vmlinux-image.bin dumps/audio/kallsyms.txt c003c394 c003fa84
IMAGE is Yealink's decompressed kernel (loaded at 0xC0008000), KALLSYMS a copy of its
/proc/kallsyms. Addresses printed are the kernel's virtual ones: 0xF36xxxxx is
physical 0x306xxxxx, 0xF80xxxxx is 0x800xxxxx."""
import sys,re,struct,subprocess
IMG=sys.argv[1]
img=open(IMG,'rb').read()
BASE=0xc0008000
syms={}
for l in open(sys.argv[2]):
    p=l.split()
    if len(p)>=3 and '[' not in l: syms.setdefault(int(p[0],16),p[2])
def word(a): return struct.unpack_from('<I',img,a-BASE)[0]
def run(start,stop):
    out=subprocess.run(['arm-linux-gnueabihf-objdump','-D','-b','binary','-marm','--adjust-vma=%#x'%BASE,'--start-address=%#x'%start,'--stop-address=%#x'%stop,IMG],capture_output=True,text=True).stdout
    reg={}
    for l in out.splitlines():
        m=re.match(r'\s*([0-9a-f]+):\s+[0-9a-f]+\s+(\S+)\s*(.*)',l)
        if not m: continue
        a=int(m.group(1),16); op=m.group(2); args=m.group(3).split('@')[0].strip()
        if a in syms: print('\n== %s (%#x)'%(syms[a],a)); reg={}
        mm=re.match(r'(r\d+|ip|lr), \[pc, #(-?\d+)\]',args)
        if op.startswith('ldr') and mm:
            v=word(a+8+int(mm.group(2))); reg[mm.group(1)]=v; continue
        mm=re.match(r'(r\d+|ip|lr), #(-?\d+)',args)
        if op in('mov','mvn') and mm:
            v=int(mm.group(2)); reg[mm.group(1)]=(~v if op=='mvn' else v)&0xffffffff; continue
        mm=re.match(r'(r\d+|ip|lr), \[(r\d+|ip|lr)(?:, #(-?\d+))?\]',args)
        if op[:3] in('ldr','str') and mm and mm.group(2) in reg and reg[mm.group(2)]>=0xf0000000:
            ad=(reg[mm.group(2)]+int(mm.group(3) or 0))&0xffffffff
            val=''
            if op.startswith('str'):
                val=' = %#x'%reg[mm.group(1)] if mm.group(1) in reg else ' = <%s>'%mm.group(1)
            else: reg.pop(mm.group(1),None)
            print('  %x %-5s %#010x%s'%(a,op,ad,val)); continue
        mm=re.match(r'(r\d+|ip|lr), (r\d+|ip|lr), #(-?\d+)',args)
        if op in('orr','bic','and','add','sub','eor') and mm:
            if mm.group(2) in reg and op in('add','sub') :
                v=int(mm.group(3)); reg[mm.group(1)]=(reg[mm.group(2)]+(v if op=='add' else -v))&0xffffffff
            else:
                reg.pop(mm.group(1),None)
                print('  %x   %s %s'%(a,op,args))
            continue
        if op in('bl','b') or op.startswith('b') and len(op)<=4 and op not in('bic','bics'):
            t=args.split()[0]
            try:
                ta=int(t,16); 
                if op=='bl': print('  %x bl %s'%(a,syms.get(ta,t))); [reg.pop(r,None) for r in('r0','r1','r2','r3','ip','lr')]
            except: pass
            continue
        d=args.split(',')[0]
        if d in reg and not op.startswith(('cmp','tst','str','push','teq')): reg.pop(d,None)
run(int(sys.argv[3],16),int(sys.argv[4],16))
