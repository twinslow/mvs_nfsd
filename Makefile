# Makefile - Minimal NFSv3 server
#
# Build:
#   make          (debug)
#   make RELEASE=1
#
# Clean:
#   make clean
#
# On MVS / GCCMVS: replace CC and CFLAGS as needed; replace vfs.c
# with your MVS-specific implementation before building.

CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L -g

ifdef RELEASE
CFLAGS += -O2 -DNDEBUG
endif

SRCS = nfsd.c xdr.c rpc.c exports.c fhandle.c vfs.c \
       portmap.c mount3.c nfs3.c

OBJS = $(SRCS:.c=.o)
TARGET = nfsd

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

# All .c files depend on the shared header
%.o: %.c nfsd.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all clean
