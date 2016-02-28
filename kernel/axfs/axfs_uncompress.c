/* 2009-08-03: File added and changed by Sony Corporation */
/*
 * Advanced XIP File System for Linux - AXFS
 *   Readonly, compressed, and XIP filesystem for Linux systems big and small
 *
 *   Modified in 2006 by Eric Anderson
 *     from the cramfs sources fs/cramfs/uncompress.c
 *
 * (C) Copyright 1999 Linus Torvalds
 *
 * axfs_uncompress.c -
 *  axfs interfaces to the uncompression library. There's really just
 * three entrypoints:
 *
 *  - axfs_uncompress_init() - called to initialize the thing.
 *  - axfs_uncompress_exit() - tell me when you're done
 *  - axfs_uncompress_block() - uncompress a block.
 *
 * NOTE NOTE NOTE! The uncompression is entirely single-threaded. We
 * only have one stream, and we'll initialize it only once even if it
 * then is used by multiple filesystems.
 *
 * This is reduntant code basically a duplicate of fs/cramfs/uncompress.c
 * I plan to merge the two and make a ready to use decompressor API in lib
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>
#include <linux/mutex.h>

static z_stream stream;
static int initialized;
static DEFINE_MUTEX(axfs_uncmp_mutex);

/******************************************************************************
 *
 * axfs_uncompress_block
 *
 * Description: Actually handles the decompression of data
 *
 *
 * Parameters:
 *    (OUT) dst - pointer to the uncompressed data
 *
 *    (IN) dstlen - length of the original decompressed data
 *
 *    (IN) src - pointer to the compressed data
 *
 *    (IN) srclen - length of data in it's compressed state
 *
 * Returns:
 *     length of uncompressed data
 *
 *****************************************************************************/
int axfs_uncompress_block(void *dst, int dstlen, void *src, int srclen)
{
	int err;
	int out;

	mutex_lock(&axfs_uncmp_mutex);

	stream.next_in = src;
	stream.avail_in = srclen;

	stream.next_out = dst;
	stream.avail_out = dstlen;

	err = zlib_inflateReset(&stream);
	if (err != Z_OK) {
		printk("zlib_inflateReset error %d\n", err);
		zlib_inflateEnd(&stream);
		zlib_inflateInit(&stream);
	}

	err = zlib_inflate(&stream, Z_FINISH);
	if (err != Z_STREAM_END)
		goto err;

	out = stream.total_out;

	mutex_unlock(&axfs_uncmp_mutex);

	return out;

      err:

	mutex_unlock(&axfs_uncmp_mutex);

	printk("Error %d while decompressing!\n", err);
	printk("%p(%d)->%p(%d)\n", src, srclen, dst, dstlen);
	return 0;
}

/******************************************************************************
 *
 * axfs_uncompress_init
 *
 * Description: Initialize a zlib stream
 *
 *
 * Parameters:
 *    none
 *
 * Returns:
 *     0 or error number
 *
 *****************************************************************************/
int axfs_uncompress_init(void)
{
	if (!initialized++) {
		stream.workspace = vmalloc(zlib_inflate_workspacesize());
		if (!stream.workspace) {
			initialized = 0;
			return -ENOMEM;
		}
		stream.next_in = NULL;
		stream.avail_in = 0;
		zlib_inflateInit(&stream);
	}
	return 0;
}

/******************************************************************************
 *
 * axfs_uncompress_exit
 *
 * Description: Cleans up zlib stream once all users exit
 *
 *
 * Parameters:
 *    none
 *
 * Returns:
 *     0 or error number
 *
 *****************************************************************************/
int axfs_uncompress_exit(void)
{
	if (!--initialized) {
		zlib_inflateEnd(&stream);
		vfree(stream.workspace);
	}
	return 0;
}
