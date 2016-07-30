obj-m += applespi.o

all:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean

test: all
	sync
	-rmmod applespi
	insmod ./applespi.ko
