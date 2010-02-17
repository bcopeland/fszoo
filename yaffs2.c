#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <talloc.h>
#include <string.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include <errno.h>
#include <glib.h>

#include "yaffs2.h"
#include "config.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define div_round(a,b) ((a)+(b)-1)/(b)

struct yaffs2_info
{
    FILE *dev;

    /* parameters for our fake flash */
    int mtd_page;
    int mtd_extra;
    int mtd_erase;
    int chunks_per_block;
    int nblocks;
    int nchunks;

    /* object table, indexed by inode number */
    GHashTable *object_map;
};

/* internal I/O routines */

int bread(void *buf, int blk_size, u64 blk, FILE *fp)
{
    fseeko(fp, blk * blk_size, SEEK_SET);
    return fread(buf, blk_size, 1, fp);
}

u8 *bread_m(int blk_size, u64 blk, u64 count, FILE *fp)
{
    u8 *mem = talloc_size(NULL, blk_size * count);
    bread(mem, blk_size, blk * count, fp);
    return mem;
}

size_t device_get_size(FILE *fp)
{
    off_t here = ftello(fp);
    off_t end;

    fseeko(fp, 0, SEEK_END);
    end = ftello(fp);
    fseeko(fp, here, SEEK_SET);

    return end;
}

#if 0
u8 *yaffs2_get_block_n(struct yaffs2_info *info, struct yaffs2_inode *inode,
                     int blknum)
{
    u32 ptrs_per_block = info->block_size /  sizeof(u32);
    u32 dptrs = ptrs_per_block * ptrs_per_block;
    u32 ptrs[4];
    int nptrs = 0;
    int i;

    u8 *block = talloc_size(info, info->block_size);

    /* build a list of blocks to read to reach the target block */
    /* direct blocks */
    if (blknum < EXT2_NDIR_BLOCKS)
    {
        ptrs[nptrs++] = blknum;
    }
    /* one indirection */
    else if ( (blknum -= EXT2_NDIR_BLOCKS) < ptrs_per_block)
    {
        ptrs[nptrs++] = EXT2_IND_BLOCK;
        ptrs[nptrs++] = blknum;
    }
    /* double indirection */
    else if ( (blknum -= ptrs_per_block) < dptrs)
    {
        ptrs[nptrs++] = EXT2_DIND_BLOCK;
        ptrs[nptrs++] = blknum / ptrs_per_block;
        ptrs[nptrs++] = blknum % ptrs_per_block;
    }
    /* triple indirection */
    else
    {
        blknum -= dptrs;
        ptrs[nptrs++] = EXT2_TIND_BLOCK;
        ptrs[nptrs++] = blknum / dptrs;
        ptrs[nptrs++] = (blknum / ptrs_per_block) % ptrs_per_block;
        ptrs[nptrs++] = blknum % ptrs_per_block;
    }

    blknum = inode->i_block[ptrs[0]];

    for (i=1; i < nptrs; i++)
    {
        bread(block, info->block_size, blknum, info->dev);
        blknum = ((u32 *) block)[ptrs[i]];
    }
    talloc_free(block);
    return bread_m(info->block_size, blknum, 1, info->dev);
}
#endif

int yaffs2_read_inode(struct yaffs2_info *info, u32 ino,
                      struct yaffs2_inode **ret)
{
    gboolean found;
    gpointer key, value;

    found = g_hash_table_lookup_extended(info->object_map, &ino,
                                         &key, &value);
    if (!found)
        return -ENOENT;

    *ret = value;
    return 0;
}

struct yaffs2_inode *find_or_create_inode(struct yaffs2_info *info, u32 ino)
{
    struct yaffs2_inode *inode;

    if (yaffs2_read_inode(info, ino, &inode) == 0)
        return inode;

    inode = talloc_zero_size(info, sizeof(*inode));
    inode->object_id = ino;

    /* hash the new inode */
    g_hash_table_insert(info->object_map, &inode->object_id, inode);
    return inode;
}

