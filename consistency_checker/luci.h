/*
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  from
 *
 *  fs/ext2.h
 *
 *  Copyright (C) 2017 Saptarshi Sen
 */
#ifndef _LUCI_H_
#define _LUCI_H_

#include <linux/byteorder/little_endian.h>

typedef unsigned int __le32;

typedef unsigned int __u32;

typedef unsigned int u32;

typedef unsigned short int __le16;

typedef unsigned short int __u16;

typedef unsigned long long __le64;

typedef unsigned long long __u64;

typedef unsigned char __u8;

/* data type for filesystem-wide blocks number */
typedef unsigned long luci_fsblk_t;

/*
 * Structure of the super block
 */
struct luci_super_block {
    __le32  s_inodes_count;     /* Inodes count */
    __le32  s_blocks_count;     /* Blocks count */
    __le32  s_r_blocks_count;   /* Reserved blocks count */
    __le32  s_free_blocks_count;    /* Free blocks count */
    __le32  s_free_inodes_count;    /* Free inodes count */
    __le32  s_first_data_block; /* First Data Block */
    __le32  s_log_block_size;   /* Block size */
    __le32  s_log_frag_size;    /* Fragment size */
    __le32  s_blocks_per_group; /* # Blocks per group */
    __le32  s_frags_per_group;  /* # Fragments per group */
    __le32  s_inodes_per_group; /* # Inodes per group */
    __le32  s_mtime;        /* Mount time */
    __le32  s_wtime;        /* Write time */
    __le16  s_mnt_count;        /* Mount count */
    __le16  s_max_mnt_count;    /* Maximal mount count */
    __le16  s_magic;        /* Magic signature */
    __le16  s_state;        /* File system state */
    __le16  s_errors;       /* Behaviour when detecting errors */
    __le16  s_minor_rev_level;  /* minor revision level */
    __le32  s_lastcheck;        /* time of last check */
    __le32  s_checkinterval;    /* max. time between checks */
    __le32  s_creator_os;       /* OS */
    __le32  s_rev_level;        /* Revision level */
    __le16  s_def_resuid;       /* Default uid for reserved blocks */
    __le16  s_def_resgid;       /* Default gid for reserved blocks */
    /*
     * These fields are for LUCI_DYNAMIC_REV superblocks only.
     *
     * Note: the difference between the compatible feature set and
     * the incompatible feature set is that if there is a bit set
     * in the incompatible feature set that the kernel doesn't
     * know about, it should refuse to mount the filesystem.
     *
     * e2fsck's requirements are more strict; if it doesn't know
     * about a feature in either the compatible or incompatible
     * feature set, it must abort and not try to meddle with
     * things it doesn't understand...
     */
    __le32  s_first_ino;        /* First non-reserved inode */
    __le16  s_inode_size;      /* size of inode structure */
    __le16  s_block_group_nr;   /* block group # of this superblock */
    __le32  s_feature_compat;   /* compatible feature set */
    __le32  s_feature_incompat;     /* incompatible feature set */
    __le32  s_feature_ro_compat;    /* readonly-compatible feature set */
    __u8    s_uuid[16];     /* 128-bit uuid for volume */
    char    s_volume_name[16];  /* volume name */
    char    s_last_mounted[64];     /* directory where last mounted */
    __le32  s_algorithm_usage_bitmap; /* For compression */
    /*
     * Performance hints.  Directory preallocation should only
     * happen if the LUCI_COMPAT_PREALLOC flag is on.
     */
    __u8    s_prealloc_blocks;  /* Nr of blocks to try to preallocate*/
    __u8    s_prealloc_dir_blocks;  /* Nr to preallocate for dirs */
    __u16   s_padding1;
    /*
     * Journaling support valid if EXT3_FEATURE_COMPAT_HAS_JOURNAL set.
     */
    __u8    s_journal_uuid[16]; /* uuid of journal superblock */
    __u32   s_journal_inum;     /* inode number of journal file */
    __u32   s_journal_dev;      /* device number of journal file */
    __u32   s_last_orphan;      /* start of list of inodes to delete */
    __u32   s_hash_seed[4];     /* HTREE hash seed */
    __u8    s_def_hash_version; /* Default hash version to use */
    __u8    s_reserved_char_pad;
    __u16   s_reserved_word_pad;
    __le32  s_default_mount_opts;
    __le32  s_first_meta_bg;    /* First metablock block group */
    __u32   s_reserved[189];    /* Padding to the end of the block */
    __u32   s_checksum;         /* Borrow reserved for adding csum */
};


