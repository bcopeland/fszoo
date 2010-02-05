ext2_srcs=ext2.c
ext2_objs=$(ext2_srcs:.c=.o)

CFLAGS+=-g -Wall -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=26 `pkg-config --cflags fuse talloc`

all: ext2_fuse

ext2_fuse: $(ext2_objs)
	gcc -o ext2_fuse $(ext2_objs) `pkg-config --libs fuse talloc`
