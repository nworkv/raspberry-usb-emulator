CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -Werror -std=gnu11

all: usb_emulator

usb_emulator: usb_emulator.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f usb_emulator
