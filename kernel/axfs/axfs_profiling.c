/* 2007-11-20: File added and changed by Sony Corporation */
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
 * axfs_profiling.c -
 *   Tracks pages of files that enter the page cache.  Will not count XIP
 *   pages as they never enter the page cache.  Outputs through a proc file
 *   which generates a comma separated data file with path, page offset,
 *   count of times entered page cache.
 */

#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/proc_fs.h>
#include <linux/axfs_fs.h>

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define AXFS_PROC_DIR_NAME "axfs"

struct axfs_profiling_manager {
	struct axfs_profiling_data *profiling_data;
	struct axfs_super_incore *sbi;
	u32 *dir_structure;
	u32 size;
};

/* 128 is the max file name length and
  then x2 for extra room, x4 is for wide characters */
#define MAX_STRING_LEN 128*2*4

/* Handles for our Directory and File */
static struct proc_dir_entry *our_proc_dir = NULL;
static u32 proc_name_inc = 0;

int axfs_register_profiling_proc(struct axfs_profiling_manager *manager);
struct axfs_profiling_manager *axfs_unregister_profiling_proc(struct axfs_super_incore *sbi);
int init_profile_dir_structure(struct axfs_profiling_manager * manager, u32 num_inodes);

/******************************************************************************
 *
 * init_axfs_profiling
 *
 * Description:
 *   Creates the structures for tracking the page usage data and creates the
 *   proc file that will be used to get the data.
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer
 *
 * Returns:
 *    TRUE or FALSE
 *
 *****************************************************************************/
int init_axfs_profiling(struct axfs_super_incore *sbi)
{

	u32 num_nodes, num_inodes;
	struct axfs_profiling_manager *manager = NULL;
	struct axfs_profiling_data *profile_data = NULL;

	/* determine the max number of pages in the FS */
	num_nodes = sbi->blocks;

	manager = vmalloc(sizeof(*manager));
	if (manager == NULL) {
		return FALSE;
	}

	profile_data = vmalloc(num_nodes * sizeof(*profile_data));
	if (profile_data == NULL) {
		vfree(manager);
		return FALSE;
	}

	memset(profile_data, 0, num_nodes * sizeof(*profile_data));

	/* determine the max number of inodes in the FS */
	num_inodes = sbi->files;

	manager->dir_structure =
	    vmalloc(num_inodes * sizeof(u32 *));
	if (manager->dir_structure == NULL) {
		vfree(manager);
		vfree(profile_data);
		return FALSE;
	}

	memset(manager->dir_structure, 0,
	       (num_inodes * sizeof(u32 *)));

	manager->profiling_data = profile_data;
	manager->size = num_nodes * sizeof(*profile_data);
	manager->sbi = sbi;
	sbi->profiling_on = TRUE; /* Turn on profiling by default */
	sbi->profile_data_ptr = profile_data;

	init_profile_dir_structure(manager, num_inodes);

	axfs_register_profiling_proc(manager);

	return TRUE;
}

/******************************************************************************
 *
 * init_profile_dir_structure
 *
 * Description:
 *   Creates the structures for tracking the page usage data and creates the
 *   proc file that will be used to get the data.
 *
 * Parameters:
 *    (IN) manager - pointer to the profile manager for the filing system
 *
 *    (IN) num_inodes - number of files in the system
 *
 * Returns:
 *    0
 *
 *****************************************************************************/
int init_profile_dir_structure(struct axfs_profiling_manager * manager, u32 num_inodes)
{

   struct axfs_metadata_ptrs_incore *metadata = manager->sbi->metadata;
   u32 child_index=0, i , j;
   u32 * dir_structure = manager->dir_structure;

   /* loop through each inode in the image and find all
      of the directories and mark their children */
   for (i=0; i < num_inodes; i++)
   {
      /* determine if the entry is a directory */
      if (S_ISDIR(AXFS_GET_MODE(metadata, i)))
      {
         /* get the index number for this directory */
         child_index = AXFS_GET_INODE_ARRAY_INDEX(metadata,i);

         /* get the offset to its children */
         for(j=0; j < AXFS_GET_INODE_NUM_ENTRIES(metadata,i); j++)
         {
            /* debug code */
            if (dir_structure[child_index+j] != 0)
            {
               printk(KERN_ERR "axfs: ERROR inode was already set old %lu new %lu\n",
                      (unsigned long)dir_structure[child_index+j], (unsigned long)i);
            }

            dir_structure[child_index+j] = i;
         }
      }
   }

   return 0;
}

