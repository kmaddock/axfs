/* 2009-01-16: File added and changed by Sony Corporation */
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
 * axfs_super.c -
 *   Contains the core code used to mount the fs.
 *
 */

#ifndef CLEAN_VERSION
#include <linux/version.h> /* For multi-version support */
#endif

#include <linux/fs.h>
#include <linux/mount.h>
#include <linux/ctype.h>
#include <linux/vmalloc.h>
#include <linux/namei.h>
#include <linux/buffer_head.h>
#include <linux/statfs.h>
#include <linux/parser.h>
#include <asm/io.h>

#include <linux/axfs_fs.h>

#ifdef CONFIG_MTD
#include <linux/mtd/mtd.h>
#include <linux/mtd/map.h>
#include <linux/mtd/partitions.h>
#endif


#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

/******************** Function Declarations ****************************/
static void * axfs_fetch_data(struct super_block *sb, u64 offset, u64 len);
static int axfs_do_fill_data_ptrs(struct super_block *sb, u64 region_desc_offset, struct axfs_region_desc_incore *iregion, int force_va);
static void axfs_do_fill_metadata_ptrs(u8 **metadata, struct axfs_region_desc_incore *desc);
static void axfs_fill_metadata_ptrs(struct axfs_super_incore *sbi);
static int axfs_do_fill_super(struct super_block *sb, struct axfs_fill_super_info *fsi);
static int axfs_check_super(struct axfs_super_incore *sbi);
static struct axfs_fill_super_info * axfs_get_sb_physaddr(unsigned long physaddr);
static int axfs_fill_super(struct super_block *sb, void *data, int silent);
static struct axfs_fill_super_info * axfs_get_sb_mtdnr(int mtdnr);
static struct axfs_fill_super_info * axfs_get_sb_mtd(const char *dev_name);
static struct axfs_fill_super_info * axfs_get_sb_block(struct file_system_type *fs_type,
				   int flags, const char *dev_name, char *secondary_blk_dev);
static int parse_axfs_options(char *options, char **secondary_blk_dev, unsigned long *physaddr, unsigned long *virtaddr);
int axfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mnt);
static void axfs_put_super(struct super_block *sb);
static int axfs_remount(struct super_block *sb, int *flags, char *data);
static int axfs_statfs(struct dentry *dentry, struct kstatfs *buf);

/***************** functions in other axfs files ***************************/
#ifdef CONFIG_AXFS_PROFILING
extern int init_axfs_profiling(struct axfs_super_incore *sbi);
extern int shutdown_axfs_profiling(struct axfs_super_incore *sbi);
#endif
struct inode *axfs_create_vfs_inode(struct super_block *sb,int);
extern void axfs_copy_block_data(struct super_block *sb, void * dst_addr, u64 offset, u64 len);

/******************** Structure Declarations ****************************/
struct super_operations axfs_ops = {
	.put_super = axfs_put_super,
	.remount_fs = axfs_remount,
	.statfs = axfs_statfs,
};

/******************************************************************************
 *
 * axfs_fetch_mmapable_data
 *
 * Description:
 *   Fetches pages of data.  Only called when a page is in the mmaped region.
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer
 *
 *    (IN) fofffset - offset from the beginning of the filesystem
 *
 *    (IN) len - length to be fetched
 *
 * Returns:
 *    a vmalloc()'ed pointer with a copy of the requested data
 *
 *****************************************************************************/
static void *axfs_fetch_mmapable_data(struct axfs_super_incore *sbi, u64 fsoffset, u64 len) {
	unsigned long addr;
	void *copy_buffer;
	void *return_buffer;

	return_buffer = vmalloc((size_t)len);
	if(AXFS_PHYSADDR_IS_VALID(sbi)) {
		addr = sbi->phys_start_addr + (unsigned long)fsoffset;
		copy_buffer = AXFS_REMAP(addr, (unsigned long)len);
		memcpy(return_buffer, copy_buffer, (size_t)len);
		AXFS_UNMAP(copy_buffer);
	} else {
		addr = sbi->virt_start_addr + (unsigned long)fsoffset;
		memcpy(return_buffer, (void *)addr, (size_t)len);
	}

	return return_buffer;
}

/******************************************************************************
 *
 * axfs_fetch_block_data
 *
 * Description:
 *   Fetches pages of from the byte aligned data.
 *
 * Parameters:
 *    (IN) sb- superblock pointer
 *
 *    (IN) bofffset - offset from the beginning of the block device
 *
 *    (IN) len - length to be fetched
 *
 * Returns:
 *    a vmalloc()'ed pointer with a copy of the requested data
 *
 *****************************************************************************/
void *axfs_fetch_block_data(struct super_block *sb, u64 boffset, u64 len) {
	void *return_buffer;

	return_buffer = vmalloc((size_t)len);
	axfs_copy_block_data(sb, return_buffer, boffset, len);
	return return_buffer;
}

/******************************************************************************
 *
 * axfs_fetch_data
 *
 * Description:
 *   Copies data from the media, memory or block dev, to a buffer.
 *
 * Parameters:
 *    (IN) sb- superblock pointer
 *
 *    (IN) fofffset - offset from the beginning of the filesystem
 *
 *    (IN) len - length to be fetched
 *
 * Returns:
 *    a vmalloc()'ed pointer with a copy of the requested data
 *
 *****************************************************************************/