/*
 * Constants relative to the data blocks
 */
#define LUCI_NDIR_BLOCKS        2 // ((12 + 1 + 1) * 32)/sizeof(blkptr)
#define LUCI_IND_BLOCK          (LUCI_NDIR_BLOCKS)
#define LUCI_DIND_BLOCK         (LUCI_IND_BLOCK + 1)
#define LUCI_TIND_BLOCK         (LUCI_DIND_BLOCK + 1)
#define LUCI_N_BLOCKS           (LUCI_TIND_BLOCK + 1)

/*
 * Block pointer defintion
 */
typedef struct blkptr {
    __le32 blockno;
    __le16 length;
    __le32 checksum;
    __le32 birth;
    __le16 flags;
}__attribute__ ((aligned (8), packed)) blkptr;

#define COMPR_LEN(bp) (bp->length)

static void inline
bp_reset(blkptr *bp, unsigned long block, unsigned int size,
         unsigned short flags, u32 checksum) {
    bp->blockno = block;
    bp->length = size;
    bp->flags = flags;
    bp->checksum = checksum;
}

/*
 * Structure of an inode on the disk
 */
struct luci_inode {
    __le16  i_mode;     /* File mode */
    __le16  i_uid;      /* Low 16 bits of Owner Uid */
    __le32  i_size;     /* Size in bytes */
    __le32  i_atime;    /* Access time */
    __le32  i_ctime;    /* Creation time */
    __le32  i_mtime;    /* Modification time */
    __le32  i_dtime;    /* Deletion Time */
    __le16  i_gid;      /* Low 16 bits of Group Id */
    __le16  i_links_count;  /* Links count */
    __le32  i_blocks;   /* Blocks count */
    __le32  i_flags;    /* File flags */
    union {
        struct {
            __le32  l_i_reserved1; // used by compression to store block count
        } linux1;
        struct {
            __le32  h_i_translator;
        } hurd1;
        struct {
            __le32  m_i_reserved1;
        } masix1;
    } osd1;             /* OS dependent 1 */
    blkptr  i_block[LUCI_N_BLOCKS];/* Pointers to blocks */
    __le32  i_generation;   /* File version (for NFS) */
    __le32  i_file_acl; /* File ACL */
    __le32  i_dir_acl;  /* Directory ACL */
    __le32  i_faddr;    /* Fragment address */
    union {
        struct {
            __u8    l_i_frag;   /* Fragment number */
            __u8    l_i_fsize;  /* Fragment size */
            __u16   i_pad1;
            __le16  l_i_uid_high;   /* these 2 fields    */
            __le16  l_i_gid_high;   /* were reserved2[0] */
            __u32   l_i_reserved2;
        } linux2;
        struct {
            __u8    h_i_frag;   /* Fragment number */
            __u8    h_i_fsize;  /* Fragment size */
            __le16  h_i_mode_high;
            __le16  h_i_uid_high;
            __le16  h_i_gid_high;
            __le32  h_i_author;
        } hurd2;
        struct {
            __u8    m_i_frag;   /* Fragment number */
            __u8    m_i_fsize;  /* Fragment size */
            __u16   m_pad1;
            __u32   m_i_reserved2[2];
        } masix2;
    } osd2;             /* OS dependent 2 */
};

/*
 * Structure of a blocks group descriptor
 */
