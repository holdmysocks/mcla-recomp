PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_HOST ?= 192.168.1.77
PS5_PORT ?= 9021

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

ELF := sysprobe.elf
CFLAGS := -Wall -O1 -g

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)

# Sends the payload to the console's ELF loader and prints its stdout.
test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^
