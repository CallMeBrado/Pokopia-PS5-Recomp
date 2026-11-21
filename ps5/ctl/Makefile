# pokopia-ctl resident payload; build inside PS5Build with PS5_PAYLOAD_SDK set.
ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

ELF := pokopia-ctl.elf
CFLAGS := -Wall -Werror -O2
CFLAGS += -lSceSystemService -lSceUserService

all: $(ELF)

$(ELF): main.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(ELF)