static void *axfs_fetch_data(struct super_block *sb, u64 fsoffset, u64 len) {
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 boffset;
	u64 end;
	u64 mmap_buffer_len;
	u64 blk_buffer_len;
	void *mmap_buffer;
	void *blk_buffer;
	void *return_buffer;

	end = fsoffset + len;

	if(AXFS_IS_OFFSET_MMAPABLE(sbi,fsoffset)){
		if(AXFS_IS_OFFSET_MMAPABLE(sbi,end))
			return axfs_fetch_mmapable_data(sbi,fsoffset,len);
		mmap_buffer_len = sbi->mmap_size - fsoffset;
		mmap_buffer = axfs_fetch_mmapable_data(sbi,fsoffset, mmap_buffer_len);
		blk_buffer_len = end - sbi->mmap_size;
		blk_buffer = axfs_fetch_block_data(sb, 0, blk_buffer_len);
		return_buffer = vmalloc((size_t)len);
		memcpy(return_buffer, mmap_buffer, mmap_buffer_len);
		memcpy((void *)((unsigned long)return_buffer + (unsigned long)mmap_buffer_len), blk_buffer, blk_buffer_len);
		vfree(blk_buffer);
		vfree(mmap_buffer);
		return return_buffer;
	}
	boffset = AXFS_FSOFFSET_2_BLOCKOFFSET(sbi,fsoffset);
	return axfs_fetch_block_data(sb, boffset, len);
}

/******************************************************************************
 *
 * axfs_do_fill_data_ptrs
 *
 * Description:
 *      Fills the incore region descriptor with data from the onmedia version.
 *    Processes the region to populate virt_addr by mapping to the physical
 *    address or copying the data to RAM or if the data can be fetched later,
 *    it is set to NULL.
 *
 * Parameters:
 *    (IN) sb - pointer to the super block on media
 *
 *    (IN) region_desc_offset - offset to the region descriptor from the
 *                                beginning of filesystem
 *
 *    (OUT) iregion - pointer to the in RAM copy of the region descriptor
 *
 *    (IN) force_va - if true the region must have a valid virt_addr
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_do_fill_data_ptrs(struct super_block *sb, u64 region_desc_offset, struct axfs_region_desc_incore *iregion, int force_va) {
	struct axfs_super_incore *sbi;
	struct axfs_region_desc_onmedia *oregion;
	unsigned long addr;
	void *mapped = NULL;
	int err;
	u64 size;
	u64 end;
	u64 struct_len = sizeof(*oregion);

	sbi = AXFS_SB(sb);

	oregion = (struct axfs_region_desc_onmedia *) axfs_fetch_data(sb,region_desc_offset,struct_len);

	iregion->fsoffset = be64_to_cpu(oregion->fsoffset);
	iregion->size = be64_to_cpu(oregion->size);
	iregion->compressed_size = be64_to_cpu(oregion->compressed_size);
	iregion->max_index = be64_to_cpu(oregion->max_index);
	iregion->table_byte_depth = oregion->table_byte_depth;
	iregion->incore = oregion->incore;
	vfree(oregion);

    if (iregion->size == 0)
		return 0;

	end = iregion->fsoffset + iregion->size;
	if( (AXFS_IS_REGION_XIP(sbi,iregion)) && !(force_va && (sbi->mmap_size < end)) ) {
		if(AXFS_PHYSADDR_IS_VALID(sbi) ) {
			addr = sbi->phys_start_addr;
			addr += (unsigned long)iregion->fsoffset;
			size = (sbi->mmap_size > (iregion->fsoffset + iregion->size)) ? iregion->size : (sbi->mmap_size - iregion->fsoffset);
			iregion->virt_addr = AXFS_REMAP(addr,size);
		} else {
			addr = sbi->virt_start_addr;
			addr += (unsigned long)iregion->fsoffset;
			iregion->virt_addr = (void *)addr;
		}
	} else if(AXFS_IS_REGION_INCORE(iregion) || AXFS_IS_REGION_COMPRESSED(iregion) || force_va) {
		iregion->virt_addr = vmalloc(iregion->size);
		if(!(iregion->virt_addr)) {
			err = -ENOMEM;
			goto out;
		}
		if(AXFS_IS_REGION_COMPRESSED(iregion)) {
			mapped = axfs_fetch_data(sb,iregion->fsoffset,iregion->compressed_size);
			err = axfs_uncompress_block(iregion->virt_addr,iregion->size,mapped,iregion->compressed_size);
			if (err)
				goto out;
		} else {
			mapped = axfs_fetch_data(sb,iregion->fsoffset,iregion->size);
			memcpy(iregion->virt_addr,mapped,iregion->size);
		}
	} else {
		iregion->virt_addr = NULL;
	}

	if(mapped)
		vfree(mapped);
	return 0;

out:
	if(mapped)
		vfree(mapped);
	if(iregion->virt_addr)
		vfree(iregion->virt_addr);
	return err;
}

/******************************************************************************
 *
 * axfs_do_fill_metadata_ptrs
 *
 * Description:
 *      Populates a part of the axfs_metadata_ptrs_incore structure for a given
 *    region
 *
 * Parameters:
 *    (OUT) metadata - pointer pointer to a segment of the
 *                       axfs_metadata_ptrs_incore
 *
 *    (IN) desc - pointer the a region descriptor
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
static void axfs_do_fill_metadata_ptrs(u8 **metadata, struct axfs_region_desc_incore *desc) {
	int i;
	u64 split;

	if (desc->size != 0) {
		if (desc->table_byte_depth == 0) {
			printk(KERN_ERR "axfs: Can't have ByteTable with size>0 and depth==0\n");
			BUG();
		}

		if (!(desc->virt_addr)) {
			printk(KERN_ERR "axfs: virtual address is NULL\n");
			BUG();
		}

		split = (unsigned long)desc->size / (unsigned int)desc->table_byte_depth;
		for(i=0;i<desc->table_byte_depth;i++) {
			metadata[i] = (u8 *)(desc->virt_addr + (i * (unsigned long)split));
		}
	}
}

/* hopefully makes axfs_fill_metadata_ptrs() easier to read */
#define metadata_helper(a,b) axfs_do_fill_metadata_ptrs((u8 **)&(a), &(b))

