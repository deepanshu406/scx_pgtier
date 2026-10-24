# SPDX-License-Identifier: GPL-2.0
#
# Standalone build for scx_pgtier. It borrows the scx headers and the
# vmlinux.h from a scx-c-examples checkout (SCX_DIR) and links against a
# locally built libbpf (LIBBPF_PREFIX) so the headers, bpftool and library
# versions match.
#
#   make                 build ./build/scx_pgtier
#   make clean
#   make SCX_DIR=/path/to/scx-c-examples LIBBPF_PREFIX=/path/to/libbpf/install

SCX_DIR       ?= $(HOME)/work/scx-c-examples
LIBBPF_PREFIX ?= $(HOME)/opt/libbpf
BPF_CLANG     ?= clang
BPFTOOL       ?= bpftool
CC            ?= cc

BUILD := build

ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
  TARGET_ARCH := x86
else ifeq ($(ARCH),aarch64)
  TARGET_ARCH := arm64
else
  $(error unsupported architecture $(ARCH); add it here)
endif

# libbpf was installed with PREFIX=$(LIBBPF_PREFIX); its .pc file may be in
# lib/ or lib64/ depending on the system. The variable is set inside each
# $(shell ...) call because older GNU make does not pass makefile-level
# exports to $(shell).
PKGCFG := PKG_CONFIG_PATH='$(LIBBPF_PREFIX)/lib64/pkgconfig:$(LIBBPF_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)' pkg-config

LIBBPF_CFLAGS := $(shell $(PKGCFG) --cflags libbpf)
LIBBPF_LIBS   := $(shell $(PKGCFG) --libs libbpf)

# Fail early if we picked up the system libbpf instead of the local one.
ifeq ($(findstring $(LIBBPF_PREFIX),$(LIBBPF_CFLAGS)),)
  $(warning libbpf from $(LIBBPF_PREFIX) not found by pkg-config; using the system one, which may not match bpftool)
endif

BPF_CFLAGS := -g -O2 -Wall -Wno-compare-distinct-pointer-types \
	-D__TARGET_ARCH_$(TARGET_ARCH) -mcpu=v3 -mlittle-endian

BPF_INCLUDES := -I$(SCX_DIR)/scheds/include \
	-I$(SCX_DIR)/scheds/include/bpf-compat \
	-I$(SCX_DIR)/scheds/include/lib \
	-I$(SCX_DIR)/scheds/vmlinux \
	-I$(SCX_DIR)/scheds/vmlinux/arch/$(TARGET_ARCH) \
	$(LIBBPF_CFLAGS)

CFLAGS := -std=gnu11 -O2 -g -Wall \
	-I$(SCX_DIR)/scheds/include \
	-I$(SCX_DIR)/scheds/vmlinux \
	-I$(BUILD) -I. \
	$(LIBBPF_CFLAGS)

LDLIBS := $(LIBBPF_LIBS) -lelf -lz -lzstd -lpthread

all: $(BUILD)/scx_pgtier

$(BUILD):
	mkdir -p $@

# 1. BPF C -> BPF object
$(BUILD)/scx_pgtier.bpf.o: scx_pgtier.bpf.c scx_pgtier.h | $(BUILD)
	$(BPF_CLANG) $(BPF_CFLAGS) $(BPF_INCLUDES) -target bpf -c $< -o $@

# 2. Link the object, then generate the skeleton header from it
$(BUILD)/scx_pgtier.linked.o: $(BUILD)/scx_pgtier.bpf.o
	$(BPFTOOL) gen object $@ $<

$(BUILD)/scx_pgtier.bpf.skel.h: $(BUILD)/scx_pgtier.linked.o
	$(BPFTOOL) gen skeleton $< name scx_pgtier > $@

# 3. Loader
$(BUILD)/scx_pgtier: scx_pgtier.c scx_pgtier.h $(BUILD)/scx_pgtier.bpf.skel.h
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
