/* 2007-11-13: File added and changed by Sony Corporation */
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
 * axfs_inode.c -
 *   Contains the core filesystem routines with the major exception of the
 *   mounting infrastructure.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/blkdev.h>
#include <linux/axfs_fs.h>
#include <linux/slab.h>
#include <linux/vfs.h>
#include <linux/semaphore.h>
#include <asm/uaccess.h>
#include <linux/vmalloc.h>
#include <asm/tlbflush.h>
#include <linux/buffer_head.h>
#include <linux/mm.h>

/******************** Function Declarations ****************************/
static int axfs_mmap(struct file *file, struct vm_area_struct *vma);

static struct dentry *axfs_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd);

static int axfs_readdir(struct file *filp, void *dirent, filldir_t filldir);

int axfs_fault(struct vm_area_struct *area, struct vm_fault *vmf);

ssize_t axfs_file_read(struct file *filp, char __user * buf, size_t len,
		       loff_t * ppos);

static int axfs_readpage(struct file *file, struct page *page);

int axfs_get_xip_mem(struct address_space *mapping, pgoff_t pgoff, int create,
		     void **kmem, unsigned long *pfn);

/***************** functions in filemap_xip.c ***************************/
extern ssize_t xip_file_read(struct file *filp, char __user * buf, size_t len,
			     loff_t * ppos);

extern int xip_file_fault(struct vm_area_struct *area, struct vm_fault *vmf);

/***************** functions in other axfs files ***************************/
extern int axfs_get_sb(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data, struct vfsmount *mnt);

#ifdef CONFIG_AXFS_PROFILING
extern void axfs_profiling_add(struct axfs_super_incore *sbi, unsigned long array_index, unsigned int axfs_inode_number);
#endif

/******************** Structure Declarations ****************************/
static struct file_system_type axfs_fs_type = {
	.owner = THIS_MODULE,
	.name = "axfs",
	.get_sb = axfs_get_sb,
	.kill_sb = kill_anon_super,
};

static struct file_operations axfs_directory_operations = {
	.llseek = generic_file_llseek,
	.read = generic_read_dir,
	.readdir = axfs_readdir,
};

static struct inode_operations axfs_dir_inode_operations = {
	.lookup = axfs_lookup,
};

static struct file_operations axfs_fops = {
	.read = axfs_file_read,
	.aio_read = generic_file_aio_read,
	.mmap = axfs_mmap,
};

static struct address_space_operations axfs_aops = {
	.readpage = axfs_readpage,
	.get_xip_mem = axfs_get_xip_mem,
};

static struct vm_operations_struct axfs_vm_ops = {
	.fault = axfs_fault,
};

static struct backing_dev_info axfs_backing_dev_info = {
	.ra_pages = 0,		/* No readahead */
};

/******************************************************************************
 *
 * axfs_copy_block_data
 *
 * Description: Helper function to read data from block device
 *
 *
 * Parameters:
 *    (IN) sb - pointer to super block structure.
 *
 *    (IN) dst_addr - pointer to buffer into which data is to be read.
 *
 *    (IN) boffset - offset within block device
 *
 *    (IN) len - length of data to be read
 *
 * Returns:
 *     none
 *
 *****************************************************************************/
void axfs_copy_block_data(struct super_block *sb, void * dst_addr, u64 boffset, u64 len) {
	unsigned long dst;
	unsigned long src;
	sector_t block;
	size_t bytes;
	struct buffer_head *bh;
	u64 copied = 0;

	if(len == 0)
		return;

	while(copied < len) {
		block = (sector_t)((unsigned long)(boffset + copied) / (sb->s_blocksize));
		bh = sb_bread(sb, block);
		src = (unsigned long)bh->b_data;
		dst = (unsigned long)dst_addr;
		if (copied == 0) {
			bytes = sb->s_blocksize - (unsigned long)boffset % sb->s_blocksize;
			if (bytes > len)
				bytes = (size_t)len;
			src += (unsigned long)boffset % sb->s_blocksize;
        } else {
 			dst += copied;
			if( (len - copied) < sb->s_blocksize ) {
				bytes = (size_t)(len - copied);
			} else {
				bytes = sb->s_blocksize;
			}
		}
		memcpy((void *)dst, (void *)src, bytes);
		copied += bytes;
		brelse(bh);
	}
}

