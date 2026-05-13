KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-m         := kvstore.o
kvstore-objs  := dev.o store.o

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(shell pwd) modules

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(shell pwd) clean
