# SPDX-License-Identifier: GPL-2.0
# Out-of-tree module Makefile.
#
# Build against a configured mainline tree (cross aarch64):
#   make KDIR=~/mainline ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- modules
#
# Builds run on the dedicated build host only (not on the dev/edit machine).
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: modules clean
