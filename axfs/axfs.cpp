// axfs.cpp : Defines the entry point for the console application.
//
// LICENSE: GPL v2.  This project is a derivative work of the linux kernel.

#include "stdafx.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

inline uint16_t byteswap(uint16_t v)
{
	return _byteswap_ushort(v);
}

inline uint32_t byteswap(uint32_t v)
{
	return _byteswap_ulong(v);
}

inline uint64_t byteswap(uint64_t v)
{
	return _byteswap_uint64(v);
}

// wrapper for reading big-ending values
template<typename T>
struct BigEndianInt
{
	T value;

	operator T() const
	{
		return byteswap(value);
	}
};

typedef BigEndianInt<uint32_t> __be32;
typedef BigEndianInt<uint64_t> __be64;
typedef uint8_t u8;
typedef uint64_t u64;

#define	S_ISDIR(m)	((m & 0170000) == 0040000)	/* directory */
#define	S_ISCHR(m)	((m & 0170000) == 0020000)	/* char special */
#define	S_ISBLK(m)	((m & 0170000) == 0060000)	/* block special */
#define	S_ISREG(m)	((m & 0170000) == 0100000)	/* regular file */
#define	S_ISFIFO(m)	((m & 0170000) == 0010000)	/* fifo */
#ifndef _POSIX_SOURCE
#define	S_ISLNK(m)	((m & 0170000) == 0120000)	/* symbolic link */
#define	S_ISSOCK(m)	((m & 0170000) == 0140000)	/* socket */
#endif

#define PAGE_SHIFT 12
#define PAGE_CACHE_SHIFT 12
#define PAGE_CACHE_SIZE (1<<PAGE_CACHE_SHIFT)

/* on media format for the super block */
struct axfs_super_onmedia
{
	__be32 magic;		/* 0x48A0E4CD - random number */
	u8 signature[16];	/* "Advanced XIP FS" */
	u8 digest[40];		/* sha1 digest for checking data integrity */
	__be32 cblock_size;	/* maximum size of the block being compressed */
	__be64 files;		/* number of inodes/files in fs */
	__be64 size;		/* total image size */
	__be64 blocks;		/* number of nodes in fs */
	__be64 mmap_size;	/* size of the memory mapped part of image */
	__be64 strings;		/* offset to strings region descriptor */
	__be64 xip;		/* offset to xip region descriptor */
	__be64 byte_aligned;	/* offset to the byte aligned region desc */
	__be64 compressed;	/* offset to the compressed region desc */
	__be64 node_type;	/* offset to node type region desc */
	__be64 node_index;	/* offset to node index region desc */
	__be64 cnode_offset;	/* offset to cnode offset region desc */
	__be64 cnode_index;	/* offset to cnode index region desc */
	__be64 banode_offset;	/* offset to banode offset region desc */
	__be64 cblock_offset;	/* offset to cblock offset region desc */
	__be64 inode_file_size;	/* offset to inode file size desc */
	__be64 inode_name_offset;	/* offset to inode num_entries region desc */
	__be64 inode_num_entries;	/* offset to inode num_entries region desc */
	__be64 inode_mode_index;	/* offset to inode mode index region desc */
	__be64 inode_array_index;	/* offset to inode node index region desc */
	__be64 modes;		/* offset to mode mode region desc */
	__be64 uids;		/* offset to mode uid index region desc */
	__be64 gids;		/* offset to mode gid index region desc */
	u8 version_major;
	u8 version_minor;
	u8 version_sub;
	u8 compression_type;	/* Identifies type of compression used on FS */
	__be64 timestamp;	/* UNIX time_t of filesystem build time */
	u8 page_shift;
};

struct axfs_region_desc_onmedia
{
	__be64 fsoffset;
	__be64 size;
	__be64 compressed_size;
	__be64 max_index;
	u8 table_byte_depth;
	u8 incore;
};

struct axfs_region : public axfs_region_desc_onmedia
{
	void* data;

	axfs_region()
		: data(nullptr)
	{ }

	~axfs_region()
	{
		free(data);
	}

	uint64_t axfs_bytetable_stitch(uint64_t index) const
	{
		assert(index < max_index);

		// This is the old v1.9.1 AXFS version of axfs_bytetable_stitch
		const u8 *table = (const u8*)data;
		uint64_t output = 0;
		const uint64_t split = size / table_byte_depth;

		for (int i = 0; i < table_byte_depth; i++)
		{
			auto j = index + i * split;
			auto bits = 8 * i;
			uint64_t byte = table[j];
			output += byte << bits;
		}
		return output;
	}

};



void loadRegionImpl(const char* name, axfs_region& region, FILE* file, uint64_t offset)
{
	fseek(file, (long) offset, SEEK_SET);
	fread(&region, sizeof(axfs_region_desc_onmedia), 1, file);

	printf("loadRegion %s: %lld bytes at %lld %dx%d\n", name, (uint64_t)region.size, (uint64_t)region.fsoffset, (uint32_t) region.max_index, (uint32_t) region.table_byte_depth);
	assert(region.compressed_size == 0); // not implemented

	region.data = malloc((size_t) region.size);
	fseek(file, (long) region.fsoffset, SEEK_SET);
	fread(region.data,  (size_t) region.size, 1, file);
}

#define loadRegion(region, file, offset) loadRegionImpl(#region, region, file, offset)

struct axfs
{
	axfs_super_onmedia superblock;
	axfs_region strings;
	axfs_region xip;
	axfs_region compressed;
	axfs_region byte_aligned;
	axfs_region node_type;
	axfs_region node_index;
	axfs_region cnode_offset;
	axfs_region cnode_index;
	axfs_region banode_offset;
	axfs_region cblock_offset;
	axfs_region inode_file_size;
	axfs_region inode_name_offset;
	axfs_region inode_num_entries;
	axfs_region inode_mode_index;
	axfs_region inode_array_index;
	axfs_region modes;
	axfs_region uids;
	axfs_region gids;

	void* cblock_buffer = nullptr;
	mutable uint64_t cachedBlock = (uint64_t)-1;

	~axfs()
	{
		free(cblock_buffer);
	}

	void load(const char* filename)
	{
		FILE* file = nullptr;
		auto result = fopen_s(&file, filename, "rb");
		assert(file);
		fread(&superblock, sizeof(superblock), 1, file);
		assert(superblock.magic == 0x48A0E4CD);
		assert(superblock.compression_type == 0); // ZLIB

		loadRegion(xip, file, superblock.xip);
		loadRegion(strings, file, superblock.strings);
		loadRegion(compressed, file, superblock.compressed);
		loadRegion(byte_aligned, file, superblock.byte_aligned);
		loadRegion(node_type, file, superblock.node_type);
		loadRegion(node_index, file, superblock.node_index);
		loadRegion(cnode_offset, file, superblock.cnode_offset);
		loadRegion(cnode_index, file, superblock.cnode_index);
		loadRegion(banode_offset, file, superblock.banode_offset);
		loadRegion(cblock_offset, file, superblock.cblock_offset);
		loadRegion(inode_file_size, file, superblock.inode_file_size);
		loadRegion(inode_name_offset, file, superblock.inode_name_offset);
		loadRegion(inode_num_entries, file, superblock.inode_num_entries);
		loadRegion(inode_mode_index, file, superblock.inode_mode_index);
		loadRegion(inode_array_index, file, superblock.inode_array_index);
		loadRegion(modes, file, superblock.modes);
		loadRegion(uids, file, superblock.uids);
		loadRegion(gids, file, superblock.gids);

		fclose(file);

		printf("%lld files\n", (uint64_t)superblock.files);
		printf("version %d.%d.%d\n", superblock.version_major, superblock.version_minor, superblock.version_sub);

		cblock_buffer = malloc(superblock.cblock_size);
	}

	const char* getName(uint64_t id) const
	{
		auto offset = inode_name_offset.axfs_bytetable_stitch(id);
		return (const char*)((const u8*)strings.data + offset);
	};

	uint64_t getFileSize(uint64_t id) const
	{
		return inode_file_size.axfs_bytetable_stitch(id);
	};

	uint64_t getMode(uint64_t id)const
	{
		auto modeIndex = inode_mode_index.axfs_bytetable_stitch(id);
		return modes.axfs_bytetable_stitch(modeIndex);
	};

	uint64_t getNumEntries(uint64_t id)const
	{
		return inode_num_entries.axfs_bytetable_stitch(id);
	};

	uint64_t getArrayIndex(uint64_t id)const
	{
		return inode_array_index.axfs_bytetable_stitch(id);
	};

	uint64_t getNodeType(uint64_t id) const
	{
		return node_type.axfs_bytetable_stitch(id);
	}

	uint64_t getByteAlignedOffset(uint64_t id) const
	{
		return banode_offset.axfs_bytetable_stitch(id);
	}


	static void* offsetAddress(void* addr, uint64_t offset)
	{
		return ((void*)((uintptr_t)(addr)+(offset)));
	}

	void* readFile(uint64_t id, void* data, uint64_t start, uint64_t length) const
	{
		uint64_t fileSize = getFileSize(id);

		length = std::min(fileSize - start, length);

		uint64_t arrayIndex = getArrayIndex(id);
		uint64_t offset = 0;
		void* out = data;
		while (length > 0)
		{
			uint64_t nodeIndex = getNodeIndex(arrayIndex);

			switch (getNodeType(arrayIndex))
			{
			case 2: // Byte_aligned
			{
				uint64_t srcOffset = getByteAlignedOffset(nodeIndex);
				uint64_t blockSize = std::min(1llu << PAGE_SHIFT, length);
				memcpy(offsetAddress(out, offset), offsetAddress(byte_aligned.data, srcOffset), (size_t) blockSize);
				length -= blockSize;
				offset += blockSize;
				break;
			}
			case 0: // XIP
			{
				memcpy(offsetAddress(out, offset), offsetAddress(xip.data, nodeIndex << PAGE_SHIFT), 1 << PAGE_SHIFT);
				offset += 1 << PAGE_SHIFT;
				length -= 1 << PAGE_SHIFT;
				break;
			}
			case 1: // Compressed
			{
				uint64_t cnodeOffset = cnode_offset.axfs_bytetable_stitch(nodeIndex);
				uint64_t cnodeIndex = cnode_index.axfs_bytetable_stitch(nodeIndex);
				uint64_t srcOffset = cblock_offset.axfs_bytetable_stitch(cnodeIndex);
				uint64_t len = cblock_offset.axfs_bytetable_stitch(cnodeIndex + 1) - srcOffset;
				if (cachedBlock != cnodeIndex)
				{
					stbi_zlib_decode_buffer((char*)cblock_buffer, (int) superblock.cblock_size, (const char*)offsetAddress(compressed.data, srcOffset), (int) len);
					cachedBlock = cnodeIndex;
				}
				len = std::min(superblock.cblock_size - cnodeOffset, length);
				memcpy(offsetAddress(out, offset), offsetAddress(cblock_buffer, cnodeOffset), (size_t) len);
				length -= len;
				offset += len;
				break;
			}
			default:
				assert(false);
				break;
			}

			++arrayIndex;
		}

		return data;
	}

	void printInfo(uint64_t id) const
	{
		uint64_t arrayIndex = getArrayIndex(id);
		uint64_t last = (getFileSize(id) + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
		for (auto i = 0; i < last; ++i)
		{
			switch (getNodeType(arrayIndex + i))
			{
			case 0: // XIP
				printf("X");
				break;
			case 1: // Compressed
				printf("c");
				break;
			case 2: // bytes
				printf("b");
				break;
			default:
				assert(false);
				break;
			}
		}

		printf("\n");
	}

	void ls(uint64_t id, bool recursive = true, int level = 0) const
	{
		uint64_t numFiles = getNumEntries(id);
		uint64_t first = getArrayIndex(id);

		for (uint64_t i = 0; i < numFiles; ++i)
		{
			printf("%3lld:", first + i);
			for (int j = 0; j < level; ++j)
				printf("\t");
			const char* name = getName(first + i);
			auto mode = getMode(first + i);
			if (S_ISDIR(mode))
			{
				printf("%s/\n", name);
				if (recursive)
				{
					ls(first + i, recursive, level + 1);
				}
			}
			else if (S_ISLNK(mode))
			{
				uint64_t size = getFileSize(first + i);
				char linkName[1024];
				readFile(first + i, linkName, 0, sizeof(linkName));
				linkName[size] = 0;
				printf("%s -> %s\n", name, linkName);
			}
			else if (S_ISREG(mode))
			{
				uint64_t size = getFileSize(first + i);
				printf("%s\t%lld ", name, size);
				printInfo(first + i);
			}
			else
			{
				printf("%s?\n", name);
			}
		}
	};

	uint64_t getNodeIndex(uint64_t id) const
	{
		return node_index.axfs_bytetable_stitch(id);
	};

};

int main()
{
	axfs fs;
	fs.load("initrd.img");

	fs.ls(0);

	uint64_t size = fs.getFileSize(19);
	void * data = malloc((size_t) size);
	fs.readFile(19, data, 0, size);
	free(data);
	
    return 0;
}
