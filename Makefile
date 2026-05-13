KERNEL_DIR ?= /lib/modules/$(shell uname -r)/build

obj-m         := kvstore.o
kvstore-objs  := dev.o store.o

all:
	make -C $(KERNEL_DIR) M=$(shell pwd) modules

clean:
	make -C $(KERNEL_DIR) M=$(shell pwd) clean
