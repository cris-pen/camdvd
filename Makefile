CC      ?= clang
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
UNAME   := $(shell uname -s)

SRC = src/main.c src/transport_sim.c

ifeq ($(UNAME),Darwin)
SRC     += src/transport_macos.c
LDFLAGS += -framework IOKit -framework CoreFoundation
CFLAGS  += -mmacosx-version-min=11.0
endif

bin/camdvd: $(SRC) src/drive.h
	@mkdir -p bin
	$(CC) $(CFLAGS) -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -rf bin

.PHONY: clean
