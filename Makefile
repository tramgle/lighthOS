TARGET  = i686-elf
CC      = $(TARGET)-gcc
AS      = nasm
LD      = $(TARGET)-gcc

CFLAGS  = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -Isrc -mno-sse -mno-mmx -mno-sse2
ASFLAGS = -f elf32
LDFLAGS = -T linker.ld -nostdlib -lgcc -ffreestanding

C_SOURCES = $(shell find src -name '*.c')
S_SOURCES = $(shell find src -name '*.s')

C_OBJECTS = $(patsubst src/%.c, build/%.o, $(C_SOURCES))
S_OBJECTS = $(patsubst src/%.s, build/%.o, $(S_SOURCES))
OBJECTS   = $(C_OBJECTS) $(S_OBJECTS)

KERNEL_BIN = build/vibeos.bin
ISO        = build/vibeos.iso

.PHONY: all clean iso run debug docker-build docker-run

all: $(ISO)

$(KERNEL_BIN): $(OBJECTS)
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.s
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) $< -o $@

$(ISO): $(KERNEL_BIN) grub.cfg
	@mkdir -p build/iso/boot/grub
	cp $(KERNEL_BIN) build/iso/boot/vibeos.bin
	cp grub.cfg build/iso/boot/grub/grub.cfg
	grub-mkrescue -o $@ build/iso 2>/dev/null

disk.img:
	dd if=/dev/zero of=disk.img bs=1M count=8

run: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio -m 128M

run-disk: $(ISO) disk.img
	qemu-system-i386 -cdrom $(ISO) -drive file=disk.img,format=raw,if=ide \
		-serial stdio -m 128M

debug: $(ISO)
	qemu-system-i386 -cdrom $(ISO) -serial stdio -m 128M \
		-d int,cpu_reset -no-reboot -no-shutdown

clean:
	rm -rf build

docker-build:
	docker run --rm -v "$$(pwd)":/src vibeos-toolchain make all

docker-run: docker-build
	make run