/******************************************************************************
 *
 * axfs_copy_data
 *
 * Description: function to copy data from a block device
 *
 *
 * Parameters:
 *    (IN) sb - pointer to the super_block structure
 *
 *    (IN) dst  - pointer to destination buffer into which data is read.
 *
 *    (IN) region - pointer to the region descriptor from which data is to be read
 *
 *    (IN) offset - offset within the region
 *
 *    (IN) len - length of data to be read
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
void axfs_copy_data(struct super_block *sb, void * dst, struct axfs_region_desc_incore *region, u64 offset, u64 len) {
	u64 boffset;
	u64 mmapped = 0;
	u64 end;
	u64 begin;
	void * src;
	void * d;
	struct axfs_super_incore *sbi = AXFS_SB(sb);

	if(len == 0)
		return;

	if (region->virt_addr != 0) {
		end = region->fsoffset + offset + len;
		begin = region->fsoffset + offset;
		if (sbi->mmap_size > end) {
			mmapped = len;
		} else if (sbi->mmap_size > begin) {
			mmapped = sbi->mmap_size - begin;
		}

		if(mmapped != 0) {
			src = (void *)((unsigned long)region->virt_addr + (unsigned long)offset);
			memcpy(dst, src, mmapped);
		}
	}
	d = (void *)((unsigned long)dst + (unsigned long)mmapped);
	boffset = AXFS_FSOFFSET_2_BLOCKOFFSET(sbi,region->fsoffset) + offset + mmapped;
	axfs_copy_block_data(sb, d, boffset, len - mmapped);
}

/******************************************************************************
 *
 * axfs_iget5_test
 *
 * Description: Helper function for VFS to handle inodes.
 *
 *
 * Parameters:
 *    (IN) inode - pointer to the VFS inode structure for the file/dir that is being
 *                 accessed.
 *
 *    (IN) opaque - pointer to the axfs inode number.
 *
 * Returns:
 *    1 on match 0 on not match
 *
 *****************************************************************************/
static int axfs_iget5_test(struct inode *inode, void *opaque)
{
	u64 *inode_number = (u64 *) opaque;

	if (inode->i_sb == NULL) {
		printk(KERN_ERR
		       "axfs_iget5_test: the super block is set to null \n");
	}
	if (inode->i_ino == *inode_number)
		return 1;   /* matches */
	else
		return 0;   /* does not match */
}

/******************************************************************************
 *
 * axfs_iget5_set
 *
 * Description: Helper function for VFS to handle inodes.
 *
 *
 * Parameters:
 *    (IN) inode - pointer to the VFS inode structure for the file/dir that is being
 *                 accessed.
 *
 *    (IN) opaque - pointer to the axfs inode number.
 *
 * Returns:
 *    always 0
 *
 *****************************************************************************/
static int axfs_iget5_set(struct inode *inode, void *opaque)
{
	u64 *inode_number = (u64 *)opaque;

	if (inode->i_sb == NULL) {
		printk(KERN_ERR
		       "axfs_iget5_set: the super block is set to null \n");
	}
	inode->i_ino = *inode_number;
	return 0;
}
/******************************************************************************
 *
 * axfs_create_vfs_inode
 *
 * Description:  Takes an axfs inode structure and returns a newly created VFS inode
 *    structure that is properly populated. new)inode is called to allocate the memory
 *    for the structure
 *
 * Parameters:
 *    (IN) sb - pointer to the fs super block.  Used in allocation the memory
 *              of the new inode structure.  General Info: the generic allocate_inode
 *             is used unless a special allocate_inode is specified in the sb structure
 *             in the super operations. The sb is stored in the inode structure for the
 *             same reason on other inode operations
 *
 *    (IN) inode_number -  inode number of the axfs inode that will be used to populate the fs
 *             Inode structure.
 *
 * Returns:
 *    a populated inode structure.
 *
 *****************************************************************************/
