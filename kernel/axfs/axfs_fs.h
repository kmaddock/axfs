/* 2007-12-20: File added and changed by Sony Corporation */
/*
 * Advanced XIP File System for Linux - AXFS
 *   Readonly, compressed, and XIP filesystem for Linux systems big and small
 *
 * Copyright(c) 2006 - 2007 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Authors:
 *  Eric Anderson
 *  Jared Hulbert <jaredeh@gmail.com>
 *  Sujaya Srinivasan
 *  Justin Treon
 *
 * More info and current contacts at http://axfs.sourceforge.net
 *
 * Borrowed heavily from fs/cramfs/inode.c by Linus Torvalds
 *
 * axfs_fs.h -
 *   Contains the core filesystem routines with the major exception of the
 *   mounting infrastructure.
 *
 */

#ifndef __AXFS_H
#define __AXFS_H

#ifndef CLEAN_VERSION
#include <linux/version.h> /* For multi-version support */
#endif
#include "linux/rwsem.h"

#define AXFS_MAGIC	0x48A0E4CD	/* some random number */
#define AXFS_SIGNATURE	"Advanced XIP FS"
#define AXFS_MAXPATHLEN 255

#define TRUE 	1
#define FALSE 	0

/* Uncompression interfaces to the underlying zlib */
int axfs_uncompress_block(void *dst, int dstlen, void *src, int srclen);
int axfs_uncompress_init(void);
int axfs_uncompress_exit(void);

#ifdef CONFIG_SNSC_DEBUG_AXFS
int axfs_xip_record(unsigned char *name, unsigned long physaddr,
		    unsigned long virtaddr, unsigned int size,
		    unsigned long pgprot);
#endif

#ifdef CONFIG_AXFS_PROFILING
struct axfs_profiling_data {
	u64 inode_number;
	unsigned long count;
};
#endif

enum axfs_node_types {
	XIP = 0,
	Compressed,
	Byte_Aligned,
};

/*
 * axfs_metadata_ptrs is a in core container used to find the on media
 * representation of in core structs above
 */
struct axfs_metadata_ptrs_incore {
	u8 * node_type;
	u8 * node_index[8];
	u8 * cnode_offset[4];
	u8 * cnode_index[8];
	u8 * banode_offset[8];
	u8 * cblock_offset[8];
	u8 * inode_file_size[8];
	u8 * inode_name_offset[8];
	u8 * inode_num_entries[8];
	u8 * inode_mode_index[8];
	u8 * inode_array_index[8];
	u8 * mode[4];
	u8 * uid[4];
	u8 * gid[4];
};

/*
 *  on media struct describing a data region
 */
struct axfs_region_desc_onmedia {
	u64 fsoffset;
	u64 size;
	u64 compressed_size;
	u64 max_index;
	u8 table_byte_depth;
	u8 incore;
};

struct axfs_region_desc_incore {
	u64 fsoffset;
	u64 size;
	u64 compressed_size;
	u64 max_index;
	void * virt_addr;
	u8 table_byte_depth;
	u8 incore;
};

struct axfs_fill_super_info {
	struct axfs_super_onmedia * onmedia_super_block;
	unsigned long physical_start_address;
	unsigned long virtual_start_address;
};

/*
 * axfs_super is the on media format for the super block it must be big endian
 */
struct axfs_super_onmedia {
	u32 magic;				/* 0x48A0E4CD - random number */
	u8 signature[16];		/* "Advanced XIP FS" */
	u8 digest[40];			/* sha1 digest for checking data integrity*/
	u32 cblock_size;		/* maximum size of the compression granularity */
	u64 files;				/* number of inodes/files in fs */
	u64 size;				/* total image size */
	u64 blocks;				/* number of nodes in fs */
	u64 mmap_size;			/* size of the memory mapped part of FS image */
	u64 strings;			/* offset to struct describing strings region */
	u64 xip;				/* offset to struct describing xip region */
	u64 byte_aligned;		/* offset to struct for byte aligned region */
	u64 compressed;			/* offset to struct for the compressed region */
	u64 node_type;			/* offset to struct for node type table */
	u64 node_index;			/* offset to struct for node index tables */
	u64 cnode_offset;		/* offset to struct for cnode offset tables */
	u64 cnode_index;		/* offset to struct for cnode index tables */
	u64 banode_offset;		/* offset to struct for banode offset tables */
	u64 cblock_offset;		/* offset to struct for cblock offset tables */
	u64 inode_file_size;	/* offset to struct for inode file size tables */
	u64 inode_name_offset;	/* offset to struct for inode num_entries tables */
	u64 inode_num_entries;	/* offset to struct for inode num_entries tables */
	u64 inode_mode_index;	/* offset to struct for inode mode index tables */
	u64 inode_array_index;	/* offset to struct for inode node index tables */
	u64 modes;				/* offset to struct for mode mode tables */
	u64 uids;				/* offset to struct for mode uid index tables */
	u64 gids;				/* offset to struct for mode gid index tables */
	u8 version_major;
	u8 version_minor;
	u8 version_sub;
};

/*
 * axfs super-block data in core
 */
