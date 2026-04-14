TARGET = i686-elf
CC     = $(TARGET)-gcc
AS     = nasm
LD     = $(TARGET)-gcc

CFLAGS  = -std=gnu99 -ffreestanding -O2 -Wall -Wextra -nostdlib -Iuser -mno-sse -mno-mmx -mno-sse2
ASFLAGS = -f elf32
LDFLAGS = -T user/user.ld -nostdlib -ffreestanding

PROGRAMS = user/hello user/shell

.PHONY: all clean

all: $(PROGRAMS)

user/crt0.o: user/crt0.s
	$(AS) $(ASFLAGS) $< -o $@

user/ulib.o: user/ulib.c
	$(CC) $(CFLAGS) -c $< -o $@

user/%.o: user/%.c
	$(CC) $(CFLAGS) -c $< -o $@

user/hello: user/crt0.o user/hello.o
	$(LD) $(LDFLAGS) -o $@ $^

user/shell: user/crt0.o user/ulib.o user/shell.o
	$(LD) $(LDFLAGS) -o $@ $^

clean:
	rm -f user/*.o $(PROGRAMS)