/******************************************************************************
 *
 * axfs_fill_metadata_ptrs
 *
 * Description:
 *      Populates the axfs_metadata_ptrs_incore structure
 *
 * Parameters:
 *    (OUT/IN) sbi - pointer to the axfs super block
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
static void axfs_fill_metadata_ptrs(struct axfs_super_incore *sbi) {
	struct axfs_metadata_ptrs_incore *metadata;

	metadata = sbi->metadata;

	metadata_helper(metadata->node_type, sbi->node_type);
	metadata_helper(metadata->node_index, sbi->node_index);
	metadata_helper(metadata->cnode_offset, sbi->cnode_offset);
	metadata_helper(metadata->cnode_index, sbi->cnode_index);
	metadata_helper(metadata->banode_offset, sbi->banode_offset);
	metadata_helper(metadata->cblock_offset, sbi->cblock_offset);
	metadata_helper(metadata->inode_file_size, sbi->inode_file_size);
	metadata_helper(metadata->inode_name_offset, sbi->inode_name_offset);
	metadata_helper(metadata->inode_num_entries, sbi->inode_num_entries);
	metadata_helper(metadata->inode_mode_index, sbi->inode_mode_index);
	metadata_helper(metadata->inode_array_index, sbi->inode_array_index);
	metadata_helper(metadata->mode, sbi->modes);
	metadata_helper(metadata->uid, sbi->uids);
	metadata_helper(metadata->gid, sbi->gids);
}

/* hopefully makes axfs_do_fill_super() easier to read */
#define fill_helper(a,b,c,d) axfs_do_fill_data_ptrs((a), be64_to_cpu(b), (struct axfs_region_desc_incore *) &(c), (d))

/******************************************************************************
 *
 * axfs_do_fill_super
 *
 * Description:
 *      Uses the data collected by axfs_get_sb() and populates the superblock
 *
 * Parameters:
 *    (OUT) sb - pointer to the super block
 *
 *    (IN) fsi - pointer to the axfs_fill_super_info struct
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_do_fill_super(struct super_block *sb, struct axfs_fill_super_info *fsi) {
	struct axfs_super_incore *sbi;
	struct axfs_super_onmedia *sbo;

#if 0
	u32 * pointer; /* REMOVEME */
	int i;
#endif
	sbo = fsi->onmedia_super_block;

	sbi = AXFS_SB(sb);

#if 0
	/* REMOVEME */
	printk("AXFS super block dump\n");
	pointer = (u32 *)sbo;
	for (i=0; i<128; i+=4)
	{
		printk("0x%06X: %08X %08X %08X %08X\n", (4*i), pointer[i], pointer[i+1], pointer[i+2], pointer[i+3]);
	}
