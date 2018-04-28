# Default settings
STD=-std=c99 -pedantic -DREDIS_STATIC=''
WARN=-Wall -W -Wno-missing-field-initializers
OPT=$(OPTIMIZATION)

FINAL_CFLAGS=$(STD) $(WARN) $(OPT) $(DEBUG) $(CFLAGS)
DEBUG=-g -ggdb

REDIS_SERVER_OBJ=adlist.o ae.o anet.o atomicvar.o zmalloc.o

all: pusher

%.o: %.c
	$(REDIS_CC) -c $<

pusher: 