/******************************************************************************
 *
 * get_directory_path
 *
 * Description:
 *   Determines the directory path of every file for printing the spreadsheet.
 *
 * Parameters:
 *    (IN) manager - Pointer to axfs profile manager
 *
 *    (OUT) buffer - Pointer to the printable directory path for each file
 *
 *    (IN) inode_number - Inode number of file to look up
 *
 * Returns:
 *    Size of the path to the file
 *
 *
 **************************************************************************/
int get_directory_path(struct axfs_profiling_manager *manager, char *buffer, u32 inode_number)
{
   u32 path_depth=0, path_size=0, string_len=0, index = inode_number;
   u8 **path_array = NULL;
   int i;

   /* determine how deep the directory path is and how big the name string will be
      walk back until the root directory index is found (index 0 is root)*/
   while (manager->dir_structure[index] != 0)
   {
      path_depth++;
      /* set the index to the index of the parent directory*/
      index = manager->dir_structure[index];
   }

   if (path_depth != 0) {
      /* create an array that will hold a pointer for each of the directories names */
      path_array = vmalloc(path_depth * sizeof(*path_array));
      if (path_array == NULL) {
         printk(KERN_DEBUG "axfs: directory_path vmalloc failed.\n");
         goto out;
      }
   }

   index = manager->dir_structure[inode_number];
   for (i=path_depth; i>0; i--)
   {
      /* store a pointer to the name in the array */
      path_array[(i-1)]= (u8 *)AXFS_GET_INODE_NAME_ADDRESS(manager->sbi, index);

      index = manager->dir_structure[index];
   }

   /* now print out the directory structure from the begining */
   string_len = sprintf(buffer, "./");
   path_size +=string_len;
   for (i=0; i<path_depth; i++)
   {
      buffer = buffer + string_len;
      string_len = sprintf(buffer, "%s/",(char *)path_array[i]);
      path_size +=string_len;
   }

   if (path_array != NULL)
   {
      vfree(path_array);
   }
out:
   return(path_size);

}

/******************************************************************************
 *
 * shutdown_axfs_profiling
 *
 * Description:
 *   Remove the proc file for this volume and release the memory in the
 *   profiling manager
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer
 *
 * Returns:
 *    TRUE or FALSE
 *
 *****************************************************************************/
int shutdown_axfs_profiling(struct axfs_super_incore *sbi)
{
	struct axfs_profiling_manager *manager;
	u32 num_nodes = sbi->blocks;

	if(num_nodes == 0)
		return FALSE;
	/* remove the proc file for this volume and release the memory in the
	   profiling manager */
	manager = axfs_unregister_profiling_proc(sbi);
	if (manager == NULL)
		return FALSE;

	vfree(manager->profiling_data);
	vfree(manager->dir_structure);
	vfree(manager);
	return TRUE;
}

/******************************************************************************
 *
 * axfs_profiling_add
 *
 * Description:
 *    Log when a node is paged into memory by incrementing the count in the
 *    array profile data structure.
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer
 *
 *    (IN) array_index - The offset into the nodes table of file (node number)
 *
 *    (IN) axfs_inode_number - Inode of the node to determine file name later
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
void axfs_profiling_add(struct axfs_super_incore *sbi, unsigned long array_index, unsigned int axfs_inode_number)
{
	unsigned long size;
	struct axfs_profiling_data *profile_data; /* This represents the node number */

	if(sbi->profiling_on == TRUE)
	{
		size = sizeof(*profile_data);
		profile_data =
		    (struct axfs_profiling_data *)((unsigned long)sbi->profile_data_ptr +
					   (unsigned long)(array_index * size));

		/* Record the inode number to determine the file name later. */
		profile_data->inode_number = axfs_inode_number;

		/* Increment the number of times the node has been paged in */
		profile_data->count++;
	}
}

