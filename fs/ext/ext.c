/*
 * Copyright (C) 2020 user94729 (https://omegazero.org/) and contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
 *
 * Covered Software is provided under this License on an "as is" basis, without warranty of any kind,
 * either expressed, implied, or statutory, including, without limitation, warranties that the Covered Software
 * is free of defects, merchantable, fit for a particular purpose or non-infringing.
 * The entire risk as to the quality and performance of the Covered Software is with You.
 */

#include <klibc/stdlib.h>
#include <klibc/stdint.h>
#include <klibc/stdbool.h>
#include <klibc/string.h>
#include <kernel/msio.h>
#include <kernel/util.h>
#include <kernel/list.h>
#include <kernel/log.h>
#include "ext.h"


static uint32_t ext_incompat_support = EXT_INCOMPAT_FILETYPE | EXT_INCOMPAT_64BIT | EXT_INCOMPAT_EXTENTS | EXT_INCOMPAT_FLEX_BG |
	EXT_INCOMPAT_RECOVER | EXT_INCOMPAT_JOURNAL_DEV;


status_t ext_get_file(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode){
	status_t status = 0;

	uint32_t cInode;
	uint8_t cType;
	status = ext_get_path_inode(driveLabel, partStart, path, &cInode, &cType);
	CERROR();

	if(cType != EXT_INODE_TYPE_FILE)
		FERROR(TSX_NO_SUCH_FILE);
	if(inode)
		*inode = cInode;
	_end:
	return status;
}

status_t ext_get_dir(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode){
	status_t status = 0;
	size_t pathlen = strlen(path) + 1;
	char* pathcpy = kmalloc(pathlen);
	if(!pathcpy)
		FERROR(TSX_OUT_OF_MEMORY);
	memcpy(pathcpy, path, pathlen);
	pathcpy[pathlen] = 0;
	for(size_t i = pathlen - 1; i > 0; i--){ // cut trailing file name ("/path/to/file" -> "/path/to/")
		if(pathcpy[i] != '/')
			pathcpy[i] = 0;
		else
			break;
	}
	uint32_t cInode;
	uint8_t cType;
	status = ext_get_path_inode(driveLabel, partStart, pathcpy, &cInode, &cType);
	kfree(pathcpy, pathlen);
	CERROR();
	if(cType != EXT_INODE_TYPE_DIRECTORY)
		FERROR(TSX_NO_SUCH_DIRECTORY);
	if(inode)
		*inode = cInode;
	_end:
	return status;
}

status_t ext_get_path_inode(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode, uint8_t* type){
	status_t status = 0;
	uint32_t cInode = 2 /* root inode */;
	uint8_t cType = EXT_INODE_TYPE_DIRECTORY;
	ext_inode dirNode;

	path++;
	size_t parts = util_count_parts(path, '/');
	bool found = FALSE;
	for(int i = 0; i < parts; i++){
		if(!*path)
			break;
		size_t pathpartlen = 0;
		while(!(path[pathpartlen] == '/' || path[pathpartlen] == 0))
			pathpartlen++;
		status = ext_get_inode(driveLabel, partStart, cInode, &dirNode);
		CERROR();
		ext_dir_entry* dirEntry = NULL;
		size_t dirTableSize = 0;
		size_t dirTableSizeAbs = 0;
		status = ext_read_inode(driveLabel, partStart, &dirNode, (void**) &dirEntry, &dirTableSize, &dirTableSizeAbs);
		CERROR();
		ext_dir_entry* startEntry = dirEntry;
		while(1){
			if((i < parts - 1 ? (dirEntry->file_type == EXT_INODE_TYPE_DIRECTORY) : true) && pathpartlen == 
				dirEntry->name_len && strncmp(dirEntry->name, path, dirEntry->name_len) == 0){
				found = TRUE;
				break;
			}
			dirEntry = (ext_dir_entry*) ((size_t) dirEntry + dirEntry->rec_len);
			if((size_t) dirEntry >= (size_t) startEntry + dirTableSize || dirEntry->rec_len == 0){
				break;
			}
		}
		if(found){
			cInode = dirEntry->inode;
			cType = dirEntry->file_type;
		}
		kfree(startEntry, dirTableSizeAbs);
		if(!found){
			FERROR(i == parts - 1 ? TSX_NO_SUCH_FILE : TSX_NO_SUCH_DIRECTORY);
		}
		path = util_str_cut_to(path, '/') + 1;
		found = FALSE;
	}

	if(inode)
		*inode = cInode;
	if(type)
		*type = cType;
	_end:
	return status;
}