struct inode *axfs_create_vfs_inode(struct super_block *sb,
				    int inode_number)
{
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	struct inode *inode = iget5_locked(sb,
					   inode_number,
					   axfs_iget5_test, axfs_iget5_set,
					   &inode_number);
	static struct timespec zerotime;
	u64 size;

	if (inode && (inode->i_state & I_NEW)) {
		inode->i_mode = AXFS_GET_MODE(sbi->metadata,inode_number);
		inode->i_uid = AXFS_GET_UID(sbi->metadata,inode_number);
		size = AXFS_GET_INODE_FILE_SIZE(sbi->metadata,inode_number);
		inode->i_size = size;
		inode->i_blocks = AXFS_GET_INODE_NUM_ENTRIES(sbi->metadata,inode_number);
		inode->i_blkbits = PAGE_CACHE_SHIFT;
		inode->i_gid = AXFS_GET_GID(sbi->metadata,inode_number);
		inode->i_mapping->backing_dev_info = &axfs_backing_dev_info;

		/* Struct copy intentional */
		inode->i_mtime = inode->i_atime = inode->i_ctime = zerotime;
		inode->i_ino = inode_number;
		/* inode->i_nlink is left 1 - arguably wrong for directories,
		   but it's the best we can do without reading the directory
		   contents.  1 yields the right result in GNU find, even
		   without -noleaf option. */


		if (S_ISREG(inode->i_mode)) {
			inode->i_fop = &axfs_fops;
			inode->i_data.a_ops = &axfs_aops;
			inode->i_mapping->a_ops = &axfs_aops;
		} else if (S_ISDIR(inode->i_mode)) {
			inode->i_op = &axfs_dir_inode_operations;
			inode->i_fop = &axfs_directory_operations;
		} else if (S_ISLNK(inode->i_mode)) {
			inode->i_op = &page_symlink_inode_operations;
			inode->i_data.a_ops = &axfs_aops;
		} else {
			inode->i_size = 0;
			inode->i_blocks = 0;
			init_special_inode(inode, inode->i_mode,
					   old_decode_dev(size));
		}
		unlock_new_inode(inode);
	}

	return inode;
}

