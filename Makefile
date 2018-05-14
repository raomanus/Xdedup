obj-m += sys_xdedup.o

INC=/lib/modules/$(shell uname -r)/build/arch/x86/include

all: xdedup xdedup_call

xdedup: xdedup.c
	gcc -Wall -Werror -I$(INC)/generated/uapi -I$(INC)/uapi xdedup.c -o xdedup

xdedup_call:
	make -Wall -Werror -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm -f xdedup