status_t ext_get_inode(char* driveLabel, uint64_t partStart, uint32_t inode, ext_inode* inodeData){
	status_t status = 0;
	ext_superblock* sb = kmalloc(4096);
	if(!sb)
		FERROR(TSX_OUT_OF_MEMORY);
	ext_group_desc* gd = NULL;
	void* buf = NULL;
	status = msio_read_drive(driveLabel, partStart + 2, 4, (size_t) sb);
	CERROR();
	size_t blockSize = util_math_pow(2, 10 + sb->s_log_block_size);
	uint32_t bg = (inode - 1) / sb->s_inodes_per_group;
	uint32_t blockGroups = sb->s_inodes_count / sb->s_inodes_per_group;
	uint32_t blockGroupsLen = blockGroups * ((sb->s_feature_incompat & EXT_INCOMPAT_64BIT) ? sb->s_desc_size : 32);
	if(blockGroupsLen % 512 != 0)
		blockGroupsLen += 512 - (blockGroupsLen % 512);

	gd = kmalloc(blockGroupsLen);
	if(!gd)
		FERROR(TSX_OUT_OF_MEMORY);
	status = msio_read_drive(driveLabel, partStart + MAX(blockSize, 2048 /* padding + super block */) / 512, blockGroupsLen / 512, (size_t) gd);
	CERROR();
	ext_group_desc* blockGroup = (ext_group_desc*) ((size_t) gd + bg * ((sb->s_feature_incompat & EXT_INCOMPAT_64BIT) ? sb->s_desc_size : 32));
	uint64_t inodeTable = blockGroup->bg_inode_table_lo;
	if((sb->s_feature_incompat & EXT_INCOMPAT_64BIT) && sb->s_desc_size > 32)
		inodeTable |= ((uint64_t) blockGroup->bg_inode_table_hi) << 32;
	uint32_t inodeTableOff = ((inode - 1) % sb->s_inodes_per_group) * (sb->s_rev_level > 0 ? sb->s_inode_size : 128);
	uint64_t lba = (inodeTableOff - inodeTableOff % blockSize + inodeTable * blockSize) / 512 + partStart;
	buf = kmalloc(blockSize);
	if(!buf)
		FERROR(TSX_OUT_OF_MEMORY);
	status = msio_read_drive(driveLabel, lba, blockSize / 512, (size_t) buf);
	CERROR();
	ext_inode* inodeBuf = buf + inodeTableOff % blockSize;
	if(inodeData)
		memcpy(inodeData, inodeBuf, sizeof(ext_inode));
	_end:
	if(sb)
		kfree(sb, 4096);
	if(gd)
		kfree(gd, blockGroupsLen);
	if(buf)
		kfree(buf, blockSize);
	return status;
}

status_t ext_read_inode(char* driveLabel, uint64_t partStart, ext_inode* inode, void** location, size_t* size, size_t* absSizeWrite){
	status_t status = 0;
	ext_superblock* sb = kmalloc(4096);
	if(!sb)
		FERROR(TSX_OUT_OF_MEMORY);
	void* loc = NULL;
	status = msio_read_drive(driveLabel, partStart + 2, 4, (size_t) sb);
	CERROR();
	size_t blockSize = util_math_pow(2, 10 + sb->s_log_block_size);
	if(inode->i_size_high)
		FERROR(TSX_TOO_LARGE);
	size_t absSize = inode->i_size_lo;
	if(absSize % blockSize != 0)
		absSize += blockSize - (absSize % blockSize);
	loc = kmalloc(absSize);
	if(!loc)
		FERROR(TSX_OUT_OF_MEMORY);
	*location = loc;
	*size = inode->i_size_lo;
	if(absSizeWrite)
		*absSizeWrite = absSize;
	status = ext_read_inode_to(driveLabel, partStart, inode, loc);
	CERROR();
	_end:
	if(sb)
		kfree(sb, 4096);
	if(loc && status != TSX_SUCCESS)
		kfree(loc, absSize);
	return status;
}

