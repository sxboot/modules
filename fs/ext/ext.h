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

#ifndef __EXT_H__
#define __EXT_H__


#define EXT_MAGIC 0xEF53

#define EXT_INCOMPAT_COMPRESSION 0x1
#define EXT_INCOMPAT_FILETYPE 0x2
#define EXT_INCOMPAT_RECOVER 0x4
#define EXT_INCOMPAT_JOURNAL_DEV 0x8
#define EXT_INCOMPAT_META_BG 0x10
#define EXT_INCOMPAT_EXTENTS 0x40
#define EXT_INCOMPAT_64BIT 0x80
#define EXT_INCOMPAT_MMP 0x100
#define EXT_INCOMPAT_FLEX_BG 0x200
#define EXT_INCOMPAT_EA_INODE 0x400
#define EXT_INCOMPAT_DIRDATA 0x1000
#define EXT_INCOMPAT_CSUM_SEED 0x2000
#define EXT_INCOMPAT_LARGEDIR 0x4000
#define EXT_INCOMPAT_INLINE_DATA 0x8000
#define EXT_INCOMPAT_ENCRYPT 0x10000

#define EXT_INODE_TYPE_FILE 1
#define EXT_INODE_TYPE_DIRECTORY 2

#define EXT_INODE_EXTENT_HEADER_MAGIC 0xF30A

#define EXT_INODE_EXTENTS_FL 0x80000


#pragma pack(push,1)
typedef struct ext_superblock{
	uint32_t s_inodes_count;
	uint32_t s_blocks_count_lo;
	uint32_t s_r_blocks_count_lo;
	uint32_t s_free_blocks_count_lo;
	uint32_t s_free_inodes_count;
	uint32_t s_first_data_block;
	uint32_t s_log_block_size;
	uint32_t s_log_cluster_size;
	uint32_t s_blocks_per_group;
	uint32_t s_clusters_per_group;
	uint32_t s_inodes_per_group;
	uint32_t s_mtime;
	uint32_t s_wtime;
	uint16_t s_mnt_count;
	uint16_t s_max_mnt_count;
	uint16_t s_magic;
	uint16_t s_state;
	uint16_t s_errors;
	uint16_t s_minor_rev_level;
	uint32_t s_lastcheck;
	uint32_t s_checkinterval;
	uint32_t s_creator_os;
	uint32_t s_rev_level;
	uint16_t s_def_resuid;
	uint16_t s_def_resgid;

	uint32_t s_first_ino;
	uint16_t s_inode_size;
	uint16_t s_block_group_nr;
	uint32_t s_feature_compat;
	uint32_t s_feature_incompat;
	uint32_t s_feature_ro_compat;
	uint8_t s_uuid[16];
	char s_volume_name[16];
	char s_last_mounted[64];
	uint32_t s_algorithm_usage_bitmap;

	uint8_t s_prealloc_blocks;
	uint8_t s_prealloc_dir_blocks;
	uint16_t s_reserved_gdt_blocks;

	uint8_t s_journal_uuid[16];
	uint32_t s_journal_inum;
	uint32_t s_journal_dev;
	uint32_t s_last_orphan;
	uint32_t s_hash_seed[4];
	uint8_t s_def_hash_version;
	uint8_t s_jnl_backup_type;
	uint16_t s_desc_size;
	uint32_t s_default_mount_opts;
	uint32_t s_first_meta_bg;
	uint32_t s_mkfs_time;
	uint32_t s_jnl_blocks[17];

	uint32_t s_blocks_count_hi;
	uint32_t s_r_blocks_count_hi;
	uint32_t s_free_blocks_count_hi;
	uint16_t s_min_extra_isize;
	uint16_t s_want_extra_isize;
	uint32_t s_flags;
	// ....
} ext_superblock;

