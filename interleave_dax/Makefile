# Makefile for interleave_dax kernel module (multi-region)

obj-m := interleave_dax.o

# Kernel build tree. Defaults to the running kernel's build symlink
# under /lib/modules; override with `make KDIR=/path/to/kernel/build`
# when building against a different tree.
KDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) CC=clang modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