int yaffs2_read_super(struct yaffs2_info *info)
{
    struct yaffs2_inode *root_dir, *inode, *parent;
    struct yaffs2_object_header *object;
    struct yaffs2_tags *tags;
    int res;
    int block, chunk;
    int devsize;
    char *buf;

    info->object_map = g_hash_table_new(g_int_hash, g_int_equal);

    devsize = device_get_size(info->dev);

    info->mtd_page = 2048;
    info->mtd_extra = 64;
    info->mtd_erase = 131072;
    info->chunks_per_block = info->mtd_erase / info->mtd_page;

    /*
     * A 'chunk' in yaffs terminology is the MTD page size - we assume 2k.
     * A block is the MTD erase block size.
     */
    info->nblocks = devsize / info->mtd_erase;
    info->nchunks = info->nblocks * info->chunks_per_block;

    /* setup place holder for the root directory */
    root_dir = talloc_zero_size(info, sizeof(*root_dir));
    root_dir->object_id = YAFFS_OBJECTID_ROOT;
    root_dir->header.mode = S_IFDIR | 0755;
    g_hash_table_insert(info->object_map, &root_dir->object_id,
        root_dir);

    /* scan the whole disk, adding inodes into memory */
    buf = talloc_size(info, info->mtd_page + info->mtd_extra);
    for (block = 0; block <= info->nblocks; block++)
    {
        for (chunk = 0; chunk < info->chunks_per_block; chunk++)
        {
            bread(buf, info->mtd_page + info->mtd_extra,
                  info->chunks_per_block * block + chunk, info->dev);

            tags = (struct yaffs2_tags *) &buf[info->mtd_page];
            object = (struct yaffs2_object_header *) buf;

            if (tags->sequence_number != ~0 && tags->chunk_id == 0)
            {
                inode = find_or_create_inode(info,
                    le32_to_cpu(tags->object_id));

                if (le32_to_cpu(tags->sequence_number) >
                    inode->sequence_number)
                {
                    memcpy(&inode->header, object, sizeof(*object));
                    inode->sequence_number =
                        le32_to_cpu(tags->sequence_number);

                    /* add to parent directory's list */
                    parent = find_or_create_inode(info,
                            inode->header.parent_object_id);
                    parent->children = g_list_prepend(parent->children, inode);
                }
            }
            else if (tags->chunk_id > 0)
            {
                /* create block nr tree */
            }
        }
    }
    talloc_free(buf);

    return 0;
}

int yaffs2_stat(struct yaffs2_info *info, u32 ino, struct stat *st)
{
    struct yaffs2_inode *inode;

    if (yaffs2_read_inode(info, ino, &inode))
        return ENOENT;

    st->st_ino = inode->object_id;
    st->st_mode = le16_to_cpu(inode->header.mode);
    st->st_nlink = 2;
    st->st_uid = le16_to_cpu(inode->header.uid);
    st->st_gid = le16_to_cpu(inode->header.gid);
    st->st_size = le32_to_cpu(inode->header.size);
    st->st_atime = le32_to_cpu(inode->header.atime);
    st->st_mtime = le32_to_cpu(inode->header.mtime);
    st->st_ctime = le32_to_cpu(inode->header.ctime);
#if 0
    st->st_blksize = info->block_size;
    st->st_blocks = le32_to_cpu(inode.i_blocks);
#endif
    return 0;
}

/* FUSE API */

static void yaffs2_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct yaffs2_info *info = fuse_req_userdata(req);
    struct yaffs2_inode *dir;
    struct yaffs2_inode *inode;
    struct fuse_entry_param result;
    GList *list;

    if (yaffs2_read_inode(info, parent, &dir))
        goto out;

    /* search the directory's children for name */
    for (list = g_list_first(dir->children); list; list = g_list_next(list))
    {
        inode = list->data;

        if (strcmp(inode->header.name, name) == 0)
        {
            result.ino = le32_to_cpu(inode->object_id);
            yaffs2_stat(info, result.ino, &result.attr);
            goto found;
        }
    }

