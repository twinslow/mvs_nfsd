# Makefile - Minimal NFSv3 server
#
# Build:
#   make          (debug)
#   make RELEASE=1
#
# Clean:
#   make clean
#
# Source files live in ./src.
# Compiled object files and the final binary are written to ./build
# (the directory is created automatically if it does not exist).
#
# On MVS / GCCMVS: replace CC and CFLAGS as needed; replace vfs.c
# with your MVS-specific implementation before building.

CC       = gcc
CFLAGS   = -std=c99 -Wall -Wextra -pedantic -D_POSIX_C_SOURCE=200809L -g

ifdef RELEASE
CFLAGS  += -O2 -DNDEBUG
endif

SRCDIR   = src
BUILDDIR = build
TARGET   = $(BUILDDIR)/nfsd

SRCS = nfsd.c xdr.c rpc.c exports.c fhandle.c vfs.c \
       portmap.c mount3.c nfs3.c

# Map each bare .c name to a build/.o path
OBJS = $(patsubst %.c,$(BUILDDIR)/%.o,$(SRCS))

# ------------------------------------------------------------------ #
# Top-level targets                                                    #
# ------------------------------------------------------------------ #

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -o $@ $^

# ------------------------------------------------------------------ #
# Pattern rule: src/foo.c -> build/foo.o                              #
#                                                                      #
# -I$(SRCDIR) lets #include "nfsd.h", "types.h", etc. resolve to     #
# the headers now living in ./src without changing any source file.   #
#                                                                      #
# nfsd.h is listed as a prerequisite so all objects are rebuilt       #
# whenever the master header changes.                                  #
# ------------------------------------------------------------------ #

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(SRCDIR)/nfsd.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -I$(SRCDIR) -c -o $@ $<

# ------------------------------------------------------------------ #
# Create the build directory on demand (order-only prerequisite)       #
# ------------------------------------------------------------------ #

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ------------------------------------------------------------------ #
# Clean: remove the entire build directory                             #
# ------------------------------------------------------------------ #

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean
