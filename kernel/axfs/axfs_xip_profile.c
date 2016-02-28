/* 2008-03-31: File added and changed by Sony Corporation */
/*
 * fs/axfs/axfs_xip_profile.c
 *
 * profiler: /proc/axfs_xip
 *
 * Copyright 2005-2007 Sony Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 *
 */

#ifdef CONFIG_SNSC_DEBUG_AXFS

#include <linux/types.h>
#include <linux/proc_fs.h>


#ifndef CONFIG_SNSC_DEBUG_AXFS_XIP_RECORDS_NUM
#define CONFIG_SNSC_DEBUG_AXFS_XIP_RECORDS_NUM 500
#endif

#define  RECORDS CONFIG_SNSC_DEBUG_AXFS_XIP_RECORDS_NUM

#define  MAX_FILE_NAME_LEN	48

static DECLARE_MUTEX(record_index_sem);

/* each record is 64 bytes */
typedef struct {
	char filename[MAX_FILE_NAME_LEN];	/* XIP mapped file name */
	unsigned long physaddr; /* XIP mapped physaddr */
	unsigned long virtaddr; /* XIP mapped virtaddr */
	unsigned int size;	/* XIP mapped size */
	unsigned long pgprot; 	/* XIP mapped page prot */
} axfs_xip_record_t;

static axfs_xip_record_t axfs_xip_records[RECORDS];
static unsigned long record_index=0;

/* record function */
int axfs_xip_record(unsigned char *name, unsigned long physaddr,
		    unsigned long virtaddr, unsigned int size,
		    unsigned long pgprot)
{

	int namelen=0;
	if(down_interruptible(&record_index_sem))
		return -EINTR;

	if(record_index >= RECORDS)
		goto out;

	axfs_xip_records[record_index].physaddr = physaddr;
	axfs_xip_records[record_index].virtaddr = virtaddr;
	axfs_xip_records[record_index].size = size;
	axfs_xip_records[record_index].pgprot = pgprot;
	memset(axfs_xip_records[record_index].filename, 0, MAX_FILE_NAME_LEN);
	namelen = strlen(name);
	strncpy(axfs_xip_records[record_index].filename, name,
	       	(namelen >= MAX_FILE_NAME_LEN)? MAX_FILE_NAME_LEN-1:namelen);

	record_index++;

out:
	up(&record_index_sem);
	return 0;
}

static int axfs_xip_record_to_string(axfs_xip_record_t *record, char *buf, int len)
{

	return snprintf(buf,len,
			"0x%08lx to 0x%08lx 0x%x 0x%lx %s\n",
			record->physaddr, record->virtaddr,
			record->size, record->pgprot,record->filename) ;
}


static unsigned int is_first_line = 1;
#define PROFILE_HEADINGS "\nXIP: physaddr, virtaddr, size, pgprot, filename\n"
#define HEADINGS_LEN sizeof(PROFILE_HEADINGS)

static int axfs_xip_proc_read(char *page, char **start, off_t off, int count,
				  int *eof, void *data)
{
	unsigned long tlen=0;
	unsigned long index = record_index;
	axfs_xip_record_t *record;

	if(down_interruptible(&record_index_sem))
		return -EINTR;

	if(off >= index){
		*eof = 1;
		is_first_line = 1;
		goto out;
	}
	record = &axfs_xip_records[off];

	if ( is_first_line ){
		strncpy(page+tlen, PROFILE_HEADINGS, HEADINGS_LEN );
		tlen += HEADINGS_LEN - 1;
		is_first_line = 0;
	}
	tlen += axfs_xip_record_to_string(record, page+tlen, PAGE_SIZE-tlen);
	*start = (char *)1;
 out:
	up(&record_index_sem);
	return tlen>count?0:tlen;
}

/* Write to Clear */
static int axfs_xip_proc_write(struct file *file, const char *buffer,
				 unsigned long count, void *data)
{
	if(down_interruptible(&record_index_sem))
		return -EINTR;

	record_index = 0;

	up(&record_index_sem);
	return count;
}

static int __init axfs_xip_proc_profile(void)
{
	struct proc_dir_entry *ent;
	ent = create_proc_entry("axfs_xip", S_IFREG|S_IRUGO|S_IWUSR, NULL);
	if (!ent) {
		printk(KERN_ERR "create axfs_xip proc entry failed\n");
		return -ENOMEM;
	}
	ent->read_proc = axfs_xip_proc_read;
	ent->write_proc = axfs_xip_proc_write;
	return 0;
}
late_initcall(axfs_xip_proc_profile);

#endif /* CONFIG_SNSC_DEBUG_AXFS */