/******************************************************************************
 *
 * procfile_read
 *
 * Description:
 *   When the entry under the proc filing system is read a comma seperated
 *   data file will be returned with path and file name, page offset in the
 *   file and the number of times the page was referenced.  This funtion will
 *   be called repeatedly filling the buffer until an EOF is returned.
 *
 * Parameters:
 *    (IN) buffer - Buffer containing data going to user
 *
 *    (OUT) buffer_location - pointer to the current location in buffer
 *
 *    (IN) offset - into the proc file being read
 *
 *    (IN) buffer_length - size of the current buffer
 *
 *    (IN) eof - signals when the end of file is reached
 *
 *    (IN) data - Profiling manager for the axfs volume read from
 *
 * Returns:
 *    The profiling manager pointer for the proc file.
 *
 *****************************************************************************/
ssize_t procfile_read(char *buffer,
		      char **buffer_location,
		      off_t offset, int buffer_length, int *eof, void *data)
{
	int len = 0;
	struct axfs_profiling_manager *man_ptr =
	    (struct axfs_profiling_manager *)data;
	struct axfs_profiling_data *profile_data;
	u32 array_index;
	u32 loop_size, i, inode_page_offset, node_offset, print_len = 0;
	char *current_buf_ptr, *name =NULL;

	loop_size = man_ptr->size / sizeof(*profile_data);

	/* If all data has been returned set EOF */
	if (offset >= loop_size) {
		*eof = 1;
		return 0;
	}

	current_buf_ptr = buffer;
	/* print as much as the buffer can take */
	for (i = offset; i < loop_size; i++) {

		if ((print_len + MAX_STRING_LEN) > buffer_length)
			break;
		/* get the first profile data structure */
		profile_data = &(man_ptr->profiling_data[i]);

		if (profile_data->count != 0) {
			/* the loop count is the page number */

			/* file names can be duplicated so we must print out the path */
			len = get_directory_path(man_ptr, current_buf_ptr, profile_data->inode_number);

			print_len += len;
			current_buf_ptr += len;

			/* get a pointer to the inode name */
			array_index = AXFS_GET_INODE_ARRAY_INDEX(man_ptr->sbi->metadata, profile_data->inode_number);
			name =	(char *)AXFS_GET_INODE_NAME_ADDRESS(man_ptr->sbi, profile_data->inode_number);

			/* need to convert the page number in the node area to the page number within the file */
			node_offset = i ;
			/* gives the offset of the node in the node list area then substract that from the */
			inode_page_offset = node_offset - array_index;

			/* set everything up to print out */
			len =
			    sprintf(current_buf_ptr,
				    "%s,%lu,%lu \n",
				    name, (unsigned long)(inode_page_offset * PAGE_SIZE),
				    (profile_data->count));

			print_len += len;
			current_buf_ptr += len;
		}
	}

	/* return the number of items printed.
	   This will be added to offset and passed back to us */
	*buffer_location = (char *)(i - offset);

	return print_len;
}

/******************************************************************************
 *
 * procfile_write
 *
 * Description:
 *   This is used to clear the profiling data or turn profiling on and off.
 *   Profiling is on by default at system start up.
 *
 * Parameters:
 *    (IN) file - unused
 *
 *    (IN) buffer - data write to proc by user
 *
 *    (IN) count - Number of bytes in buffer
 *
 *    (IN) data - Profiling manager for the axfs volume written to
 *
 * Returns:
 *    The number of bytes to be written regardless of the results.
 *
 *****************************************************************************/
ssize_t procfile_write(struct file * file,
		       const char *buffer, unsigned long count, void *data)
{
	struct axfs_profiling_manager *man_ptr =
	    (struct axfs_profiling_manager *)data;

	if ((count >= 2) && (0 == memcmp(buffer, "on", 2)))
	{
		man_ptr->sbi->profiling_on = TRUE;
	}
	else if ((count >= 3) && (0 == memcmp(buffer, "off", 3)))
	{
		man_ptr->sbi->profiling_on = FALSE;
	}
	else if ((count >= 5) && (0 == memcmp(buffer, "clear", 5)))
	{
		memset(man_ptr->profiling_data, 0, man_ptr->size);
	}
	else
	{
		printk(KERN_INFO "axfs: Unknown command.  Supported options are:\n");
		printk(KERN_INFO "\t\"on\"\tTurn on profiling\n");
		printk(KERN_INFO "\t\"off\"\tTurn off profiling\n");
		printk(KERN_INFO "\t\"clear\"\tClear profiling buffer\n");
	}

	return count;
}

