# Default settings
STD=-std=c99
WARN=-Wall -W -Wno-missing-field-initializers
OPTIMIZATION?=-O2
OPT=$(OPTIMIZATION)
OS:=$(shell uname)

ifeq ($(OS),Linux)
STD+=-D_GNU_SOURCE # check for Linux
endif

FINAL_CFLAGS=$(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
DEBUG=-g -ggdb

PUSHER_SERVER_OBJ=adlist.o ae.o anet.o zmalloc.o networking.o debug.o server.o

all: pusher-server

pusher-server: $(PUSHER_SERVER_OBJ)
	@echo "Building pusher-server..."
	$(CC) $(OPT) $(PUSHER_SERVER_OBJ) -g -o pusher-server

%.o: %.c
	$(CC) $(STD) $(WARN) -g -c $<

clean:
	@echo "Cleaning up.."
	-rm -rf *.o # - prefix for ignoring errors and continue execution
	-rm pusher-server