struct luci_group_desc
{
    __le32  bg_block_bitmap;        /* Blocks bitmap block */
    __le32  bg_inode_bitmap;        /* Inodes bitmap block */
    __le32  bg_inode_table;     /* Inodes table block */
    __le16  bg_free_blocks_count;   /* Free blocks count */
    __le16  bg_free_inodes_count;   /* Free inodes count */
    __le16  bg_used_dirs_count; /* Directories count */
    __le16  bg_pag;
    __le16  bg_block_bitmap_checksum;
    __le16  bg_inode_bitmap_checksum;
    __le16  bg_inode_table_checksum;
    __le16  bg_checksum;
    __le32  bg_reserved;
}__attribute__ ((aligned (8), packed));

static inline luci_fsblk_t
luci_group_first_block_no(struct luci_super_block *lsb, unsigned long group_no)
{
    return (group_no * lsb->s_blocks_per_group) +
                __le32_to_cpu(lsb->s_first_data_block);
}

/*
 * Structure of a directory entry
 */

struct luci_dir_entry {
    __le32  inode;          /* Inode number */
    __le16  rec_len;        /* Directory entry length */
    __le16  name_len;       /* Name length */
    char    name[];         /* File name, up to LUCI_NAME_LEN */
};

/*
 * The new version of the directory entry.  Since LUCI structures are
 * stored in intel byte order, and the name_len field could never be
 * bigger than 255 chars, it's safe to reclaim the extra byte for the
 * file_type field.
 */
struct luci_dir_entry_2 {
    __le32  inode;          /* Inode number */
    __le16  rec_len;        /* Directory entry length */
    __u8    name_len;       /* Name length */
    __u8    file_type;
    char    name[];         /* File name, up to LUCI_NAME_LEN */
};

/*
 * Ext2 directory file types.  Only the low 3 bits are used.  The
 * other bits are reserved for now.
 */
enum {
    LUCI_FT_UNKNOWN     = 0,
    LUCI_FT_REG_FILE    = 1,
    LUCI_FT_DIR         = 2,
    LUCI_FT_CHRDEV      = 3,
    LUCI_FT_BLKDEV      = 4,
    LUCI_FT_FIFO        = 5,
    LUCI_FT_SOCK        = 6,
    LUCI_FT_SYMLINK     = 7,
    LUCI_FT_MAX
};

static inline unsigned ilog2(unsigned size) {
        int p = 0;
        unsigned bytes = size;
        for (; bytes > 1; bytes >>= 1, p++);
        return p;
}

#define LUCI_MAX_DEPTH                    4
#define LUCI_BLOCK_SIZE(lsb)              (1024U << __le32_to_cpu((lsb)->s_log_block_size))
#define LUCI_BLOCK_SIZE_BITS(lsb)         ilog2(LUCI_BLOCK_SIZE((lsb)))
#define LUCI_ADDR_PER_BLOCK(lsb)          (LUCI_BLOCK_SIZE(lsb) / sizeof (blkptr))
#define LUCI_ADDR_PER_BLOCK_BITS(lsb)     (ilog2(LUCI_ADDR_PER_BLOCK(lsb)))
#define LUCI_BLKPTR_SIZE                  (sizeof(blkptr))

/*
 *  * LUCI_DIR_PAD defines the directory entries boundaries
 *   *
 *    * NOTE: It must be a multiple of 4
 *     */
#define LUCI_DIR_PAD                4
#define LUCI_DIR_ROUND              (LUCI_DIR_PAD - 1)
#define LUCI_DIR_REC_LEN(name_len)  (((name_len) + 8 + LUCI_DIR_ROUND) & ~LUCI_DIR_ROUND)
#define LUCI_MAX_REC_LEN            ((1<<16) - 1)

#define LUCI_NAME_LEN           255
#define LUCI_SUPER_MAGIC        0xEF53
#define LUCI_LINK_MAX           32000

/*
 *  * Special inode numbers
 *   */
#define LUCI_BAD_INO            1  /* Bad blocks inode */
#define LUCI_ROOT_INO           2  /* Root inode */
#define LUCI_BOOT_LOADER_INO    5  /* Boot loader inode */
#define LUCI_UNDEL_DIR_INO      6  /* Undelete directory inode */

#endif