typedef struct ext_group_desc{
	uint32_t bg_block_bitmap_lo;
	uint32_t bg_inode_bitmap_lo;
	uint32_t bg_inode_table_lo;
	uint16_t bg_free_blocks_count_lo;
	uint16_t bg_free_inodes_count_lo;
	uint16_t bg_used_dirs_count_lo;
	uint16_t bg_flags;
	uint32_t bg_exclude_bitmap_lo;
	uint16_t bg_block_bitmap_csum_lo;
	uint16_t bg_inode_bitmap_csum_lo;
	uint16_t bg_itable_unused_lo;
	uint16_t bg_checksum;

	uint32_t bg_block_bitmap_hi;
	uint32_t bg_inode_bitmap_hi;
	uint32_t bg_inode_table_hi;
	uint16_t bg_free_blocks_count_hi;
	uint16_t bg_free_inodes_count_hi;
	uint16_t bg_used_dirs_count_hi;
	uint16_t bg_itable_unused_hi;
	uint32_t bg_exclude_bitmap_hi;
	uint16_t bg_block_bitmap_csum_hi;
	uint16_t bg_inode_bitmap_csum_hi;
	uint32_t bg_reserved;
} ext_group_desc;

typedef struct ext_inode{
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size_lo;
	uint32_t i_atime;
	uint32_t i_ctime;
	uint32_t i_mtime;
	uint32_t i_dtime;
	uint16_t i_gid;
	uint16_t i_links_count;
	uint32_t i_blocks_lo;
	uint32_t i_flags;
	uint32_t i_osd1;
	uint32_t i_blocks[12];
	uint32_t i_block_i1;
	uint32_t i_block_i2;
	uint32_t i_block_i3;
	uint32_t i_generation;
	uint32_t i_file_acl_lo;
	uint32_t i_size_high;
	uint32_t i_obso_faddr;
	uint32_t i_osd2;
	uint16_t i_extra_isize;
	uint16_t i_checksum_hi;
	uint32_t i_ctime_extra;
	uint32_t i_mtime_extra;
	uint32_t i_atime_extra;
	uint32_t i_crtime;
	uint32_t i_crtime_extra;
	uint32_t i_version_hi;
	uint32_t i_projid;
} ext_inode;

typedef struct ext_extent_header{
	uint16_t eh_magic;
	uint16_t eh_entries;
	uint16_t eh_max;
	uint16_t eh_depth;
	uint32_t eh_generation;
} ext_extent_header;

typedef struct ext_extent_idx{
	uint32_t ei_block;
	uint32_t ei_leaf_lo;
	uint16_t ei_leaf_hi;
	uint16_t ei_unused;
} ext_extent_idx;

typedef struct ext_extent{
	uint32_t ee_block;
	uint16_t ee_len;
	uint16_t ee_start_hi;
	uint32_t ee_start_lo;
} ext_extent;

typedef struct ext_dir_entry{
	uint32_t inode;
	uint16_t rec_len;
	uint8_t name_len;
	uint8_t file_type;
	char name[0];
} ext_dir_entry;
#pragma pack(pop)


status_t ext_get_file(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode);
status_t ext_get_dir(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode);
status_t ext_get_path_inode(char* driveLabel, uint64_t partStart, char* path, uint32_t* inode, uint8_t* type);
status_t ext_get_inode(char* driveLabel, uint64_t partStart, uint32_t inode, ext_inode* inodeData);
status_t ext_read_inode(char* driveLabel, uint64_t partStart, ext_inode* inode, void** location, size_t* size, size_t* absSizeWrite);
status_t ext_read_inode_to(char* driveLabel, uint64_t partStart, ext_inode* inode, void* location);
status_t ext_read_indirect_blocks(char* driveLabel, uint64_t partStart, uint32_t blockSize, uint32_t blockTable, uint32_t depth, void** writeLoc);
status_t ext_read_extent(char* driveLabel, uint64_t partStart, ext_extent_header* header, size_t blockSize, void* destLocation);

#endif /* __EXT_H__ */
