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
/*
 * linux86.c - Boot handler for the linux kernel.
 */

#include <klibc/stdlib.h>
#include <klibc/stdint.h>
#include <klibc/stdbool.h>
#include <klibc/string.h>
#include <klibc/stdio.h>
#include <kernel/kutil.h>
#include <kernel/parse.h>
#include <kernel/log.h>
#include "linux86.h"




status_t kboot_start(parse_entry* entry){
	status_t status = 0;

	if(kernel_get_s1data()->bootFlags & S1BOOT_DATA_BOOT_FLAGS_UEFI){
		log_error("Linux boot is currently not supported on UEFI\n");
		FERROR(TSX_UNSUPPORTED);
	}

	char* kfile = parse_get_option(entry, "kernel");
	char* initrdfile = parse_get_option(entry, "initrd");
	char* args = parse_get_option(entry, "args");
	if(!(kfile != NULL && initrdfile != NULL && args != NULL)){
		FERROR(TSX_MISSING_ARGUMENTS);
	}

	status = linux86_start(kfile, initrdfile, args);
	CERROR();
	_end:
	return status;
}


status_t linux86_start(char* kernel_file, char* initrd_file, char* cmd){
	status_t status = 0;
	void* kernelLocation = 0;
	void* initrdLocation = 0;

	char* kernelFilePath = kernel_write_boot_file(kernel_file);
	if(kernelFilePath == 0)
		FERROR(TSX_OUT_OF_MEMORY);

	size_t kernelSize = 0;
	status = kernel_read_file(kernelFilePath, (size_t*) &kernelLocation, &kernelSize);
	kfree(kernelFilePath, strlen(kernelFilePath));
	CERROR();

	linux86_setup_header* setup_header = kernelLocation + 0x1f1;
	if(setup_header->headerMagic != LINUX86_HEADER_MAGIC)
		FERROR(TSX_INVALID_FORMAT);
	if(setup_header->version < 0x202){
		log_error("Linux kernels with boot protocol version below 2.02 are unsupported\n");
		FERROR(TSX_UNSUPPORTED);
	}
	if(!(setup_header->loadflags & 0x1)){
		log_error("zImage kernels are unsupported\n");
		FERROR(TSX_UNSUPPORTED);
	}
	setup_header->vid_mode = 0xffff;
	setup_header->type_of_loader = 0xff;
	uint32_t setup_sect_size = (setup_header->setup_sects + 1) * 0x200;
	kernel_s3boot_add_mem_region(LINUX86_BASE_PTR, setup_sect_size, (size_t) kernelLocation);

	char* kernelName = kernelLocation + setup_header->kernel_version + 0x200;
	log_debug("Linux %s\n", kernelName);
	log_debug("Arguments: %s\n", cmd);

	kernel_s3boot_add_mem_region(0x100000, kernelSize - setup_sect_size, (size_t) kernelLocation + setup_sect_size);

	char* initrdFilePath = kernel_write_boot_file(initrd_file);
	if(initrdFilePath == 0)
		FERROR(TSX_OUT_OF_MEMORY);

	size_t initrdSize = 0;
	status = kernel_read_file(initrdFilePath, (size_t*) &initrdLocation, &initrdSize);
	kfree(initrdFilePath, strlen(initrdFilePath));
	CERROR();

	if((size_t) initrdLocation + initrdSize > (setup_header->version >= 0x203 ? setup_header->initrd_addr_max : 0x37ffffff))
		FERROR(TSX_ERROR);

	setup_header->ramdisk_image = (uint32_t) initrdLocation;
	setup_header->ramdisk_size = initrdSize;

	setup_header->heap_end_ptr = LINUX86_HEAP_END - 0x200;
	setup_header->loadflags |= 0x80; // heap

	setup_header->cmd_line_ptr = LINUX86_BASE_PTR + LINUX86_HEAP_END;
	kernel_s3boot_add_mem_region(setup_header->cmd_line_ptr, MIN(0x2000, strlen(cmd)), (size_t) cmd);

	uint16_t seg = LINUX86_BASE_PTR >> 4;

	arch_os_entry_state entryState;
	memset(&entryState, 0, sizeof(arch_os_entry_state));
	entryState.sp = LINUX86_HEAP_END;
	entryState.bp = LINUX86_HEAP_END;
	entryState.cs = seg + 0x20;
	entryState.ds = seg;

	kernel_jump(&entryState, 0, KERNEL_S3BOOT_BMODE_16, 1 /* disable interrupts */);

	_end:
	if(kernelLocation)
		kfree(kernelLocation, kernelSize);
	if(initrdLocation)
		kfree(initrdLocation, initrdSize);
	return status;
}

