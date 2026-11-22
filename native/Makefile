# Freestanding programs for the T48S (ARMv6, Linux 2.6.27, EABI soft-float).
# No libc: raw syscalls only, so the phone's old glibc does not matter.
CC = clang
CFLAGS = --target=armv6-linux-gnueabi -march=armv6 -mfloat-abi=soft -marm -Os \
         -ffreestanding -fno-builtin -nostdlib -static -fuse-ld=lld -Wl,-e,_start

PROGS = fbtest

all: $(PROGS)

%: %.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(PROGS)