/******************************************************************************
 *
 * axfs_mmap
 *
 * Description: Called when a file (or a portion of a file) is to be memory
 *              mapped. In AXFS, since a region can contain both pages that are
 *              to be memory mapped and those that are to be uncompressed and
 *              copied to RAM, the function figures out which pages are XIP and
 *              sets up page tables to directly point to flash regions for those
 *              pages. For the others, the normal flow of letting the page fault
 *              handler setting up the page tables is followed.
 *
 *
 * Parameters:
 *    (IN) file - pointer to file structure for the file that is being memory
 *                 mapped.
 *
 *    (IN) vma - virtual memory region structure. This contains information
 *               about the file offset and the length that is being memory
 *               mapped, in addition to permissions, etc.
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long array_index, length, offset, count;
	unsigned int numpages;
	struct inode *inode = file->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	struct axfs_metadata_ptrs_incore *md = sbi->metadata;
	u64 axfs_inode_number = inode->i_ino;
	int err, error = 0;
	unsigned long xip_node_address;

	if ((vma->vm_flags & VM_WRITE) || (!AXFS_PHYSADDR_IS_VALID(sbi)))
		return generic_file_mmap(file, vma);

	if ((vma->vm_flags & VM_SHARED) && (vma->vm_flags & VM_MAYWRITE)) {
		err = -EINVAL;
		goto out;
	}

	vma->vm_ops = &axfs_vm_ops;

	if (!file->f_mapping->a_ops->readpage) {
		err = -ENOEXEC;
		goto out;
	}

	offset = vma->vm_pgoff;

	array_index = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, axfs_inode_number);
	array_index += offset;
	length = vma->vm_end - vma->vm_start;

	if (length > inode->i_size)
		length = inode->i_size;

	length = PAGE_ALIGN(length);
	numpages = length >> PAGE_SHIFT;

	/* loop through the range to figure out which pages are XIP and map only those */
	for (count = 0; count < numpages; count++, array_index++) {
		if (AXFS_GET_NODE_TYPE(md, array_index) == XIP) {
#ifdef VM_XIP
			/* set the vma flags to indicate VM_XIP for copy on write
			Not sure what this will do for a file that has mixed pages.
			Should be fun */
#ifdef VM_PFNMAP
	        	vma->vm_flags |= (VM_IO | VM_XIP | VM_PFNMAP);
#else
			vma->vm_flags |= (VM_IO | VM_XIP);
#endif
#else
#ifdef VM_PFNMAP
	        	vma->vm_flags |= (VM_IO | VM_PFNMAP);
#else
			vma->vm_flags |= (VM_IO);
#endif
#endif
			xip_node_address = AXFS_GET_XIP_REGION_PHYSADDR(sbi);
			xip_node_address += ((AXFS_GET_NODE_INDEX(md, array_index)) << PAGE_SHIFT);

			error = vm_insert_pfn(vma, vma->vm_start + (PAGE_SIZE * count),(unsigned long)xip_node_address >> PAGE_SHIFT);

			if (error)
			{
				printk(KERN_ERR "axfs: axfs_mmap: vm_insert_pfn() error=%d\n"
					" vma->vm_flags=0x%X, vma->vm_page_prot=0x%lX, address=0x%lX, "
					"size=0x%X\n",
					error,
					(unsigned int)vma->vm_flags,
				        pgprot_val(vma->vm_page_prot),
					(unsigned long)xip_node_address,
					(unsigned int)(PAGE_SIZE));

				if (error) {
				err = -EAGAIN;
				goto out;
				}
			}

#ifdef CONFIG_SNSC_DEBUG_AXFS
			axfs_xip_record((unsigned char *)file->f_dentry->d_name.name,
					xip_node_address,
					vma->vm_start + (PAGE_SIZE * count),
					(unsigned int)(PAGE_SIZE),
					pgprot_val(vma->vm_page_prot));
#endif
		}
	}

	return 0;

out:
	return err;
}

/******************************************************************************
 *
 * axfs_lookup
 *
 * Description:  Lookup and fill in the inode data..
 *              Searches the children of the parent dentry for the name in question
 *
 *
 * Parameters:
 *    (IN) dir - pointer to the vfs inode structure for the directory to be
 *					searched
 *
 *    (IN/OUT) dentry - dentry used to find directory and to add new entry to
 *
 *    (IN) nd - Pointer to the search name
 *
 * Returns:
 *    always returns NULL
 *
 * Assumptions:
 *          The name contains accepted chactacters, no wild characters,
 *          and alpha sorted directories
 *
 *****************************************************************************/
static struct dentry *axfs_lookup(struct inode *dir, struct dentry *dentry,
				  struct nameidata *nd)
{
	struct super_block *sb = dir->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 dir_inode_number = dir->i_ino;
	u64 dir_index = 0;
	u64 dir_entry_inode_number;
	char *name;
	int namelen, err;

	while (dir_index < AXFS_GET_INODE_NUM_ENTRIES(sbi->metadata,dir_inode_number)) {
		/* get the index of the axfs inode for the given directory index */
		dir_entry_inode_number = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, dir_inode_number);

		dir_entry_inode_number += dir_index;

		/* get a pointer to the inode name */
		name = (char *)AXFS_GET_INODE_NAME_ADDRESS(sbi, dir_entry_inode_number);

		namelen = strlen(name);