out:
    fuse_reply_err(req, ENOENT);
    return;

found:
    fuse_reply_entry(req, &result);
}

static
void yaffs2_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int err;
    struct yaffs2_info *info = fuse_req_userdata(req);
    struct stat st;

    err = yaffs2_stat(info, ino, &st);
    if (err)
        goto out;

    fuse_reply_attr(req, &st, 1.0);
    return;
out:
    fuse_reply_err(req, err);
}

static void yaffs2_readlink(fuse_req_t req, fuse_ino_t ino)
{
/*
    fuse_reply_err(req, EIO);
*/
}

static
void yaffs2_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct yaffs2_info *info = fuse_req_userdata(req);
    struct yaffs2_inode *inode;

    /* read the inode and store it in fi->fh */
    if (yaffs2_read_inode(info, ino, &inode))
        fuse_reply_err(req, ENOENT);

    fi->fh = (uint64_t) (unsigned long) inode;
    fuse_reply_open(req, fi);
}

#if 0
static void yaffs2_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
               struct fuse_file_info *fi)
{
    struct yaffs2_info *info = fuse_req_userdata(req);
    struct yaffs2_inode *inode = (struct yaffs2_inode *) (unsigned long) fi->fh;
    u32 blk_start, blk_ofs;
    int nblocks;
    char *buf;
    int bufsize = 0;
    int i;

    buf = talloc_size(info, size);
    blk_start = off / info->block_size;
    blk_ofs = off % info->block_size;

    /* compute actual size to read */
    size = min(size, inode->i_size - off);
    nblocks = div_round(size + blk_ofs, info->block_size);

    /* read all the associated blocks, and copy as space allows */
    for (i=blk_start; i < blk_start + nblocks; i++)
    {
        u8 *block = yaffs2_get_block_n(info, inode, i);
        if (!block)
            goto out;

        memcpy(buf + bufsize, block + blk_ofs, info->block_size - blk_ofs);
        bufsize += info->block_size - blk_ofs;
        blk_ofs = 0;
    }
    fuse_reply_buf(req, buf, bufsize);
    return;

out:
    fuse_reply_err(req, EIO);
}

static
void yaffs2_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct yaffs2_inode *inode = (struct yaffs2_inode *) (unsigned long) fi->fh;

    talloc_free(inode);
    fuse_reply_err(req, 0);
}
#endif

static
void yaffs2_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    fuse_reply_open(req, fi);
}

static
void yaffs2_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                  struct fuse_file_info *fi)
{
    struct yaffs2_info *info = fuse_req_userdata(req);
    struct yaffs2_inode *dir, *inode;
    char *buf;
    GList *list;
    int i;
    int ret;
    int bufsize=0;

    if (yaffs2_read_inode(info, ino, &dir))
        goto err;

    buf = talloc_size(info, size);

    size = min(dir->header.size, size);

    list = g_list_first(dir->children);
    for (i=0; i < size && list; i++, list = g_list_next(list))
    {
        inode = list->data;

        if (i >= off)
        {
            struct stat st = {
                .st_ino = inode->object_id,
            };

            switch (inode->header.object_type)
            {
                case YAFFS_OBJECT_TYPE_DIRECTORY:
                    st.st_mode |= S_IFDIR;
                    break;
                case YAFFS_OBJECT_TYPE_FILE:
                default:
                    st.st_mode |= S_IFREG;
                    break;
            }

            ret = fuse_add_direntry(req, buf + bufsize, size - bufsize,
                                    inode->header.name, &st, i+1);
            if (ret >= size - bufsize)
                goto done;

            bufsize += ret;
        }
    }

done:
    fuse_reply_buf(req, buf, bufsize);
    talloc_free(buf);
    return;

err:
    fuse_reply_err(req, EIO);
}

