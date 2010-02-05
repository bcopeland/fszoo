#include <fuse.h>
#include <fuse/fuse_lowlevel.h>
#include <talloc.h>
#include <string.h>
#include <linux/fs.h>
#include <linux/ext2_fs.h>
#include <errno.h>

#include "config.h"

#define min(a,b) ((a)<(b)?(a):(b))
#define max(a,b) ((a)>(b)?(a):(b))
#define div_round(a,b) ((a)+(b)-1)/(b)

struct ext2_info
{
    FILE *dev;
    struct ext2_super_block sb;
    struct ext2_group_desc *groups;

    /* useful in-memory, cpu-endian values */
    u32 block_size;
    u32 frag_size;
    u32 ngroups;
    u32 inode_size;
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

u8 *ext2_get_block_n(struct ext2_info *info, struct ext2_inode *inode,
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

int ext2_read_inode(struct ext2_info *info, u32 ino, struct ext2_inode *ret)
{
    u32 inodes_per_group = le32_to_cpu(info->sb.s_inodes_per_group);
    u32 inode_size = le32_to_cpu(info->sb.s_inode_size);
    u32 inodes_per_block = info->block_size / inode_size;
    u64 tbl_addr, blk_addr, blk_ofs;
    u8 *inode_table;

    if (ino == FUSE_ROOT_ID)
        ino = EXT2_ROOT_INO;

    /* inodes are 1-based */
    ino--;

    /* get the correct group number */
    int bg = ino / inodes_per_group;
    int offs = ino % inodes_per_group;

    /* now find the corresponding inode table */
    tbl_addr = info->groups[bg].bg_inode_table;

    /* and get the block that is offs / inodes_per_block... */
    blk_addr = tbl_addr + offs / inodes_per_block;
    blk_ofs = offs % inodes_per_block;

    /* finally, read it */
    inode_table = bread_m(info->block_size, blk_addr, 1, info->dev);

    /* copy into ret */
    memcpy(ret, (struct ext2_inode *) (inode_table + blk_ofs * inode_size),
           sizeof(*ret));

    talloc_free(inode_table);

    return 0;
}

int ext2_read_super(struct ext2_info *info)
{
    int res;
    int i;
    int group_desc_sz;      /* size of group desc, in blocks */

    res = bread(&info->sb, EXT2_MIN_BLOCK_SIZE, 1, info->dev);

    if (res != 1)
        goto err;

    /* swap endianness for some ext2_fs.h macros */
    info->sb.s_log_block_size = le32_to_cpu(info->sb.s_log_block_size);
    info->sb.s_log_frag_size = le32_to_cpu(info->sb.s_log_frag_size);

    info->block_size = EXT2_BLOCK_SIZE(&info->sb);
    info->frag_size = EXT2_FRAG_SIZE(&info->sb);

    /* note, this is only valid for EXT2_DYNAMIC_REV */
    info->inode_size = le32_to_cpu(info->sb.s_inode_size);

    info->ngroups = div_round(le32_to_cpu(info->sb.s_blocks_count),
        le32_to_cpu(info->sb.s_blocks_per_group));

    /* read in all of the group descriptors */
    group_desc_sz = div_round(info->ngroups * sizeof(struct ext2_group_desc),
                  info->block_size);

    info->groups = talloc_size(info, group_desc_sz * info->block_size);
    for (i=0; i < group_desc_sz; i++)
    {
        int ofs = i * info->block_size / sizeof(struct ext2_group_desc);
        bread(&info->groups[ofs], info->block_size, 1+i, info->dev);
    }

    res = 0;
err:
    return res;
}

int ext2_stat(struct ext2_info *info, u32 ino, struct stat *st)
{
    struct ext2_inode inode;

    if (ext2_read_inode(info, ino, &inode))
        return ENOENT;

    st->st_ino = ino;
    st->st_mode = le16_to_cpu(inode.i_mode);
    st->st_nlink = le16_to_cpu(inode.i_links_count);
    st->st_uid = le16_to_cpu(inode.i_uid);
    st->st_gid = le16_to_cpu(inode.i_gid);
    st->st_size = le32_to_cpu(inode.i_size);
    st->st_blksize = info->block_size;
    st->st_blocks = le32_to_cpu(inode.i_blocks);
    st->st_atime = le32_to_cpu(inode.i_atime);
    st->st_mtime = le32_to_cpu(inode.i_mtime);
    st->st_ctime = le32_to_cpu(inode.i_ctime);

    return 0;
}

/* FUSE API */

static void ext2_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    struct ext2_info *info = fuse_req_userdata(req);
    struct ext2_inode dir;
    struct fuse_entry_param result;
    struct ext2_dir_entry_2 *entry;
    int dirsize;
    int i, j;
    int namelen, tgt_namelen;

    if (ext2_read_inode(info, parent, &dir))
        goto out;

    dirsize = le32_to_cpu(dir.i_size);
    tgt_namelen = strlen(name);

    /* scan directory associated with ino for name */
    for (i=0; i < dirsize; i += info->block_size)
    {
        u8 *block = ext2_get_block_n(info, &dir, i / info->block_size);
        if (!block)
            goto out;

        for (j=0; j < info->block_size && j < dirsize; )
        {
            entry = (struct ext2_dir_entry_2 *) &block[j];

            namelen = le32_to_cpu(entry->name_len);

            if (namelen == tgt_namelen &&
                strncmp(entry->name, name, le32_to_cpu(entry->name_len)) == 0)
            {
                /* got it - return success */
                result.ino = le32_to_cpu(entry->inode);
                ext2_stat(info, result.ino, &result.attr);
                talloc_free(block);
                goto found;
            }
            j += le32_to_cpu(entry->rec_len);
        }
        talloc_free(block);
    }

out:
    fuse_reply_err(req, ENOENT);
    return;

found:
    fuse_reply_entry(req, &result);
}

static
void ext2_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    int err;
    struct ext2_info *info = fuse_req_userdata(req);
    struct stat st;