#endif

	/* Do sanity checks on the superblock */
	if (be32_to_cpu(sbo->magic) != AXFS_MAGIC) {
		printk(KERN_ERR "axfs: wrong magic\n");
		goto out;
	}

	/* verify the signiture is correct */
	if (strncmp(sbo->signature, AXFS_SIGNATURE, sizeof(AXFS_SIGNATURE))) {
		printk(KERN_ERR "axfs: wrong axfs signature, read %s, expected %s\n", sbo->signature, AXFS_SIGNATURE);
		goto out;
	}

	sbi->magic = be32_to_cpu(sbo->magic);
	sbi->version_major = sbo->version_major;
	sbi->version_minor = sbo->version_minor;
	sbi->version_sub = sbo->version_sub;
	sbi->files = be64_to_cpu(sbo->files);
	sbi->size = be64_to_cpu(sbo->size);
	sbi->blocks = be64_to_cpu(sbo->blocks);
	sbi->mmap_size = be64_to_cpu(sbo->mmap_size);
	sbi->cblock_size = be32_to_cpu(sbo->cblock_size);

	if(fill_helper(sb, sbo->strings, sbi->strings, TRUE))
		goto out;
	if(fill_helper(sb, sbo->xip, sbi->xip, TRUE))
		goto out;
	if(fill_helper(sb, sbo->compressed, sbi->compressed, FALSE))
		goto out;
	if(fill_helper(sb, sbo->byte_aligned, sbi->byte_aligned, FALSE))
		goto out;
	if(fill_helper(sb, sbo->node_type, sbi->node_type, TRUE))
		goto out;
	if(fill_helper(sb, sbo->node_index, sbi->node_index, TRUE))
		goto out;
	if(fill_helper(sb, sbo->cnode_offset, sbi->cnode_offset, TRUE))
		goto out;
	if(fill_helper(sb, sbo->cnode_index, sbi->cnode_index, TRUE))
		goto out;
	if(fill_helper(sb, sbo->banode_offset, sbi->banode_offset, TRUE))
		goto out;
	if(fill_helper(sb, sbo->cblock_offset, sbi->cblock_offset, TRUE))
		goto out;
	if(fill_helper(sb, sbo->inode_file_size, sbi->inode_file_size, TRUE))
		goto out;
	if(fill_helper(sb, sbo->inode_name_offset, sbi->inode_name_offset, TRUE))
		goto out;
	if(fill_helper(sb, sbo->inode_num_entries, sbi->inode_num_entries, TRUE))
		goto out;
	if(fill_helper(sb, sbo->inode_mode_index, sbi->inode_mode_index, TRUE))
		goto out;
	if(fill_helper(sb, sbo->inode_array_index, sbi->inode_array_index, TRUE))
		goto out;
	if(fill_helper(sb, sbo->modes, sbi->modes, TRUE))
		goto out;
	if(fill_helper(sb, sbo->uids, sbi->uids, TRUE))
		goto out;
	if(fill_helper(sb, sbo->gids, sbi->gids, TRUE))
		goto out;

	axfs_fill_metadata_ptrs(sbi);

	init_rwsem(&sbi->lock); /* Semaphore for kernel premetion */

	return 0;

out:
	return -EINVAL;
}

/******************************************************************************
 *
 * axfs_check_super
 *
 * Description:
 *      Performs sanity checks on the axfs super block
 *
 * Parameters:
 *    (IN) sbi - pointer to the axfs super block
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_check_super(struct axfs_super_incore *sbi) {
	/* the root inode info structure is always the first in the inode struct area.
	 * now run some quick checks on it to make sure all is well
	 */

	/* Check that the root inode is in a sane state */
	if (!S_ISDIR(AXFS_GET_MODE(sbi->metadata,0))) {
		printk(KERN_ERR "axfs: root is not a directory\n");
		goto out;
	}
#ifdef CONFIG_SNSC_DEBUG_AXFS
	/* This is NOT error, but we can report it, not EXIT */
	if (AXFS_GET_INODE_NUM_ENTRIES(sbi->metadata,0) == 0) {
		printk(KERN_INFO "axfs: empty filesystem\n");
	}
#endif

	/* get the total size of the image */
	printk(KERN_INFO "axfs: axfs image appears to be %llu KB in size\n", (sbi->size) / 1024);

	return 0;

out:
	return -EINVAL;
}

/******************************************************************************
 *
 * axfs_fill_super
 *
 * Description:
 *     Populates the VFS super block Structure.
 *
 * Parameters:
 *    (OUT) sb - superblock of fs instance
 *
 *    (IN) data - used to pass a pointer to the axfs_fill_super_info
 *
 *    (IN) silent - not used
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct axfs_super_incore *sbi;
	struct axfs_metadata_ptrs_incore *metadata = NULL;
	struct axfs_fill_super_info *fsi = NULL;
	struct inode *root;
	int err;

	fsi = (struct axfs_fill_super_info *)data;

	/*
	 * Create the space for the private super block runtime information and store it
	 * in the VFS super block structure
	 */
	sbi = vmalloc(sizeof(*sbi));
	if (!sbi) {
		err = -ENOMEM;
		goto out;
	}
	memset(sbi, 0, sizeof(*sbi));
	sb->s_fs_info = sbi;

	sb->s_flags |= MS_RDONLY;
	sbi->metadata = vmalloc(sizeof(*metadata));
	metadata = sbi->metadata;
	if (!metadata) {
		err = -ENOMEM;
		goto out;
	}
	memset(metadata, 0, sizeof(*metadata));

	sbi->phys_start_addr=fsi->physical_start_address;
	sbi->virt_start_addr=fsi->virtual_start_address;

	printk(KERN_INFO "axfs: start axfs_do_fill_super\n");
	/* fully populate the incore superblock structures */
	err = axfs_do_fill_super(sb,fsi);
	if(err != 0)
		goto out;

	printk(KERN_INFO "axfs: doned axfs_do_fill_super\n");

	err = axfs_check_super(sbi);
	if(err != 0)
		goto out;

	printk(KERN_INFO "axfs: doned axfs_check_super\n");

	/* Setup the VFS super block now */
	sb->s_op = &axfs_ops;
	root = axfs_create_vfs_inode(sb, 0);
	if(!root) {
		err = -EINVAL;
		goto out;
	}

	sb->s_root = d_alloc_root(root);
	if(!sb->s_root) {
		iput(root);
		err = -EINVAL;
		goto out;
	}