static void yaffs2_releasedir(fuse_req_t req, fuse_ino_t ino,
                     struct fuse_file_info *fi)
{
    fuse_reply_err(req, 0);
}

static void yaffs2_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct yaffs2_info *fsi = fuse_req_userdata(req);

    struct statvfs stbuf = {
        .f_bsize = fsi->mtd_page,
        .f_frsize = fsi->mtd_page,
        .f_blocks = fsi->nblocks,
        .f_bfree = fsi->nblocks,
        .f_bavail = fsi->nblocks,
        .f_files = g_hash_table_size(fsi->object_map),
        .f_ffree = ~0,
        .f_favail = ~0,
        .f_fsid = YAFFS_MAGIC,
        .f_flag = 0,
        .f_namemax = YAFFS_MAX_NAME_LENGTH,
    };

    fuse_reply_statfs(req, &stbuf);
}

#if 0
static void yaffs2_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                   size_t size)
{
}

static void yaffs2_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
}

static void yaffs2_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize,
               uint64_t idx)
{
    /* fuse_reply_bmap(req, idx) */
}
#endif

static struct fuse_lowlevel_ops yaffs2_ops = {
    .lookup = yaffs2_lookup,
    .opendir = yaffs2_opendir,
    .readdir = yaffs2_readdir,
    .releasedir = yaffs2_releasedir,
    .statfs = yaffs2_statfs,
    .getattr = yaffs2_getattr,
#if 0
    .readlink = yaffs2_readlink,
    .open = yaffs2_open,
    .read = yaffs2_read,
    .release = yaffs2_release,
    .getxattr = yaffs2_getxattr,
    .listxattr = yaffs2_listxattr,
    .bmap = yaffs2_bmap
#endif
};

int main(int argc, char *argv[])
{
    struct yaffs2_info *ctx;
    int i, fuse_argc=0;
    char *device;
    struct fuse_session *sess;
    struct fuse_chan *chan;
    struct fuse_args args;
    char *mountpoint;
    int multithreaded;
    int foreground;
    int res;

    ctx = talloc(NULL, struct yaffs2_info);

    /* FIXME replace this with fuse_getopt */
    char **fuse_argv = malloc((argc + 1) * sizeof(char *));

    for (i=0; i < argc; i++)
    {
        if ((strcmp(argv[i], "-a") == 0) && i + 1 < argc)
        {
            i++;
            device = argv[i];
        }
        else
            fuse_argv[fuse_argc++] = argv[i];
    }

    fuse_argv[fuse_argc] = NULL;

    if (!device)
    {
        fprintf(stderr, "Usage: %s -a <device_file> <mount_point>\n", argv[0]);
        return 1;
    }

    FILE *fp = fopen(device, "r");
    if (!fp)
    {
        perror("yaffs2_fuse");
        return 2;
    }

    ctx->dev = fp;

    if (yaffs2_read_super(ctx))
    {
        printf ("Could not read super block\n");
        return 3;
    }

    args.argc = fuse_argc;
    args.argv = fuse_argv;
    args.allocated = 0;

    res = fuse_parse_cmdline(&args, &mountpoint, &multithreaded, &foreground);
    if (res == -1)
        goto out_err;

    printf ("mount %s\n", mountpoint);

    chan = fuse_mount(mountpoint, &args);
    if (!chan)
        goto out_err;

    sess = fuse_lowlevel_new(&args, &yaffs2_ops, sizeof(yaffs2_ops), ctx);
    fuse_session_add_chan(sess, chan);

    res = fuse_daemonize(foreground);
    if (res == -1)
        goto err_unmount;

    res = fuse_set_signal_handlers(sess);
    if (res == -1)
        goto err_unmount;

    fuse_session_loop_mt(sess);
    talloc_free(ctx);
    return 0;

err_unmount:
    fuse_unmount(mountpoint, chan);

out_err:
    free(mountpoint);
    return 0;
}
