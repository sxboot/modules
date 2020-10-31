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

#ifndef __LINUX86_H__
#define __LINUX86_H__

#ifndef ARCH_UPSTREAM_x86
#error linux86 is only compatible with x86
#endif

#include <klibc/stdlib.h>
#include <klibc/stdint.h>


#define LINUX86_HEADER_MAGIC 0x53726448

#define LINUX86_BASE_PTR 0x70000
#define LINUX86_HEAP_END 0xe000

#pragma pack(push,1)
typedef struct linux86_setup_header{
	uint8_t setup_sects;
	uint16_t root_flags;
	uint32_t syssize;
	uint16_t ram_size;
	uint16_t vid_mode;
	uint16_t root_dev;
	uint16_t boot_flag;
	uint16_t jump;
	uint32_t headerMagic;
	uint16_t version;
	uint32_t realmode_switch;
	uint16_t start_sys_seg;
	uint16_t kernel_version;
	uint8_t type_of_loader;
	uint8_t loadflags;
	uint16_t setup_move_size;
	uint32_t code32_start;
	uint32_t ramdisk_image;
	uint32_t ramdisk_size;
	uint32_t bootsect_kludge;
	uint16_t heap_end_ptr;
	uint8_t ext_loader_ver;
	uint8_t ext_loader_type;
	uint32_t cmd_line_ptr;
	uint32_t initrd_addr_max;
	uint32_t kernel_alignment;
	uint8_t relocatable_kernel;
	uint8_t min_alignment;
	uint16_t xloadflags;
	uint32_t cmdline_size;
	uint32_t hardware_subarch;
	uint64_t hardware_subarch_data;
	uint32_t payload_offset;
	uint32_t payload_length;
	uint64_t setup_data;
	uint64_t pref_address;
	uint32_t init_size;
	uint32_t handover_offset;
	uint32_t kernel_info_offset;
} linux86_setup_header;
#pragma pack(pop)

status_t linux86_start(char* kernel_file, char* initrd_file, char* cmd);

#endif /* __LINUX86_H__ */