    err = ext2_stat(info, ino, &st);
    if (err)
        goto out;

    fuse_reply_attr(req, &st, 1.0);
    return;
out:
    fuse_reply_err(req, err);
}

static void ext2_readlink(fuse_req_t req, fuse_ino_t ino)
{
    fuse_reply_err(req, EIO);
}

static
void ext2_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct ext2_info *info = fuse_req_userdata(req);
    struct ext2_inode *inode;

    inode = talloc_size(info, sizeof(struct ext2_inode));

    /* read the inode and store it in fi->fh */
    if (ext2_read_inode(info, ino, inode))
        fuse_reply_err(req, ENOENT);

    fi->fh = (uint64_t) (unsigned long) inode;
    fuse_reply_open(req, fi);
}

static void ext2_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
               struct fuse_file_info *fi)
{
    struct ext2_info *info = fuse_req_userdata(req);
    struct ext2_inode *inode = (struct ext2_inode *) (unsigned long) fi->fh;
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
        u8 *block = ext2_get_block_n(info, inode, i);
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
void ext2_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    struct ext2_inode *inode = (struct ext2_inode *) (unsigned long) fi->fh;

    talloc_free(inode);
    fuse_reply_err(req, 0);
}

static
void ext2_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info *fi)
{
    fuse_reply_open(req, fi);
}