#ifdef CONFIG_AXFS_PROFILING
	init_axfs_profiling(sbi);
#endif

	sbi->cblock_buffer[0] = vmalloc(sbi->cblock_size);
	sbi->cblock_buffer[1] = vmalloc(sbi->cblock_size);
	if ((!sbi->cblock_buffer[0]) || (!sbi->cblock_buffer[1])) {
		iput(root);
		err = -ENOMEM;
		goto out;
	}

	sbi->current_cnode_index = -1;

	vfree(fsi->onmedia_super_block);
	vfree(fsi);
	return 0;

out:
	vfree(fsi->onmedia_super_block);
	vfree(fsi);
	vfree(metadata);
	vfree(sbi);
	sb->s_fs_info = NULL;
	return err;
}

/******************************************************************************
 *
 * axfs_get_sb_physaddr
 *
 * Description:
 *      Populates a axfs_fill_super_info struct after sanity checking physaddr
 *
 * Parameters:
 *    (IN) physaddr - the physical address of to check
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static struct axfs_fill_super_info * axfs_get_sb_physaddr(unsigned long physaddr)
{
	void *buffer;
	struct axfs_fill_super_info *output;
	int err = -EINVAL;

	if (physaddr == 0)
		goto out;

	if (physaddr & (PAGE_SIZE - 1)) {
		printk(KERN_ERR
		       "axfs: physical address 0x%lx for axfs image isn't aligned to a page boundary\n",
		       physaddr);
		goto out;
	}

	printk(KERN_INFO
	       "axfs: checking physical address 0x%lx for axfs image\n",
	       physaddr);

	buffer = AXFS_REMAP(physaddr, PAGE_SIZE);

	if(buffer == NULL) {
		printk(KERN_ERR "axfs: ioremap() failed at physical address 0x%lx\n", physaddr);
		goto out;
	}

	output = (struct axfs_fill_super_info *) vmalloc(sizeof(struct axfs_fill_super_info));
	output->onmedia_super_block = (struct axfs_super_onmedia *) vmalloc(sizeof(struct axfs_super_onmedia));
	memcpy((void *)output->onmedia_super_block, (void *)buffer, sizeof(struct axfs_super_onmedia));
	iounmap(buffer);

	output->physical_start_address = physaddr;
	output->virtual_start_address = 0;

	return output;

out:
	return ERR_PTR(err);
}

/******************************************************************************
 *
 * axfs_get_sb_virtaddr
 *
 * Description:
 *      Populates a axfs_fill_super_info struct after sanity checking virtaddr
 *
 * Parameters:
 *    (IN) virtaddr - the virtual address to check for axfs image
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static struct axfs_fill_super_info *axfs_get_sb_virtaddr(unsigned long virtaddr)
{
	struct axfs_fill_super_info *output;
	int err;

	if (virtaddr == 0) {
		err = -EINVAL;
		goto out;
	}

	if (virtaddr & (PAGE_SIZE - 1)) {
		printk(KERN_ERR
		       "axfs: virtual address 0x%lx for axfs image isn't aligned to a page boundary\n",
		       virtaddr);
		err = -EINVAL;
		goto out;
	}

	printk(KERN_INFO
	       "axfs: checking virtual address 0x%lx for axfs image\n",
	       virtaddr);

	output = (struct axfs_fill_super_info *) vmalloc(sizeof(struct axfs_fill_super_info));
	output->onmedia_super_block = (struct axfs_super_onmedia *) vmalloc(sizeof(struct axfs_super_onmedia));
	memcpy((void *)output->onmedia_super_block, (void *)((unsigned long )virtaddr), sizeof(struct axfs_super_onmedia));

	output->physical_start_address = 0;
	output->virtual_start_address = virtaddr;

	return output;

out:
	return ERR_PTR(err);
}

#ifdef CONFIG_MTD
/******************************************************************************
 *
 * axfs_get_sb_mtdnr
 *
 * Description:
 *      Populates a axfs_fill_super_info struct after sanity checking the
 *    mtd device of device number
 *
 * Parameters:
 *    (IN) mtdnr - the mtd device number
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static struct axfs_fill_super_info *axfs_get_sb_mtdnr(int mtdnr)
{
	struct axfs_fill_super_info *output;
	struct mtd_info *mtd;
	void *axfsimage;
	size_t retlen;
	int err;

	mtd = get_mtd_device(NULL, mtdnr);
	if (!mtd) {
		printk(KERN_DEBUG
		       "axfs: MTD device #%u doesn't appear to exist\n", mtdnr);
		err = -EINVAL;
		goto out;
	}
	if (mtd->point) {
	} else {
		printk(KERN_DEBUG
		       "axfs: MTD device #%u doesn't support point()\n", mtdnr);
		err = -EINVAL;
		goto out;
	}
	if (mtd->unpoint) {
	} else {
		printk(KERN_DEBUG
		       "axfs: MTD device #%u doesn't support unpoint()\n",
		       mtdnr);
		err = -EINVAL;
		goto out;
	}

	if (!((mtd->point(mtd, 0, PAGE_SIZE, &retlen, &axfsimage, NULL)) == 0)) {
		err = -EACCES;
		goto out;
	}

	output = (struct axfs_fill_super_info *)vmalloc(sizeof(struct axfs_fill_super_info));
	if(!output) {
		err = - ENOMEM;
		goto out;
	}
	output->onmedia_super_block = (struct axfs_super_onmedia *)vmalloc(sizeof(struct axfs_super_onmedia));
	if(!output->onmedia_super_block) {
		err = - ENOMEM;
		goto out;
	}
	memcpy((void *)output->onmedia_super_block, axfsimage, sizeof(struct axfs_super_onmedia));

	mtd->unpoint(mtd, 0, retlen);

	output->physical_start_address = (unsigned long)mtd_get_partition_physaddr(mtd);
	output->virtual_start_address = 0;

	put_mtd_device(mtd);
	return output;

      out:
	put_mtd_device(mtd);
	return ERR_PTR(err);
}

/******************************************************************************
 *
 * axfs_get_sb_mtd
 *
 * Description:
 *      Populates a axfs_fill_super_info struct after sanity checking the
 *    mtd device given by a name
 *
 * Parameters:
 *    (IN) dev_name - the mtd device name
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static struct axfs_fill_super_info *axfs_get_sb_mtd(const char *dev_name)
{
	int mtdnr;
	struct nameidata nd;
	int err;
	struct axfs_fill_super_info *output;

	err = path_lookup(dev_name, LOOKUP_FOLLOW, &nd);

	if (!err) {
		/* Looking for a real and valid mtd device to attach to */
		if (imajor(nd.path.dentry->d_inode) == MTD_BLOCK_MAJOR) {
			mtdnr = iminor(nd.path.dentry->d_inode);
		} else if (imajor(nd.path.dentry->d_inode) == MTD_CHAR_MAJOR) {
			mtdnr = iminor(nd.path.dentry->d_inode);
			mtdnr = mtdnr / 2;
		} else if (nd.path.mnt->mnt_flags & MNT_NODEV) {
			err = -EACCES;
			goto out;
		} else {
			goto out;
		}
	} else if (dev_name[0] == 'm' && dev_name[1] == 't'
		   && dev_name[2] == 'd') {
		/* Mount from a mtd device but not a valid /dev/mtdXXX */
		if (dev_name[3] == ':') {
			struct mtd_info *mtd;
			int i;

			/* Mount by MTD device name */
			printk(KERN_DEBUG
			       "axfs_get_sb_mtd(): mtd:%%s, name \"%s\"\n",
			       dev_name + 4);
			mtdnr = -1;
			for (i = 0; i < MAX_MTD_DEVICES; i++) {
				mtd = get_mtd_device(NULL, i);
				if (mtd) {
					if (!strcmp(mtd->name, dev_name + 4)) {
						put_mtd_device(mtd);
						mtdnr = i;
						break;
					}
					put_mtd_device(mtd);
				}
			}
			if (mtdnr == -1) {
				printk(KERN_NOTICE
				       "axfs_get_sb_mtd(): MTD device with name \"%s\" not found.\n",
				       dev_name + 4);
				err = -EINVAL;
				goto out;
			}
		} else if (isdigit(dev_name[3])) {
			/* Mount by MTD device number */
			char *endptr;
			mtdnr = simple_strtoul(dev_name + 3, &endptr, 0);
			if (*endptr) {
				printk(KERN_NOTICE
				       "axfs_get_sb_mtd(): MTD device number \"%s\" not found.\n",
				       dev_name + 3);
				err = -EINVAL;
				goto out;
			}
		} else {
			err = -EINVAL;
			goto out;
		}
	} else {
		err = -EINVAL;
		goto out;
	}

	output = axfs_get_sb_mtdnr(mtdnr);
	if (IS_ERR(output)) {
		err = PTR_ERR(output);
		goto out;
	}

	path_put(&nd.path);
	return output;