status_t ext_read_inode_to(char* driveLabel, uint64_t partStart, ext_inode* inode, void* location){
	status_t status = 0;
	ext_superblock* sb = kmalloc(4096);
	if(!sb)
		FERROR(TSX_OUT_OF_MEMORY);
	status = msio_read_drive(driveLabel, partStart + 2, 4, (size_t) sb);
	CERROR();
	size_t blockSize = util_math_pow(2, 10 + sb->s_log_block_size);
	kfree(sb, 4096);
	if(inode->i_flags & EXT_INODE_EXTENTS_FL){
		ext_extent_header* header = (ext_extent_header*) &inode->i_blocks[0];
		status = ext_read_extent(driveLabel, partStart, header, blockSize, location);
		CERROR();
	}else{
		for(int i = 0; i < 12; i++){
			if(!inode->i_blocks[i])
				break;
			uint64_t lba = inode->i_blocks[i] * blockSize / 512 + partStart;
			status = msio_read_drive(driveLabel, lba, blockSize / 512, (size_t) location);
			CERROR();
			location += blockSize;
		}
		if(inode->i_block_i1){
			status = ext_read_indirect_blocks(driveLabel, partStart, blockSize, inode->i_block_i1, 0, &location);
			CERROR();
		}
		if(inode->i_block_i2){
			status = ext_read_indirect_blocks(driveLabel, partStart, blockSize, inode->i_block_i2, 1, &location);
			CERROR();
		}
		if(inode->i_block_i3){
			status = ext_read_indirect_blocks(driveLabel, partStart, blockSize, inode->i_block_i3, 2, &location);
			CERROR();
		}
	}
	_end:
	return status;
}

status_t ext_read_indirect_blocks(char* driveLabel, uint64_t partStart, uint32_t blockSize, uint32_t blockTable, uint32_t depth, void** writeLoc){
	status_t status = 0;
	uint32_t* table = kmalloc(blockSize);
	if(!table)
		FERROR(TSX_OUT_OF_MEMORY);
	status = msio_read_drive(driveLabel, blockTable * blockSize / 512 + partStart, blockSize / 512, (size_t) table);
	CERROR();
	if(depth == 0){
		for(int i = 0; i < blockSize / 4; i++){
			if(!table[i])
				break;
			uint64_t lba = table[i] * blockSize / 512 + partStart;
			status = msio_read_drive(driveLabel, lba, blockSize / 512, (size_t) *writeLoc);
			CERROR();
			*writeLoc += blockSize;
		}
	}else{
		for(int i = 0; i < blockSize / 4; i++){
			if(!table[i])
				break;
			status = ext_read_indirect_blocks(driveLabel, partStart, blockSize, table[i], depth - 1, writeLoc);
			CERROR();
		}
	}
	_end:
	if(table)
		kfree(table, blockSize);
	return status;
}

status_t ext_read_extent(char* driveLabel, uint64_t partStart, ext_extent_header* header, size_t blockSize, void* destLocation){
	status_t status = 0;
	void* tempData = kmalloc(blockSize);
	if(!tempData)
		FERROR(TSX_OUT_OF_MEMORY);
	if(header->eh_magic != EXT_INODE_EXTENT_HEADER_MAGIC)
		FERROR(TSX_INVALID_FORMAT);
	if(header->eh_depth == 0){
		ext_extent* extents = (ext_extent*) ((size_t) header + sizeof(ext_extent_header));
		for(size_t i = 0; i < header->eh_entries; i++){
			uint64_t lbaOff = (uint64_t) (extents[i].ee_start_lo | ((uint64_t) extents[i].ee_start_hi << 32)) * blockSize / 512;
			status = msio_read_drive(driveLabel, partStart + lbaOff, extents[i].ee_len * blockSize / 512, (size_t) destLocation + blockSize * extents[i].ee_block);
			CERROR();
		}
	}else{
		ext_extent_idx* extentNodes = (ext_extent_idx*) ((size_t) header + sizeof(ext_extent_header));
		for(size_t i = 0; i < header->eh_entries; i++){
			uint64_t lbaOff = (uint64_t) (extentNodes[i].ei_leaf_lo | ((uint64_t) extentNodes[i].ei_leaf_hi << 32)) * blockSize / 512;
			status = msio_read_drive(driveLabel, partStart + lbaOff, blockSize / 512, (size_t) tempData);
			CERROR();
			status = ext_read_extent(driveLabel, partStart, tempData, blockSize, destLocation);
			CERROR();
		}
	}
	_end:
	if(tempData)
		kfree(tempData, blockSize);
	return status;
}