		/* fast test, the entries are sorted alphabetically and the first letter is smaller than the first
		 * letter in the search name then it isn't in this directory.  Keeps this loop from needing to scan through always.
		 */
		if (dentry->d_name.name[0] < name[0])
			break;

		/* increment the directory index before all of the tests */
		dir_index++;

		/* Quick check that the name is roughly the right length */
		if (dentry->d_name.len != namelen)
			continue;

		/* do an exact compare of the strings */
		err = memcmp(dentry->d_name.name, name, namelen);
		if (err > 0)
			continue;

		/* the correct inode has been found only id retval = 0 */
		if (err == 0) {
			/* create a VFS inode from the axfs inode and then add that to the dentry. */
			d_add(dentry,
			      axfs_create_vfs_inode(dir->i_sb,
						    dir_entry_inode_number));
			goto out;
		}

		/*  retval < 0  for alpha sorted dirs this shouldn't happen */
		break;

	}
	d_add(dentry, NULL);

out:
	return NULL;
}

/******************************************************************************
 *
 * axfs_readdir
 *
 * Description:  Reads through each directory entry from the offset passed in
 *               the file structure.  It will then loop through each entry from
 *               that point and fills in the dirent structure.
 *
 *
 * Parameters:
 *    (IN) filp - the file structure pointer for the current directory being read
 *
 *    (IN) dirent - a pointer to a dirent structure that will be populated when
 *                 the filldir function is called
 *
 *    (IN) filldir - function pointer to the filldir function used for populating the
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 dir_inode_number = inode->i_ino;
	u64 dir_entry_inode_number;
	u64 dir_index;
	char *name;
	int namelen;
	int err = 0;

	/* Get the current index into the directory and verify it is not beyond the end of the list */
	dir_index = filp->f_pos;
	if (dir_index >= AXFS_GET_INODE_NUM_ENTRIES(sbi->metadata,dir_inode_number))
		goto out;

	/* Verify the inode is for a directory */
	if (!(S_ISDIR(inode->i_mode))) {
		err = -EINVAL;
		goto out;
	}

	/* loop through for the current directory index position to the end
	 * i_blocks holds the number of directory entries for this directory
	 */
	while (dir_index < AXFS_GET_INODE_NUM_ENTRIES(sbi->metadata,dir_inode_number)) {
      	/* get a pointer to the axfs inode structure for the given directory index */
		dir_entry_inode_number = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, dir_inode_number);

		dir_entry_inode_number += dir_index;

		/* get a pointer to the inode name */
		name = (char *)AXFS_GET_INODE_NAME_ADDRESS(sbi, dir_entry_inode_number);

		namelen = strlen(name);

		/* call filldir to populate the kernel specific dirent layout. */
		err = filldir(dirent, name, namelen, (loff_t) dir_index,
				dir_entry_inode_number,
				(int)(AXFS_GET_MODE(sbi->metadata, dir_entry_inode_number)));

		if (err)
			break;

		dir_index++;
		filp->f_pos = dir_index;
	}

out:
	return 0;
}


/******************************************************************************
 *
 * axfs_fault
 *
 * Description: This function is mapped into the VMA operations vector, and gets
 *              called on a page fault. Depending on whether the page is XIP or
 *              compressed, the fault routines, xip_file_fault and filemap_fault
 *              are called respectively.
 *
 * Parameters:
 *    (IN) area - The virtual memory area corresponding to this memory mapped region
 *
 *    (IN) vmf  - struct vm_fault containing details of the fault
 *
 * Returns:
 *    fault return status
 *
 *****************************************************************************/
int axfs_fault(struct vm_area_struct *area, struct vm_fault *vmf)
{
	struct file *file = area->vm_file;
	struct inode *inode = file->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 axfs_inode_number = inode->i_ino;
	u64 array_index;

	array_index = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, axfs_inode_number);
	array_index += vmf->pgoff;

#ifdef CONFIG_AXFS_PROFILING
   /* if that pages are marked for write they will be copies to RAM
      therefore we don't want their counts for being XIP'd */
   if (!(area->vm_flags & VM_WRITE))
   {
      axfs_profiling_add(sbi, array_index, axfs_inode_number);
   }