out:
	path_put(&nd.path);
	printk(KERN_NOTICE "axfs_get_sb_mtd(): Invalid device \"%s\"\n",dev_name);
	return ERR_PTR(err);
}
#else

/* dummy used if MTD not compiled in */
static struct axfs_fill_super_info *axfs_get_sb_mtdnr(int mtdnr)
{
	return ERR_PTR(-EINVAL);
}

/* dummy used if MTD not compiled in */
static struct axfs_fill_super_info *axfs_get_sb_mtd(const char *dev_name)
{
	return axfs_get_sb_mtdnr(0);
}

#endif

/******************************************************************************
 *
 * axfs_get_sb_block
 *
 * Description:
 *      Populates a axfs_fill_super_info struct after sanity checking the
 *    block device
 *
 * Parameters:
 *    (IN) fs_type - pointer to file_system_type
 *
 *    (IN) flags - mount flags
 *
 *    (IN) dev_name - block device name passed in from mount
 *
 *    (IN) secondary_blk_dev - block device name enter from mount -o
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static struct axfs_fill_super_info *axfs_get_sb_block(struct file_system_type *fs_type,
				   int flags, const char *dev_name, char *secondary_blk_dev)
{
	struct nameidata nd;
	struct buffer_head *bh = NULL;
	struct axfs_fill_super_info *output;
	struct block_device *bdev = NULL;
	int err;

	err = path_lookup(dev_name, LOOKUP_FOLLOW, &nd);
	if (err)
		return ERR_PTR(-EINVAL);

	if (secondary_blk_dev) {
		printk(KERN_ERR "axfs: can't mount 2 block device's \"%s\" and \"%s\"\n", dev_name, secondary_blk_dev);
		err = -EINVAL;
		goto path_out;
	}

	if (!S_ISBLK(nd.path.dentry->d_inode->i_mode)) {
		err = -EINVAL;
		goto path_out;
	}

	if (nd.path.mnt->mnt_flags & MNT_NODEV) {
		err = -EACCES;
		goto path_out;
	}

	bdev = open_bdev_exclusive(dev_name, flags, fs_type);

	if (IS_ERR(bdev)){
		err = PTR_ERR(bdev);
		goto path_out;
	}

	bh = __bread(bdev, 0, bdev->bd_block_size);
	if (!bh) {
		err = -EIO;
		goto out;
	}

	output = (struct axfs_fill_super_info *)vmalloc(sizeof(struct axfs_fill_super_info));
	if(!output) {
		err = -ENOMEM;
		goto out;
	}
	output->onmedia_super_block = (struct axfs_super_onmedia *)vmalloc(sizeof(struct axfs_super_onmedia));
	if(!output->onmedia_super_block) {
		err = -ENOMEM;
		goto out;
	}
	memcpy((char *)output->onmedia_super_block, bh->b_data, sizeof(struct axfs_super_onmedia));

	if(IS_ERR(output)) {
		err = PTR_ERR(output);
		goto out;
	}

	output->physical_start_address = 0;
	output->virtual_start_address = 0;

	path_put(&nd.path);
	close_bdev_exclusive(bdev, flags);
	free_buffer_head(bh);

	return output;

out:
	close_bdev_exclusive(bdev,flags);
	if (bh) {
		free_buffer_head(bh);
	}

path_out:
	path_put(&nd.path);
	printk(KERN_NOTICE "axfs_get_sb_block(): Invalid device \"%s\"\n",
	       dev_name);
	return ERR_PTR(err);
}

/* helpers for parse_axfs_options */
enum {
	Option_err,
	Option_secondary_blk_dev,
	Option_physical_address_x,
	Option_physical_address_X,
	Option_iomem
};