struct axfs_super_incore {
	u32 magic;
	u8 version_major;
	u8 version_minor;
	u8 version_sub;
	u8 padding;
	u64 files;
	u64 size;
	u64 blocks;
	u64 mmap_size;
	struct axfs_metadata_ptrs_incore * metadata;
	struct axfs_region_desc_incore strings;
	struct axfs_region_desc_incore xip;
	struct axfs_region_desc_incore compressed;
	struct axfs_region_desc_incore byte_aligned;
	struct axfs_region_desc_incore node_type;
	struct axfs_region_desc_incore node_index;
	struct axfs_region_desc_incore cnode_offset;
	struct axfs_region_desc_incore cnode_index;
	struct axfs_region_desc_incore banode_offset;
	struct axfs_region_desc_incore cblock_offset;
	struct axfs_region_desc_incore inode_file_size;
	struct axfs_region_desc_incore inode_name_offset;
	struct axfs_region_desc_incore inode_num_entries;
	struct axfs_region_desc_incore inode_mode_index;
	struct axfs_region_desc_incore inode_array_index;
	struct axfs_region_desc_incore modes;
	struct axfs_region_desc_incore uids;
	struct axfs_region_desc_incore gids;
	unsigned long phys_start_addr;
	unsigned long virt_start_addr;
	u32 cblock_size;
	u64 current_cnode_index;
	void * cblock_buffer[2];
	struct compat_rw_semaphore lock;
#ifdef CONFIG_AXFS_PROFILING
	struct axfs_profiling_data *profile_data_ptr;
	int profiling_on; 		/* Determines if profiling is on or off */
#endif
};


#define AXFS_U64_STITCH(data_ptr,index) \
    ( (((data_ptr)[0]) == 0) ? 0 : (u64)(((data_ptr)[0])[(index)]) ) \
  + ( (((data_ptr)[1]) == 0) ? 0 : ((u64)(((data_ptr)[1])[(index)]) << 8) ) \
  + ( (((data_ptr)[2]) == 0) ? 0 : ((u64)(((data_ptr)[2])[(index)]) << 16) ) \
  + ( (((data_ptr)[3]) == 0) ? 0 : ((u64)(((data_ptr)[3])[(index)]) << 24) ) \
  + ( (((data_ptr)[4]) == 0) ? 0 : ((u64)(((data_ptr)[4])[(index)]) << 32) ) \
  + ( (((data_ptr)[5]) == 0) ? 0 : ((u64)(((data_ptr)[5])[(index)]) << 40) ) \
  + ( (((data_ptr)[6]) == 0) ? 0 : ((u64)(((data_ptr)[6])[(index)]) << 48) ) \
  + ( (((data_ptr)[7]) == 0) ? 0 : ((u64)(((data_ptr)[7])[(index)]) << 56) )

#define AXFS_U32_STITCH(data_ptr,index) \
    ( (((data_ptr)[0]) == 0) ? 0 : (u32)(((data_ptr)[0])[index]) ) \
  + ( (((data_ptr)[1]) == 0) ? 0 : ((u32)(((data_ptr)[1])[index]) << 8) ) \
  + ( (((data_ptr)[2]) == 0) ? 0 : ((u32)(((data_ptr)[2])[index]) << 16) ) \
  + ( (((data_ptr)[3]) == 0) ? 0 : ((u32)(((data_ptr)[3])[index]) << 24) )

#define AXFS_GET_NODE_INDEX(data_ptrs,node__index) \
    AXFS_U64_STITCH( ((data_ptrs)->node_index), (node__index) )

#define AXFS_GET_NODE_TYPE(data_ptrs,node_index) \
    ( (u8)((data_ptrs)->node_type)[(node_index)] )

#define AXFS_GET_CNODE_INDEX(data_ptrs,cnode__index) \
    AXFS_U64_STITCH( ((data_ptrs)->cnode_index), (cnode__index) )

#define AXFS_GET_BANODE_OFFSET(data_ptrs,banode__index) \
    AXFS_U64_STITCH( ((data_ptrs)->banode_offset), (banode__index) )

#define AXFS_GET_CNODE_OFFSET(data_ptrs,cnode_index) \
    AXFS_U32_STITCH( ((data_ptrs)->cnode_offset), (cnode_index) )

#define AXFS_GET_CBLOCK_OFFSET(data_ptrs,cblock_index) \
    AXFS_U64_STITCH( ((data_ptrs)->cblock_offset), (cblock_index) )

#define AXFS_GET_INODE_FILE_SIZE(data_ptrs,inode_index) \
    AXFS_U64_STITCH( ((data_ptrs)->inode_file_size), (inode_index) )

#define AXFS_GET_INODE_NAME_OFFSET(data_ptrs,inode_index) \
    AXFS_U64_STITCH( ((data_ptrs)->inode_name_offset), (inode_index) )

#define AXFS_GET_INODE_NUM_ENTRIES(data_ptrs,inode_index) \
    AXFS_U64_STITCH( ((data_ptrs)->inode_num_entries), (inode_index) )

