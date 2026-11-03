CC_DEFAULT := clang
ifeq ($(origin CC),default)
CC := $(CC_DEFAULT)
endif

CFLAGS  ?= -std=gnu11 -O2 -Wall -Wextra
LDFLAGS ?=
LDLIBS  := -ldl

all: df_pcc_relay

df_pcc_relay: src/df_pcc_relay.c src/df_pcc_relay.h
	$(CC) $(CFLAGS) -pthread -s $(LDFLAGS) -o $@ src/df_pcc_relay.c $(LDLIBS)

check:
	sh tests/run.sh

clean:
	rm -f df_pcc_relay

.PHONY: all check clean