/* helpers for parse_axfs_options */
static match_table_t tokens = {
	{Option_secondary_blk_dev, "block_dev=%s"},
	{Option_physical_address_x, "physaddr=0x%s"},
	{Option_physical_address_X, "physaddr=0X%s"},
	{Option_iomem, "iomem=%s"},
	{Option_err, NULL}
};
/******************************************************************************
 *
 * parse_axfs_options
 *
 * Description:
 *      Parses the mount -o options specific to axfs
 *
 * Parameters:
 *    (IN) options - mount -o options
 *
 *    (OUT) secondary_blk_dev - name of the block device containing part of
 *                               image
 *
 *    (OUT) physaddr - the physical address
 *
 *    (OUT) virtaddr - the virtual address
 *
 * Returns:
 *    pointer to a axfs_file_super_info or an error pointer
 *
 *****************************************************************************/
static int parse_axfs_options(char *options, char **secondary_blk_dev, unsigned long *physaddr, unsigned long *virtaddr)
{
	int err;
	char *p;
	substring_t args[MAX_OPT_ARGS];

	*secondary_blk_dev = NULL;
	*physaddr = 0;
	*virtaddr = 0;

	if(!options) {
		err = 0;
		goto out;
	}

        if ((p = strstr(options, "physaddr=")) == NULL ) {
                printk(KERN_ERR "axfs: physaddr option for axfs image is not specified\n");
		err = -EINVAL;
		goto out;
        }

	while ((p = (char *)strsep(&options, ",")) != NULL) {
		int token;
		if(!*p)
			continue;

		token = match_token(p, tokens, args);
		switch(token) {
			case Option_secondary_blk_dev:
				*secondary_blk_dev = (char *)match_strdup(&args[0]);
				if(!secondary_blk_dev) {
					err = -ENOMEM;
					goto out;
				}
				break;
			case Option_physical_address_x:
			case Option_physical_address_X:
				if(match_hex(&args[0],(int *)physaddr) != 0) {
					err = -EINVAL;
					goto out;
				}
				break;
			case Option_iomem:
			 default:
				printk(KERN_ERR
					"axfs: unrecognized mount option \"%s\" "
					"or missing value\n", p);
				err = -EINVAL;
				goto out;
		}
	}
	if(*physaddr)
		return 0;

	err = -EINVAL;
out:
	return err;
}

