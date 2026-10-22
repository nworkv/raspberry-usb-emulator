CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11

all: usb_emulator

usb_emulator: usb_emulator.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f usb_emulator

install: usb_emulator
	install -m 0755 usb_emulator /usr/local/bin/