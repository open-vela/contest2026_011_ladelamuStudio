# D13x Bootloader Makefile

CROSSDEV = riscv32-unknown-elf-
CC = $(CROSSDEV)gcc
AS = $(CROSSDEV)gcc
LD = $(CROSSDEV)gcc
OBJCOPY = $(CROSSDEV)objcopy
SIZE = $(CROSSDEV)size

ARCHFLAGS = -march=rv32imafdc -mabi=ilp32d -mcmodel=medlow
CFLAGS = $(ARCHFLAGS) -Os -ffunction-sections -fdata-sections -nostdlib -ffreestanding -Iinclude
ASFLAGS = $(ARCHFLAGS) -c
LDFLAGS = $(ARCHFLAGS) -nostdlib -T scripts/linker.ld -Wl,--gc-sections

OBJS = src/startup.o src/system.o src/main.o

TARGET = d13x_bootloader

.PHONY: all clean

all: $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(LD) $(LDFLAGS) -o $@ $^
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary -R .note -R .note.gnu.build-id -R .comment $< $@
	$(SIZE) $<

src/startup.o: src/startup.S
	$(AS) $(ASFLAGS) -o $@ $<

src/system.o: src/system.c include/d13x_boot.h
	$(CC) $(CFLAGS) -c -o $@ $<

src/main.o: src/main.c include/d13x_boot.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET).elf $(TARGET).bin
