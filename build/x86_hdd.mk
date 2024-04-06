IMG_NAME := $(NAME).img

BOOT_PART_SIZE := 32
BOOT_PART_TYPE := 0xBA
SYS_PART_START := $(shell expr $(BOOT_PART_SIZE) + 1)

.PHONY: x86_hdd
x86_hdd: bin/$(IMG_NAME)

#mkpart primary ext2 $(SYS_PART_START)s -1s \

bin/$(IMG_NAME): bin/boot/mbr.bin bin/boot/boot.bin
	dd if=/dev/zero of=$@ bs=1k count=0 seek=1k
	parted $@ -s -f -- \
		mklabel msdos \
		mkpart primary 1s $(BOOT_PART_SIZE)s \
		set 1 boot on \
		type 1 $(BOOT_PART_TYPE)
	dd if=$(word 1, $^) of=$@ conv=notrunc count=446 bs=1
	dd if=$(word 1, $^) of=$@ conv=notrunc bs=1 count=2 seek=510 skip=510
	dd if=$(word 2, $^) of=$@ conv=notrunc seek=1 bs=512