#define AXFS_GET_INODE_MODE_INDEX(data_ptrs,inode_index) \
    AXFS_U64_STITCH( ((data_ptrs)->inode_mode_index), (inode_index) )

#define AXFS_GET_INODE_ARRAY_INDEX(data_ptrs,inode_index) \
    AXFS_U64_STITCH( ((data_ptrs)->inode_array_index), (inode_index) )

#define AXFS_GET_MODE(data_ptrs,inode_index) \
    AXFS_U32_STITCH( ((data_ptrs)->mode),\
    (AXFS_GET_INODE_MODE_INDEX((data_ptrs),(inode_index))) )

#define AXFS_GET_UID(data_ptrs,inode_index) \
    AXFS_U32_STITCH( ((data_ptrs)->uid),\
    (AXFS_GET_INODE_MODE_INDEX((data_ptrs),(inode_index))) )

#define AXFS_GET_GID(data_ptrs,inode_index) \
    AXFS_U32_STITCH( ((data_ptrs)->gid),\
    (AXFS_GET_INODE_MODE_INDEX((data_ptrs),(inode_index))) )

#define AXFS_IS_REGION_COMPRESSED(_region) \
    (( \
     ((struct axfs_region_desc_incore *)(_region))->compressed_size > \
     0 \
    ) ? TRUE : FALSE )

#define AXFS_PHYSADDR_IS_VALID(sbi) \
    (((((struct axfs_super_incore *)(sbi))->phys_start_addr) > 0 \
	) ? TRUE : FALSE )

#define AXFS_VIRTADDR_IS_VALID(sbi) \
    (((((struct axfs_super_incore *)(sbi))->virt_start_addr) > 0 \
	) ? TRUE : FALSE )

#define AXFS_IS_MMAPABLE(sbi,offset) \
    ((\
       (((struct axfs_super_incore *)(sbi))->mmap_size) >= \
       (offset) \
    ) ? TRUE : FALSE )

#define AXFS_IS_OFFSET_MMAPABLE(sbi,offset) \
    (( \
     AXFS_IS_MMAPABLE(sbi,offset) && ( AXFS_PHYSADDR_IS_VALID(sbi) || AXFS_VIRTADDR_IS_VALID(sbi) ) \
    ) ? TRUE : FALSE )

#define AXFS_IS_REGION_MMAPABLE(sbi,_region) \
    (( \
      AXFS_IS_MMAPABLE(sbi,((struct axfs_region_desc_incore *)(_region))->fsoffset) && \
	  ( AXFS_PHYSADDR_IS_VALID(sbi) || AXFS_VIRTADDR_IS_VALID(sbi) ) \
     ) ? TRUE : FALSE )

#define AXFS_IS_REGION_INCORE(_region) \
    (((_region)->incore > 0) ? TRUE : FALSE )

#define AXFS_IS_REGION_XIP(sbi,_region) \
    (( \
     !AXFS_IS_REGION_COMPRESSED(_region) && \
     !AXFS_IS_REGION_INCORE(_region) && \
     AXFS_IS_REGION_MMAPABLE(sbi,_region) \
    ) ? TRUE : FALSE )

#define AXFS_SB(a) (a)->s_fs_info

#define AXFS_GET_XIP_REGION_PHYSADDR(sbi) \
	(unsigned long)((sbi)->phys_start_addr + (sbi)->xip.fsoffset)

#define AXFS_GET_INODE_NAME_ADDRESS(sbi,inode_index) \
	(unsigned long)((sbi)->strings.virt_addr + AXFS_GET_INODE_NAME_OFFSET((sbi)->metadata,inode_index))

#define AXFS_GET_CBLOCK_ADDRESS(sbi, cnode_index)\
	(unsigned long)((sbi)->compressed.virt_addr+AXFS_GET_CBLOCK_OFFSET((sbi)->metadata, cnode_index))

#define AXFS_GET_NODE_ADDRESS(sbi,node__index) \
    (unsigned long)((sbi)->node_index.virt_addr+AXFS_GET_NODE_INDEX((sbi)->metadata, node__index))

#define AXFS_GET_BANODE_ADDRESS(sbi,banode_index) \
    (unsigned long)((sbi)->byte_aligned.virt_addr+AXFS_GET_BANODE_OFFSET((sbi)->metadata, banode_index))

#define AXFS_FSOFFSET_2_BLOCKOFFSET(sbi,fsoffset) \
	(( \
	  ((sbi)->phys_start_addr == 0) && ((sbi)->virt_start_addr == 0) \
	) ? (fsoffset) : (fsoffset - (sbi)->mmap_size) )

#define AXFS_GET_CBLOCK_LENGTH(data_ptrs,cblock_index) \
    (u64)( \
     (u64)AXFS_GET_CBLOCK_OFFSET((data_ptrs),((u64)(cblock_index)+(u64)1)) \
     - (u64)AXFS_GET_CBLOCK_OFFSET((data_ptrs),(cblock_index)) \
    )


#ifdef ioremap_cached
#define AXFS_REMAP ioremap_cached
#else
#define AXFS_REMAP ioremap
#endif
#define AXFS_UNMAP iounmap

#endif
