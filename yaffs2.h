/*
 * This header has the essential datastructures taken from yaffs_guts.h
 * The disk format varies by platform; I chose values that match my ARM
 * device.
 */

/*
 * YAFFS: Yet another Flash File System . A NAND-flash specific file system.
 *
 * Copyright (C) 2002-2007 Aleph One Ltd.
 *   for Toby Churchill Ltd and Brightstar Engineering
 *
 * Created by Charles Manning <charles@aleph1.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1 as
 * published by the Free Software Foundation.
 *
 * Note: Only YAFFS headers are LGPL, YAFFS C code is covered by GPL.
 */

#include <linux/types.h>
#include "config.h"
#define YAFFS_MAGIC             0x5941FF53
#define YAFFS_MAX_NAME_LENGTH   255
#define YAFFS_MAX_ALIAS_LENGTH  159
#define YAFFS_OBJECTID_ROOT     1

enum object_type {
	YAFFS_OBJECT_TYPE_UNKNOWN,
	YAFFS_OBJECT_TYPE_FILE,
	YAFFS_OBJECT_TYPE_SYMLINK,
	YAFFS_OBJECT_TYPE_DIRECTORY,
	YAFFS_OBJECT_TYPE_HARDLINK,
	YAFFS_OBJECT_TYPE_SPECIAL
};

/* On-flash structures */

struct yaffs2_tags {
    __le32 sequence_number;
    __le32 object_id;
    __le32 chunk_id;
    __le32 byte_count;
    __le32 ecc_result;
    __le32 pad[11];
};

struct yaffs2_object_header {
    __le32 object_type;
    __le32 parent_object_id;
    __le16 sum_obsolete;
    char name[YAFFS_MAX_NAME_LENGTH + 1];
    __le32 mode;
    __le32 uid;
    __le32 gid;
    __le32 atime;
    __le32 mtime;
    __le32 ctime;
    __le32 size;
    __le32 equiv_object_id;
    char alias[YAFFS_MAX_NAME_LENGTH + 1];
    __le32 rdev;
    __le32 reserved[6];
    __le32 inband_shadows_object;
    __le32 inband_is_shrink;
    __le32 reserved2[2];
    __le32 shadows_object;
    __le32 is_shrink;
};

/* In-memory structures */
struct yaffs2_inode {
    struct yaffs2_object_header header;
    u32 object_id;
    GList *children;
};