bool vfs_isFilesystem(char* driveLabel, uint64_t partStart){
	ext_superblock* sb = kmalloc(4096);
	if(!sb)
		return FALSE;
	status_t status = msio_read_drive(driveLabel, partStart + 2, 1, (size_t) sb);
	if(status == TSX_SUCCESS && sb->s_magic == EXT_MAGIC && (sb->s_feature_incompat & ~ext_incompat_support) == 0){
		kfree(sb, 4096);
		return TRUE;
	}else{
		kfree(sb, 4096);
		return FALSE;
	}
}

status_t vfs_readFile(char* driveLabel, uint64_t partStart, char* path, size_t dest){
	uint32_t inode = 0;
	void* destTemp = NULL;
	status_t status = ext_get_file(driveLabel, partStart, path, &inode);
	CERROR();
	ext_inode inodeData;
	status = ext_get_inode(driveLabel, partStart, inode, &inodeData);
	CERROR();
	// the caller will probably only explicitly allocate a buffer of size fileSize, but data is read to a buffer of size fileSizeAbs (which is larger)
	// use a temporary buffer to read to to only ever read to explicitly allocated memory
	size_t fileSize = 0;
	size_t fileSizeAbs = 0;
	status = ext_read_inode(driveLabel, partStart, &inodeData, &destTemp, &fileSize, &fileSizeAbs);
	CERROR();
	memcpy((void*) dest, destTemp, fileSize);
	_end:
	if(destTemp)
		kfree(destTemp, fileSizeAbs);
	return status;
}

status_t vfs_getFileSize(char* driveLabel, uint64_t partStart, char* path, size_t* sizeWrite){
	uint32_t inode = 0;
	status_t status = ext_get_file(driveLabel, partStart, path, &inode);
	CERROR();
	ext_inode inodeData;
	status = ext_get_inode(driveLabel, partStart, inode, &inodeData);
	CERROR();
	size_t size = inodeData.i_size_lo;
	if(sizeWrite)
		*sizeWrite = size;
	_end:
	return status;
}

status_t vfs_listDir(char* driveLabel, uint64_t partStart, char* path, list_array** listWrite){
	uint32_t inode = 0;
	status_t status = ext_get_dir(driveLabel, partStart, path, &inode);
	CERROR();
	ext_inode inodeData;
	status = ext_get_inode(driveLabel, partStart, inode, &inodeData);
	CERROR();
	ext_dir_entry* dirEntry = NULL;
	size_t dirTableSize = 0;
	size_t dirTableSizeAbs = 0;
	status = ext_read_inode(driveLabel, partStart, &inodeData, (void**) &dirEntry, &dirTableSize, &dirTableSizeAbs);
	CERROR();
	ext_dir_entry* startEntry = dirEntry;
	list_array* list = list_array_create(0);
	while(1){
		if(dirEntry->name_len > 0){
			char* name = kmalloc(dirEntry->name_len + 1);
			if(!name)
				FERROR(TSX_OUT_OF_MEMORY);
			memcpy(name, dirEntry->name, dirEntry->name_len);
			name[dirEntry->name_len] = 0;
			list_array_push(list, name);
		}
		dirEntry = (ext_dir_entry*) ((size_t) dirEntry + dirEntry->rec_len);
		if((size_t) dirEntry >= (size_t) startEntry + dirTableSize || dirEntry->rec_len == 0){
			break;
		}
	}
	kfree(startEntry, dirTableSizeAbs);
	*listWrite = list;
	_end:
	return status;
}