/******************************************************************************
 *
 * axfs_get_sb
 *
 * Description:
 *      After testing various mounting options and media mounts the image
 *
 * Parameters:
 *    (IN) fs_type - pointer to file_system_type
 *
 *    (IN) flags - mount flags
 *
 *    (IN) dev_name - block device name passed in from mount
 *
 *    (IN) data - pointer to a string containing mount options
 *
 *    (IN) mnt - VFS mount point
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
int axfs_get_sb(struct file_system_type *fs_type, int flags, const char *dev_name, void *data, struct vfsmount *mnt)
{
	char *secondary_blk_dev;
	struct axfs_fill_super_info *output;
	unsigned long physaddr;
	unsigned long virtaddr;
	int err;

	err = parse_axfs_options((char *)data, &secondary_blk_dev, &physaddr, &virtaddr);
	if(err != 0)
		return err;

#ifdef CONFIG_SNSC_FIRST_MOUNT_AXFS_AS_ROOTFS
	printk("AXFS: Checking AXFS filesystem at virtaddr: 0x%08lx "
	       "(physaddr=0x%08lx).\n", virtaddr, physaddr);
#endif

	/* Check if physaddr is valid */
	output = axfs_get_sb_physaddr(physaddr);
	if(!(IS_ERR(output)))
		goto out;

	/* Check if virtaddr is valid */
	output = axfs_get_sb_virtaddr(virtaddr);
	if(!(IS_ERR(output)))
		goto out;

	/* Next we assume there's a MTD device */
	output = axfs_get_sb_mtd(dev_name);
	if(!(IS_ERR(output)))
		goto out;

	/* Now we assume it's a block device */
	output = axfs_get_sb_block(fs_type, flags, dev_name, secondary_blk_dev);
	if(!(IS_ERR(output)))
	{
		return get_sb_bdev(fs_type, flags, dev_name, output, axfs_fill_super, mnt);
	}

	return PTR_ERR(output);

out:
	if(secondary_blk_dev) {
		return get_sb_bdev(fs_type, flags, secondary_blk_dev, output, axfs_fill_super, mnt);
	}
	return get_sb_nodev(fs_type, flags, output, axfs_fill_super, mnt);
}
/******************************************************************************
 *
 * axfs_free_region
 *
 * Description:
 *      Unmaps or frees memory used by a region
 *
 * Parameters:
 *    (IN) sbi - pointer to axfs super block
 *
 *    (IN) region - pointer to a region descriptor
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
static void axfs_free_region(struct axfs_super_incore *sbi, struct axfs_region_desc_incore *region) {
	if(AXFS_IS_REGION_XIP(sbi, region)) {
		AXFS_UNMAP(region->virt_addr);
	} else if(region->virt_addr) {
		vfree(region->virt_addr);
	}
}

/******************************************************************************
 *
 * axfs_put_super
 *
 * Description:
 *      After testing various mounting options and media mounts the image
 *
 * Parameters:
 *    (IN) sb - pointer to the super block
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
static void axfs_put_super(struct super_block *sb)
{
	struct axfs_super_incore *sbi = AXFS_SB(sb);

#ifdef CONFIG_AXFS_PROFILING
	shutdown_axfs_profiling(sbi);
#endif

	vfree(sbi->metadata);

    axfs_free_region(sbi,&sbi->strings);
    axfs_free_region(sbi,&sbi->xip);
    axfs_free_region(sbi,&sbi->compressed);
    axfs_free_region(sbi,&sbi->byte_aligned);
    axfs_free_region(sbi,&sbi->node_type);
    axfs_free_region(sbi,&sbi->node_index);
    axfs_free_region(sbi,&sbi->cnode_offset);
    axfs_free_region(sbi,&sbi->cnode_index);
    axfs_free_region(sbi,&sbi->banode_offset);
    axfs_free_region(sbi,&sbi->cblock_offset);
    axfs_free_region(sbi,&sbi->inode_file_size);
    axfs_free_region(sbi,&sbi->inode_name_offset);
    axfs_free_region(sbi,&sbi->inode_num_entries);
    axfs_free_region(sbi,&sbi->inode_mode_index);
    axfs_free_region(sbi,&sbi->inode_array_index);
    axfs_free_region(sbi,&sbi->modes);
    axfs_free_region(sbi,&sbi->uids);
    axfs_free_region(sbi,&sbi->gids);

	vfree(sbi->cblock_buffer[0]);
	vfree(sbi->cblock_buffer[1]);

	vfree(sbi);
	sbi = NULL;
}

/******************************************************************************
 *
 * axfs_remount
 *
 * Description:
 *      Returns flags modified to signal this is a readonly fs
 *
 *****************************************************************************/
static int axfs_remount(struct super_block *sb, int *flags, char *data)
{
	*flags |= MS_RDONLY;
	return 0;
}

/******************************************************************************
 *
 * axfs_statfs
 *
 * Description:
 *      Returns fs stats which are static
 *
 *****************************************************************************/
static int axfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct axfs_super_incore *sbi = AXFS_SB(dentry->d_sb);

	buf->f_type = AXFS_MAGIC;
	buf->f_bsize = PAGE_CACHE_SIZE;
	buf->f_blocks = sbi->blocks;
	buf->f_bfree = 0;
	buf->f_bavail = 0;
	buf->f_files = sbi->files;
	buf->f_ffree = 0;
	buf->f_namelen = AXFS_MAXPATHLEN;
	return 0;
}