#endif

	/* figure out if the node is XIP or compressed and call the
	   appropriate function
	 */
	if (AXFS_GET_NODE_TYPE(sbi->metadata, array_index) == XIP) {
		return xip_file_fault(area, vmf);
	} else {
		return filemap_fault(area, vmf);
	}
}

/******************************************************************************
 *
 * axfs_file_read
 *
 * Description: axfs_file_read is mapped into the file_operations vector for
 *              all axfs files. It loops through the pages to be read and calls
 *              either generic_file_read (if the page is a compressed one) or
 *              xip_file_read (if the page is XIP).
 *
 * Parameters:
 *    (IN) file -  file to be read
 *
 *    (IN) len - length of file to be read
 *
 *    (IN) ppos - offset within the file to read from
 *
 *    (OUT) buf - user buffer that is filled with the data that we read.
 *
 * Returns:
 *    actual size of data read.
 *
 *****************************************************************************/
ssize_t axfs_file_read(struct file *filp, char __user * buf, size_t len,
		       loff_t * ppos)
{
	struct inode *inode = filp->f_dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 axfs_inode_number = inode->i_ino;
	size_t size_read = 0, total_size_read = 0;
	size_t readlength, actual_size;
	u64 size;
	u64 array_index;
	ssize_t total_file_size = AXFS_GET_INODE_FILE_SIZE(sbi->metadata, axfs_inode_number);

	/* loop through and figure out which page is XIP and which is compressed
	   Then call the appropriate read function. This might result in some additional
	   overhead for a read of large size, but if most reads are 4K on page
	   boundaries, it should be fine
	 */
	actual_size =
	    (len > (total_file_size - *ppos)) ? (total_file_size - *ppos) : len;

	readlength = actual_size < PAGE_SIZE ? actual_size : PAGE_SIZE;

	for (size = actual_size; size > 0; size -= size_read) {
		array_index = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, axfs_inode_number);
		array_index += *ppos >> PAGE_SHIFT;

		if (AXFS_GET_NODE_TYPE(sbi->metadata, array_index) == XIP) {
			size_read = xip_file_read(filp, buf, readlength, ppos);
		} else {
			size_read =
			    do_sync_read(filp, buf, readlength, ppos);
		}
		buf += size_read;
		total_size_read += size_read;
		if ((len - total_size_read < PAGE_SIZE)
		    && (total_size_read != len)) {
			readlength = len - total_size_read;
		}
	}

	return total_size_read;
}


