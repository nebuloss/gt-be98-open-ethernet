# SPDX-License-Identifier: GPL-2.0-only
#
# Out-of-tree build of the read-only stock-stack RE tools:
#   natc_dump.ko  - dump the live NAT-C table (kallsyms + probe_kernel_read)
#   xrdp_peek.ko  - read XRDP registers/SRAM (ioremap MMIO, opt-in)
#   rdpa_trace.ko - kprobe the stock rdpa control plane (NEEDS CONFIG_KPROBES=y)
#
# MIRRORS the WiFi effort's tools/dhd-kprobe-trace/Makefile: same KDIR / TC
# absolute paths, same cross-toolchain, same BUILD_NAME workaround. vermagic
# must match the RUNNING device kernel EXACTLY (CONFIG_KALLSYMS=y always).
#
# ★ rdpa_trace.ko requires CONFIG_KPROBES=y. As of 2026-06-24 the SDK kernel
#   .config HAS CONFIG_KPROBES=y + CONFIG_KALLSYMS_ALL=y (added to
#   hostTools/scripts/defconfig-bcm.template), so the DEFAULT $(KDIR) builds a
#   real (non-stub) rdpa_trace.ko. vermagic must match the kernel you BOOT for
#   the trace session - rebuild against that exact tree if it differs.
#   (CONFIG_FTRACE is still off -> CONFIG_KPROBE_EVENTS/tracefs path is likely
#   unavailable; the .ko is the primary tool, rdpa-trace-events.sh the fallback.)
#
# Usage (run on dev-build, NOT dev-code):
#   make            # build all .ko against $(KDIR) with $(CROSS_COMPILE)
#   make clean
#
# Override KDIR / TC / CROSS_COMPILE / ARCH on the command line as needed.

obj-m += natc_dump.o
obj-m += xrdp_peek.o
obj-m += rdpa_trace.o

# Point SDK at your local asuswrt-merlin.ng 4916 "behnd" checkout (the WiFi
# effort's 4.19.294 KDIR has Module.symvers). KDIR/TC default off $(SDK).
SDK   ?= $(HOME)/be98/gt-be98-firmware/vendor/asuswrt-merlin.ng/release/src-rt-5.04behnd.4916
KDIR  ?= $(SDK)/kernel/linux-4.19

# aarch64 cross-toolchain that built the device kernel (override on cmdline).
TC    ?= $(HOME)/be98/gt-be98-firmware/toolchain/am-toolchains/brcm-arm-hnd/crosstools-aarch64-gcc-10.3-linux-4.19-glibc-2.32-binutils-2.36.1
ARCH  ?= arm64
CROSS_COMPILE ?= aarch64-buildroot-linux-gnu-

PWD := $(shell pwd)

# The merlin kernel Makefile does `KBUILD_CFLAGS += -D$(MODEL)` where
# MODEL = $(subst -,,$(BUILD_NAME)); without BUILD_NAME that emits a bare `-D`.
BUILD_NAME ?= gt-be98

export PATH := $(TC)/usr/bin:$(TC)/bin:$(PATH)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		BUILD_NAME=$(BUILD_NAME) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) \
		BUILD_NAME=$(BUILD_NAME) clean

.PHONY: all clean