static
void ext2_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                  struct fuse_file_info *fi)
{
    struct ext2_info *info = fuse_req_userdata(req);
    struct ext2_super_block *sb = &info->sb;
    struct ext2_inode dir;
    struct ext2_dir_entry_2 *entry;
    struct stat file_info;
    int i, j;
    int curblk, curofs;
    int dirsize;
    char *buf;
    int bufsize = 0, ret;
    char name[EXT2_NAME_LEN+1];
    int namelen;

    if (ext2_read_inode(info, ino, &dir))
        goto err;

    buf = talloc_size(info, size);

    dirsize = dir.i_size;

    for (i=off; i < off + size && i < dirsize; )
    {
        curblk = i / info->block_size;
        curofs = i % info->block_size;

        u8 *block = ext2_get_block_n(info, &dir, curblk);
        if (!block)
            goto err;

        /* parse all of the directory items, etc */
        for (j=curofs; j < info->block_size && j < dirsize; )
        {
            entry = (struct ext2_dir_entry_2 *) &block[j];

            struct stat st = {
                .st_ino = le32_to_cpu(entry->inode),
            };

            switch (entry->file_type)
            {
                case EXT2_FT_DIR:
                    st.st_mode |= S_IFDIR;
                    break;
                case EXT2_FT_REG_FILE:
                default:
                    st.st_mode |= S_IFREG;
                    break;
            }

            namelen = le32_to_cpu(entry->name_len);
            memcpy(name, entry->name, namelen);
            name[namelen] = 0;

            ret = fuse_add_direntry(req, buf + bufsize, size - bufsize, name,
                                    &st, i + j + entry->rec_len);
            if (ret >= size - bufsize)
            {
                talloc_free(block);
                goto done;
            }

            bufsize += ret;
            j += le32_to_cpu(entry->rec_len);
        }
        talloc_free(block);
        i += j;
    }

done:
    fuse_reply_buf(req, buf, bufsize);
    talloc_free(buf);
    return;

err:
    fuse_reply_err(req, EIO);
}

static void ext2_releasedir(fuse_req_t req, fuse_ino_t ino,
                     struct fuse_file_info *fi)
{
    fuse_reply_err(req, 0);
}

static void ext2_statfs(fuse_req_t req, fuse_ino_t ino)
{
    struct ext2_info *fsi = fuse_req_userdata(req);
    struct ext2_super_block *sb = &fsi->sb;

    struct statvfs stbuf = {
        .f_bsize = fsi->block_size,
        .f_frsize = fsi->frag_size,
        .f_blocks = le32_to_cpu(sb->s_blocks_count),
        .f_bfree = le32_to_cpu(sb->s_free_blocks_count),
        .f_bavail = le32_to_cpu(sb->s_free_blocks_count) -
                    le32_to_cpu(sb->s_r_blocks_count),
        .f_files = le32_to_cpu(sb->s_inodes_count),
        .f_ffree = le32_to_cpu(sb->s_free_inodes_count),
        .f_favail = le32_to_cpu(sb->s_free_inodes_count),
        .f_fsid = sb->s_magic,
        .f_flag = 0,
        .f_namemax = EXT2_NAME_LEN,
    };

    fuse_reply_statfs(req, &stbuf);
}

static void ext2_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                   size_t size)
{
}

static void ext2_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size)
{
}

static void ext2_bmap(fuse_req_t req, fuse_ino_t ino, size_t blocksize,
               uint64_t idx)
{
    /* fuse_reply_bmap(req, idx) */
}

static struct fuse_lowlevel_ops ext2_ops = {
    .lookup = ext2_lookup,
    .getattr = ext2_getattr,
    .readlink = ext2_readlink,
    .open = ext2_open,
    .read = ext2_read,
    .release = ext2_release,
    .opendir = ext2_opendir,
    .readdir = ext2_readdir,
    .releasedir = ext2_releasedir,
    .statfs = ext2_statfs,
/*
    .getxattr = ext2_getxattr,
    .listxattr = ext2_listxattr,
    .bmap = ext2_bmap
*/
};

int main(int argc, char *argv[])
{
    struct ext2_info *ctx;
    int i, fuse_argc=0;
    char *device;
    struct fuse_session *sess;
    struct fuse_chan *chan;
    struct fuse_args args;
    char *mountpoint;
    int multithreaded;
    int foreground;
    int res;

    ctx = talloc(NULL, struct ext2_info);

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
        perror("ext2_fuse");
        return 2;
    }

    ctx->dev = fp;

    if (ext2_read_super(ctx))
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

    sess = fuse_lowlevel_new(&args, &ext2_ops, sizeof(ext2_ops), ctx);
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