/******************************************************************************
 *
 * axfs_readpage
 *
 * Description: This routine gets called to read in a compressed page from an axfs file. It
 *              gets called from the generic read routine.
 *
 *
 * Parameters:
 *    (IN) file - file to be read
 *
 *    (OUT) page -  page filled with data from the file.
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
static int axfs_readpage(struct file *file, struct page *page)
{
	struct inode *inode;
	void *pgdata = NULL;
	struct super_block *sb;
	struct axfs_super_incore *sbi;
	struct axfs_metadata_ptrs_incore *md;

	u64 axfs_inode_number, maxblock;
	u64 array_index, node_index, cnode_index;
	u64 offset;
	u32 max_len;
	u32 len = 0;
	u32 cnode_offset;
	u8 node_type;

	void *src;

	inode = page->mapping->host;
	sb = inode->i_sb;
	sbi = AXFS_SB(sb);
	md = sbi->metadata;

	axfs_inode_number = inode->i_ino;

	maxblock = (inode->i_size + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	pgdata = kmap(page);
	if (page->index < maxblock) {
		array_index = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, axfs_inode_number);
		array_index += page->index;

		node_index = AXFS_GET_NODE_INDEX(sbi->metadata, array_index);
		node_type = AXFS_GET_NODE_TYPE(sbi->metadata, array_index);

		if (node_type == Compressed) { /* node is in compessed region */
			cnode_offset = AXFS_GET_CNODE_OFFSET(md, node_index);
			cnode_index = AXFS_GET_CNODE_INDEX(md, node_index);
			down_write(&sbi->lock);
			if(cnode_index != sbi->current_cnode_index){ /* uncompress only necessary if different cblock */
				offset = AXFS_GET_CBLOCK_OFFSET(md, cnode_index);
				len = AXFS_GET_CBLOCK_OFFSET(md, cnode_index+1) - offset;
    			axfs_copy_data(sb, sbi->cblock_buffer[1], &(sbi->compressed), offset, len);
				axfs_uncompress_block(sbi->cblock_buffer[0], sbi->cblock_size, sbi->cblock_buffer[1], len);
				sbi->current_cnode_index = cnode_index;
			}
			downgrade_write(&sbi->lock);
			max_len = sbi->cblock_size - cnode_offset;
			len = (max_len > PAGE_CACHE_SIZE) ? PAGE_CACHE_SIZE : max_len;
			src = (void *)((unsigned long)sbi->cblock_buffer[0] + (unsigned long)cnode_offset);
			memcpy((void *)pgdata, src, len);
			up_read(&sbi->lock);
		}
		else if (node_type == Byte_Aligned){ /* node is in BA region*/
			offset = AXFS_GET_BANODE_OFFSET(md, node_index);
			max_len = sbi->byte_aligned.size - offset;
			len = (max_len > PAGE_CACHE_SIZE) ? PAGE_CACHE_SIZE : max_len;
			axfs_copy_data(sb, (void*)pgdata, &(sbi->byte_aligned), offset, len);
		}
		else{ /* node is XIP */
			offset = node_index << PAGE_SHIFT;
			len = PAGE_CACHE_SIZE;
			axfs_copy_data(sb, (void*)pgdata, &(sbi->xip), offset, len);
		}
	}

	memset(pgdata + len, 0, PAGE_CACHE_SIZE - len);
	kunmap(page);
	flush_dcache_page(page);
	SetPageUptodate(page);
	unlock_page(page);
	return 0;
}

/******************************************************************************
 *
 * axfs_get_xip_mem
 *
 * Description: This routine gets called to read an XIP page. The page is not
 *              actually copied into RAM. The page table is just set up to point
 *              to the actual flash address.
 *
 *
 * Parameters:
 *    (IN) mapping - address space that the page belongs to
 *
 *    (IN) pgoff   - offset within the file
 *
 *    (IN) create  -
 *
 *    (OUT) kmem   - XIP data virtual address
 *
 *    (OUT) pfn    - XIP data pfn
 *
 * Returns:
 *    0
 *
 *****************************************************************************/

int axfs_get_xip_mem(struct address_space *mapping, pgoff_t pgoff, int create,
				void **kmem, unsigned long *pfn)
{
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct axfs_super_incore *sbi = AXFS_SB(sb);
	u64 axfs_inode_number = inode->i_ino;
	u64 array_index;
	unsigned long data_address;

	/* pgoff is the file offset */
	array_index = AXFS_GET_INODE_ARRAY_INDEX(sbi->metadata, axfs_inode_number);
	array_index += pgoff;

	data_address = (unsigned long)
		       (((AXFS_GET_NODE_INDEX(sbi->metadata, array_index))<< PAGE_SHIFT)
			+ sbi->xip.virt_addr);

	*kmem = (void *)data_address;
	*pfn = virt_to_phys(*kmem) >> PAGE_SHIFT;

	return 0;
}

static int __init init_axfs_fs(void)
{
	axfs_uncompress_init();
	return register_filesystem(&axfs_fs_type);
}

static void __exit exit_axfs_fs(void)
{
	axfs_uncompress_exit();
	unregister_filesystem(&axfs_fs_type);
}

module_init(init_axfs_fs);
module_exit(exit_axfs_fs);
MODULE_LICENSE("GPL");