/******************************************************************************
 *
 * create_proc_directory
 *
 * Description:
 *   Creates the proc file direcotry for all of the proc files.
 *
 * Parameters:
 *    none
 *
 * Returns:
 *    TRUE or FALSE
 *
 *****************************************************************************/
int create_proc_directory(void)
{
	if (our_proc_dir == NULL) {
		our_proc_dir = proc_mkdir(AXFS_PROC_DIR_NAME, NULL);
		if (!our_proc_dir) {
			printk(KERN_WARNING "axfs: Failed to create directory\n");
			return FALSE;
		}
	}
	return TRUE;
}

/******************************************************************************
 *
 * delete_proc_directory
 *
 * Description:
 *   Removes the proc directory once all of the proc files have been removed.
 *
 * Parameters:
 *    none
 *
 * Returns:
 *    none
 *
 *****************************************************************************/
void delete_proc_directory(void)
{
	/* Determine if there are any directory elements
	   and remove if all of the proc files are removed. */
	if (our_proc_dir != NULL) {
		if (our_proc_dir->subdir == NULL) {
			remove_proc_entry(AXFS_PROC_DIR_NAME, NULL);
			our_proc_dir = NULL;
		}
	}
}

/******************************************************************************
 *
 * delete_proc_file
 *
 * Description:
 *   Will search through the proc directory for the correct proc file.
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer to determine which proc file to remove
 *
 * Returns:
 *    The profiling manager pointer for the proc file.
 *
 *****************************************************************************/
struct axfs_profiling_manager *delete_proc_file(struct axfs_super_incore *sbi)
{
	struct proc_dir_entry *current_proc_file;
	struct axfs_profiling_manager *manager;
	void *rv = NULL;
	/* Walk through the proc file entries to find the matching sbi */
	current_proc_file = our_proc_dir->subdir;

	while (current_proc_file != NULL) {
		manager = current_proc_file->data;
		if (manager == NULL) {
			printk(KERN_WARNING
			       "axfs: Error removing proc file private data was NULL.\n");
			rv = NULL;
			break;
		}
		if (manager->sbi == sbi) {
			/* we found the match */
			remove_proc_entry(current_proc_file->name,
					  our_proc_dir);
			rv = (void *)manager;
			break;
		}
		current_proc_file = our_proc_dir->next;
	}
	return (struct axfs_profiling_manager *)rv;
}

/******************************************************************************
 *
 * axfs_register_profiling_proc
 *
 * Description:
 *   Will register the instance of the proc file for a given volume.
 *
 * Parameters:
 *    (IN) manager - Pointer to the profiling manager for the axfs volume
 *
 * Returns:
 *    0 or error number
 *
 *****************************************************************************/
int axfs_register_profiling_proc(struct axfs_profiling_manager *manager)
{
	int rv = 0;
	struct proc_dir_entry *proc_file;
	char file_name[20];

	if (!create_proc_directory()) {
		rv = -ENOMEM;
		goto out;
	}

	snprintf(file_name, sizeof(file_name), "volume%d", proc_name_inc);
	proc_file = create_proc_entry(file_name, (mode_t)0644, our_proc_dir);
	if (proc_file == NULL) {
		remove_proc_entry(file_name, our_proc_dir);
		delete_proc_directory();
		rv = -ENOMEM;
		goto out;
	}

	proc_name_inc++;
	proc_file->read_proc = procfile_read;
	proc_file->write_proc = procfile_write;
	proc_file->owner = THIS_MODULE;
	proc_file->mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
	proc_file->uid = 0;
	proc_file->gid = 0;
	proc_file->data = manager;

	printk(KERN_DEBUG "axfs: Proc entry created\n");

out:
	return rv;
}

/******************************************************************************
 *
 * axfs_unregister_profiling_proc
 *
 * Description:
 *   Will unregister the instance of the proc file for the volume that was
 *   mounted.  If this is the last volume mounted then the proc directory
 *   will also be removed.
 *
 * Parameters:
 *    (IN) sbi- axfs superblock pointer to determine which proc file to remove
 *
 * Returns:
 *    The profiling manager pointer for the proc file.
 *
 *****************************************************************************/
struct axfs_profiling_manager *axfs_unregister_profiling_proc(struct
							      axfs_super_incore *sbi)
{
	struct axfs_profiling_manager *manager;
	manager = delete_proc_file(sbi);
	delete_proc_directory();
	return manager;
}


