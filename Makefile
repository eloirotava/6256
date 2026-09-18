# SPDX-License-Identifier: GPL-2.0-only
obj-$(CONFIG_SSV6256) += ssv6256.o
ssv6256-y := sdio.o hw.o phy.o mac.o tx.o rx.o

ifeq ($(KERNELRELEASE),)
# Out-of-tree build: make [KVER=<kernel version>] [KDIR=<kernel build dir>]
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) CONFIG_SSV6256=m modules

install: all
	install -D -m 644 ssv6256.ko $(DESTDIR)/lib/modules/$(KVER)/updates/ssv6256.ko
	install -D -m 644 ssv6x5x-sw.bin $(DESTDIR)/lib/firmware/ssv/ssv6x5x-sw.bin
	[ -n "$(DESTDIR)" ] || depmod -a $(KVER)

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all install clean
endif
