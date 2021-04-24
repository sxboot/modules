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
 * ubi.c - Implementation of the UBI boot protocol.
 * Specification: https://static.omegazero.org/d/spec/ubi/ubi_1_0.pdf
 */

#include <klibc/stdlib.h>
#include <klibc/stdint.h>
#include <klibc/stdbool.h>
#include <klibc/string.h>
#include <klibc/stdio.h>
#include <kernel/kutil.h>
#include <kernel/parse.h>
#include <kernel/errc.h>
#include <kernel/log.h>
#include <kernel/elf.h>
#include <kernel/pe.h>
#include <kernel/mmgr.h>
#include <kernel/stdio64.h>
#include <kernel/list.h>
#include <kernel/vfs.h>
#include <kernel/util.h>
#include <kernel/dynl.h>
#include <shared/s1bootdecl.h>
#include "ubi.h"


#define UBI_VERSION_MAJOR 1
#define UBI_VERSION_MINOR 0


static s1boot_data* s1data = NULL;
static parse_entry* configData = NULL;

static char* kernelPartition = NULL;
static char* kernelPath = NULL;
static size_t kernelImgLocation = 0;
static size_t kernelImgSize = 0;

static ubi_b_root_table* ubi_root = NULL;
static ubi_k_root_table* ubi_kernel = NULL;
static size_t ubi_kernel_type = 0; // 1 - elf, 2 - pe
static void* ubi_kernel_location = NULL;
static size_t ubi_kernel_base = 0;
static size_t ubi_kernel_top = 0;
static size_t ubi_kernel_offset = 0;

static ubi_b_table_header* lastTable = NULL;

static char* kernel_args = NULL;


static bool ubi_clearScreen = FALSE;


status_t kboot_start(parse_entry* entry){
	status_t status = 0;
	s1data = kernel_get_s1data();
	configData = entry;

	reloc_ptr((void**) &ubi_root);
	reloc_ptr((void**) &ubi_kernel);
	reloc_ptr((void**) &ubi_kernel_location);

	char* pfile = parse_get_option(configData, "file");
	if(!(pfile != NULL)){
		FERROR(TSX_MISSING_ARGUMENTS);
	}

	kernel_args = parse_get_option(configData, "args");

	status = ubi_start(pfile);
	CERROR();
	_end:
	if(ubi_root){
		ubi_b_table_header* table = (ubi_b_table_header*) ubi_root;
		while(table){
			kfree(table, ubi_get_table_size(table->magic));
			table = table->nextTable;
		}
	}
	del_reloc_ptr((void**) &ubi_kernel_location);
	del_reloc_ptr((void**) &ubi_kernel);
	del_reloc_ptr((void**) &ubi_root);

	ubi_root = NULL;
	ubi_kernel = NULL;
	ubi_kernel_type = 0;
	ubi_kernel_location = NULL;
	ubi_kernel_base = 0;
	ubi_kernel_top = 0;
	ubi_kernel_offset = 0;
	lastTable = NULL;
	return status;
}


status_t ubi_start(char* file){
	status_t status = 0;

	size_t oldRelocBase = kernel_get_reloc_base();

	ubi_root = kmalloc(sizeof(ubi_b_root_table));
	if(!ubi_root){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(ubi_root, 0, sizeof(ubi_b_root_table));
	ubi_root->hdr.magic = UBI_B_ROOT_MAGIC;
	reloc_ptr((void**) &ubi_root->hdr.nextTable);
	ubi_root->specificationVersionMajor = UBI_VERSION_MAJOR;
	ubi_root->specificationVersionMinor = UBI_VERSION_MINOR;
	log_info("Universal Boot Interface version %u.%u\n", UBI_VERSION_MAJOR, UBI_VERSION_MINOR);
	ubi_root->flags |= (s1data->bootFlags & S1BOOT_DATA_BOOT_FLAGS_UEFI) ? UBI_FLAGS_FIRMWARE_UEFI : UBI_FLAGS_FIRMWARE_BIOS;
	ubi_set_checksum(&ubi_root->hdr, sizeof(ubi_b_root_table));
	lastTable = &ubi_root->hdr;

	status = ubi_load_kernel(file);
	CERROR();

	if(ubi_kernel->hdr.magic != UBI_K_ROOT_MAGIC){
		log_error("Kernel header is invalid (magic=0x%X)\n", ubi_kernel->hdr.magic);
		FERROR(TSX_INVALID_FORMAT);
	}
	if(ubi_kernel->minimumSpecificationVersionMajor > UBI_VERSION_MAJOR || ubi_kernel->minimumSpecificationVersionMinor > UBI_VERSION_MINOR){
		log_error("Kernel file requires UBI version %u.%u\n", ubi_kernel->minimumSpecificationVersionMajor, ubi_kernel->minimumSpecificationVersionMinor);
		FERROR(TSX_UNAVAILABLE);
	}
	if(ubi_kernel->bits != sizeof(size_t) * 8){
		log_error("Kernel file is %u-bit (not %u-bit)\n", ubi_kernel->bits, sizeof(size_t) * 8);
		FERROR(TSX_INVALID_FORMAT);
	}

	status = ubi_create_tables(&ubi_kernel->hdr);
	CERROR();

	if(!(ubi_kernel->flags & UBI_FLAGS_FIRMWARE_UEFI_EXIT)){ // not keep boot services
		status = kernel_exit_uefi();
		CERROR();
		ubi_root->flags |= UBI_FLAGS_FIRMWARE_UEFI_EXIT; // set exit boot services called
	}

	status = ubi_load_kernel_segs();
	CERROR();

	status = ubi_post_init();
	CERROR();

	ubi_status_t kreturn = ubi_call_kernel();
	log_warn("Kernel returned status %u\n", (size_t) kreturn);
	kernel_halt(); // at this point it may be too unsafe to return (we dont know what the kernel does)

	_end:
	if(kernel_get_reloc_base() != oldRelocBase)
		kernel_relocate(oldRelocBase);
	return status;
}

status_t ubi_load_kernel(char* filename){
	status_t status = 0;

	char* filePath = kernel_write_boot_file(filename);
	if(filePath == 0)
		FERROR(TSX_OUT_OF_MEMORY);

	size_t size = 0;
	size_t imglocation = 0;
	status = kernel_read_file(filePath, &imglocation, &size);
	CERROR();

	size_t blLoc = kernel_get_reloc_base();

	if(elf_is_elf((elf_file*) imglocation)){
		log_debug("Kernel is ELF file\n");

		ubi_kernel_type = 1;
		ubi_kernel_location = (void*) imglocation;

		elf_file* file = (elf_file*) imglocation;

#if defined(ARCH_amd64)
		if(file->e_machine != ELF_MACHINE_AMD64){
			log_error("ELF file is not compatible with amd64 (e_machine=0x%X)\n", file->e_machine);
			FERROR(TSX_INVALID_FORMAT);
		}
#elif defined(ARCH_i386)
		if(file->e_machine != ELF_MACHINE_i386){
			log_error("ELF file is not compatible with i386 (e_machine=0x%X)\n", file->e_machine);
			FERROR(TSX_INVALID_FORMAT);
		}
#endif

		// get header
		elf_symtab* headerSym = elf_get_symtab_entry(file, "ubi_header");
		if(headerSym){
			ubi_kernel = ubi_get_file_addr(headerSym->st_value);
		}else{
			elf_sh* ubihdrSec = elf_get_sh_entry(file, ".ubihdr");
			if(ubihdrSec){
				ubi_kernel = ubi_get_file_addr(ubihdrSec->sh_addr);
			}
		}

		elf_ph* ph = elf_get_ph(file);
		if(ph == 0)
			FERROR(TSX_INVALID_FORMAT);
		if(file->e_phentsize != sizeof(elf_ph))
			FERROR(TSX_INVALID_FORMAT);

		size_t minAddr = SIZE_MAX;
		size_t maxAddr = 0;
		for(int i = 0; i < file->e_phnum; i++){
			if(ph[i].p_type != ELF_PH_TYPE_LOAD)
				continue;
			if(ph[i].p_vaddr < minAddr)
				minAddr = ph[i].p_vaddr;
			if(ph[i].p_vaddr + ph[i].p_memsz > maxAddr)
				maxAddr = ph[i].p_vaddr + ph[i].p_memsz;
		}
		if(minAddr == SIZE_MAX)
			FERROR(TSX_ERROR);
		ubi_kernel_base = minAddr;
		ubi_kernel_top = maxAddr;
	}else if(mz_is_mz((mz_file*) imglocation) && pe_is_pe(mz_get_pe((mz_file*) imglocation))){
		log_debug("Kernel is PE file\n");

		ubi_kernel_type = 2;
		ubi_kernel_location = (void*) imglocation;

		pe_file* file = mz_get_pe((mz_file*) imglocation);

#if defined(ARCH_amd64)
		if(file->p_machine != PE_MACHINE_AMD64){
			log_error("PE file is not compatible with amd64 (p_machine=0x%X)\n", file->p_machine);
			FERROR(TSX_INVALID_FORMAT);
		}
#elif defined(ARCH_i386)
		if(file->p_machine != PE_MACHINE_i386){
			log_error("PE file is not compatible with i386 (p_machine=0x%X)\n", file->p_machine);
			FERROR(TSX_INVALID_FORMAT);
		}
#endif

		pe_section_header* section = pe_get_section(file, ".ubihdr");
		if(section){
			ubi_kernel = ubi_get_file_addr(section->ps_vaddr);
		}

		pe_section_header* sections = pe_get_sections(file);
		if(sections == 0)
			FERROR(TSX_INVALID_FORMAT);

		size_t minAddr = SIZE_MAX;
		size_t maxAddr = 0;
		for(size_t i = 0; i < file->p_sections; i++){
			if(sections[i].ps_vaddr < minAddr)
				minAddr = sections[i].ps_vaddr;
			if(sections[i].ps_vaddr + sections[i].ps_vsize > maxAddr)
				maxAddr = sections[i].ps_vaddr + sections[i].ps_vsize;
		}
		if(minAddr == SIZE_MAX)
			FERROR(TSX_ERROR);
		ubi_kernel_base = minAddr;
		ubi_kernel_top = maxAddr;
	}else{
		log_error("File format not recognized\n");
		FERROR(TSX_INVALID_FORMAT);
	}
	if(!ubi_kernel){
		log_error("No UBI kernel header found in kernel file\n");
		FERROR(TSX_INVALID_FORMAT);
	}

	log_debug("ubi_k_root_table=%Y\n", (size_t) ubi_kernel);

	if(ubi_kernel_top - ubi_kernel_base == 0){
		log_error("Kernel is empty\n");
		FERROR(TSX_ERROR);
	}

	kernelPartition = kmalloc(strlen(filePath) + 1);
	memcpy(kernelPartition, filePath, strlen(filePath) + 1);
	*strchr(kernelPartition + 1, '/') = 0;
	reloc_ptr((void**) &kernelPartition);

	kernelPath = kmalloc(strlen(filename) + 1);
	char* rfile = strchr(filename + 1, '/'); // cut off the preceding partition location (eg "/[BDRIVE]/")
	memcpy(kernelPath, rfile, strlen(rfile) + 1);
	reloc_ptr((void**) &kernelPath);

	kernelImgLocation = imglocation;
	kernelImgSize = size;
	reloc_ptr((void**) &kernelImgLocation);

	_end:
	if(filePath)
		kfree(filePath, strlen(filePath) + 1);
	if(status != TSX_SUCCESS){
		if(imglocation)
			kfree((void*) imglocation, size);
	}
	return status;
}

status_t ubi_load_kernel_segs(){
	status_t status = 0;

	if(ubi_kernel_type == 1){
		elf_file* file = ubi_kernel_location;

		// load segments
		elf_ph* ph = elf_get_ph(file);
		if(ph == 0)
			FERROR(TSX_INVALID_FORMAT);
		status = ubi_relocate(ubi_kernel_base + ubi_kernel_offset, ubi_kernel_top + ubi_kernel_offset);
		CERROR();
		file = ubi_kernel_location; // file location may have been changed by ubi_relocate
		ph = elf_get_ph(file);

		elf_loaded_image* image = kmalloc(sizeof(elf_loaded_image));
		if(!image)
			FERROR(TSX_OUT_OF_MEMORY);
		elf_gen_loaded_image_data(file, ubi_kernel_offset, image);

		for(int i = 0; i < file->e_phnum; i++){
			if(ph[i].p_type != ELF_PH_TYPE_LOAD)
				continue;
			void* secLoc = mmgr_alloc_block_sequential(ph[i].p_memsz);
			if(!secLoc)
				FERROR(TSX_OUT_OF_MEMORY);
			mmgr_reserve_mem_region((size_t) secLoc, ph[i].p_memsz, MMGR_MEMTYPE_OS);
			log_debug("%Y -> %Y (0x%X) : 0x%X (0x%X)\n", ph[i].p_vaddr + ubi_kernel_offset, (size_t) secLoc, ph[i].p_memsz, ph[i].p_offset, ph[i].p_filesz);

			size_t segSize = ph[i].p_memsz + ph[i].p_vaddr % VMMGR_PAGE_SIZE;
			vmmgr_map_pages_req(mmgr_get_used_blocks() * MMGR_BLOCK_SIZE + segSize);
			for(size_t addr = 0; addr < segSize; addr += VMMGR_PAGE_SIZE){
				if(vmmgr_is_address_accessible(ph[i].p_vaddr + ubi_kernel_offset + addr)){
					continue;
				}
				vmmgr_map_page((size_t) secLoc + addr, ph[i].p_vaddr + ubi_kernel_offset + addr);
			}
			memset((void*) (ph[i].p_vaddr + ubi_kernel_offset), 0, ph[i].p_memsz);
			memcpy((void*) (ph[i].p_vaddr + ubi_kernel_offset), (void*) (ph[i].p_offset + (size_t) file), ph[i].p_filesz);
		}
		if(file->e_type == ELF_ET_DYN)
			dynl_link_image_to_image(image, image);
		kfree(image, sizeof(elf_loaded_image));
	}else if(ubi_kernel_type == 2){
		pe_file* file = mz_get_pe(ubi_kernel_location);

		pe_section_header* sections = pe_get_sections(file);
		if(sections == 0)
			FERROR(TSX_INVALID_FORMAT);
		status = ubi_relocate(ubi_kernel_base + ubi_kernel_offset, ubi_kernel_top + ubi_kernel_offset);
		CERROR();
		file = mz_get_pe(ubi_kernel_location); // file location may have been changed by ubi_relocate
		sections = pe_get_sections(file);

		for(int i = 0; i < file->p_sections; i++){
			void* secLoc = mmgr_alloc_block_sequential(sections[i].ps_vsize);
			if(!secLoc)
				FERROR(TSX_OUT_OF_MEMORY);
			mmgr_reserve_mem_region((size_t) secLoc, sections[i].ps_vsize, MMGR_MEMTYPE_OS);
			log_debug("%Y -> %Y (0x%X) : 0x%X (0x%X)\n", sections[i].ps_vaddr, (size_t) secLoc, sections[i].ps_vsize, sections[i].ps_fileoff, sections[i].ps_rawsize);

			size_t segSize = sections[i].ps_vsize + sections[i].ps_vaddr % VMMGR_PAGE_SIZE;
			vmmgr_map_pages_req(mmgr_get_used_blocks() * MMGR_BLOCK_SIZE + segSize);
			for(size_t addr = 0; addr < segSize; addr += VMMGR_PAGE_SIZE){
				if(vmmgr_is_address_accessible(sections[i].ps_vaddr + addr)){
					continue;
				}
				vmmgr_map_page((size_t) secLoc + addr, sections[i].ps_vaddr + addr);
			}
			memset((void*) ((size_t) sections[i].ps_vaddr), 0, sections[i].ps_vsize);
			memcpy((void*) ((size_t) sections[i].ps_vaddr), (void*) (sections[i].ps_fileoff + (size_t) ubi_kernel_location), sections[i].ps_rawsize);
		}
	}
	_end:
	if(status != TSX_SUCCESS){
	}
	return status;
}

status_t ubi_relocate(size_t kernelMinAddr, size_t kernelMaxAddr){
#if defined(ARCH_amd64)
	size_t addr = 0xffffffff00000000; // preferred address
#elif defined(ARCH_i386)
	size_t addr = 0; // 0 because relocation is not supported on i386 (default address)
#endif
	if((addr >= kernelMinAddr && addr <= kernelMaxAddr) || (addr + MMGR_USABLE_MEMORY >= kernelMinAddr && addr + MMGR_USABLE_MEMORY <= kernelMaxAddr) ||
		(kernelMinAddr >= addr && kernelMinAddr <= addr + MMGR_USABLE_MEMORY) || (kernelMaxAddr >= addr && kernelMaxAddr <= addr + MMGR_USABLE_MEMORY)){
		// this needs to be changed
		if(SIZE_MAX - kernelMaxAddr >= MMGR_USABLE_MEMORY){
			addr = kernelMaxAddr;
		}else if(kernelMinAddr > MMGR_USABLE_MEMORY){
			addr = kernelMinAddr - MMGR_USABLE_MEMORY;
		}else{
			log_error("No suitable virtual memory location found to relocate to\n");
			return 1;
		}
	}
	log_debug("Reloc to %Y\n", addr);
	return kernel_relocate(addr);
}


status_t ubi_create_tables(ubi_table_header* kroottable){
	status_t status = 0;

	status = ubi_create_mem_table((ubi_k_mem_table*) ubi_get_kernel_table(UBI_K_MEM_MAGIC));
	CERROR();
	status = ubi_create_vid_table((ubi_k_video_table*) ubi_get_kernel_table(UBI_K_VID_MAGIC));
	CERROR();
	status = ubi_create_module_table((ubi_k_module_table*) ubi_get_kernel_table(UBI_K_MODULES_MAGIC));
	CERROR();
	status = ubi_create_system_table();
	CERROR();
	status = ubi_create_memmap_table();
	CERROR();
	status = ubi_create_loader_table();
	CERROR();
	status = ubi_create_cmd_table();
	CERROR();
	status = ubi_create_bdrive_table();
	CERROR();
	_end:
	return status;
}


status_t ubi_create_mem_table(ubi_k_mem_table* table){
	status_t status = 0;

	ubi_b_mem_table* btable = kmalloc(sizeof(ubi_b_mem_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_mem_table));
	btable->hdr.magic = UBI_B_MEM_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	if(table){
		log_debug("Table %Y @ %Y\n", (size_t) table->hdr.magic, (size_t) table);
		if(table->heapSize > 0){
			btable->heapSize = table->heapSize;
			if(table->heapLocation == 0){
				btable->heapLocation = kmalloc(table->heapSize);
			}else{
				btable->heapLocation = table->heapLocation;
				ubi_alloc_virtual(&btable->heapLocation, table->heapSize);
			}
			if(!btable->heapLocation){
				FERROR(TSX_OUT_OF_MEMORY);
			}
		}

		if(table->stackSize == 0){
			kernel_get_stack_meta(NULL, &table->stackSize);
		}
		if(table->stackLocation == 0){
			btable->stackLocation = kmalloc(table->stackSize);
		}else{
			btable->stackLocation = table->stackLocation - table->stackSize;
			ubi_alloc_virtual(&btable->stackLocation, table->stackSize);
		}
		if(!btable->stackLocation){
			FERROR(TSX_OUT_OF_MEMORY);
		}
		btable->stackLocation += table->stackSize;
		btable->stackSize = table->stackSize;

		kernel_move_stack((size_t) btable->stackLocation, btable->stackSize);

		if(table->idMapSize > 0){
			vmmgr_map_pages(0, (size_t) table->idMapLocation, table->idMapSize - (table->idMapSize & 0xfff));
			btable->idMapLocation = table->idMapLocation;
			btable->idMapSize = table->idMapSize;
		}

		bool elfDyn = ((elf_file*) ubi_kernel_location)->e_type == ELF_ET_DYN;
		if((table->flags & UBI_FLAGS_MEMORY_KASLR) && ubi_kernel_type == 1 && elfDyn && !parse_get_boolean(configData, "disableKaslr")){
			size_t kernelSize = ubi_kernel_top - ubi_kernel_base;
			if(kernelSize > table->kaslrSize){
				log_error("Kernel size is larger than kaslrSize\n");
				FERROR(TSX_ERROR);
			}
			if(kernelSize + table->kernelBase < table->kernelBase){ // means it wrapped around and kernelSize is too large
				log_error("Kernel size is too large (kernelBase is too high)\n");
				FERROR(TSX_ERROR);
			}
			ubi_kernel_offset = ubi_get_random_kernel_offset((size_t) table->kernelBase, table->kaslrSize);
			btable->flags |= UBI_FLAGS_MEMORY_KASLR;
		}else if(elfDyn){ // no kaslr, but relocatable
			ubi_kernel_offset = (size_t) table->kernelBase;
		}
	}else{
		btable->heapLocation = 0;
		btable->heapSize = 0;
		kernel_get_stack_meta((size_t*) &btable->stackLocation, &btable->stackSize);
	}
	reloc_ptr((void**) &btable->stackLocation);
	reloc_ptr((void**) &btable->heapLocation);

	_end:
	return status;
}


static uint32_t commonVideoModes[8][2] = {
	{320, 200},
	{640, 480},
	{800, 600},
	{1024, 768},
	{1366, 768},
	{1280, 1024},
	{1600, 900},
	{1920, 1080}
};

static uint32_t commonBPPs[] = {
	15, 16, 24, 32
};

status_t ubi_create_vid_table(ubi_k_video_table* table){
	status_t status = 0;

	ubi_b_video_table* btable = kmalloc(sizeof(ubi_b_video_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_video_table));
	btable->hdr.magic = UBI_B_VID_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	if(table){
		log_debug("Table %Y @ %Y\n", (size_t) table->hdr.magic, (size_t) table);
		uint8_t kvidmode = table->flags & UBI_MASK_VIDEO_MODE;
		if(kvidmode == 1){
			status = kernel_set_video(80, 25, 16, 0);
			CERROR();
		}else if(kvidmode == 2){
			bool success = FALSE;
			if(kernel_set_video(table->width, table->height, table->bpp, 1) != TSX_SUCCESS){
				// attempt to set other BPPs
				for(int i = 3; i >= 0; i--){
					if(commonBPPs[i] == table->bpp)
						continue;
					if(kernel_set_video(table->width, table->height, commonBPPs[i], 1) == TSX_SUCCESS){
						success = TRUE;
						break;
					}
				}

				if(!success){
					// set closest video mode smaller than requested
					for(int i = 7; i >= 0; i--){
						if(table->width * table->height <= commonVideoModes[i][0] * commonVideoModes[i][1]){
							if(kernel_set_video(commonVideoModes[i][0], commonVideoModes[i][1], 32, 1) == TSX_SUCCESS){
								success = TRUE;
								break;
							}
						}
					}
				}

				if(!success){
					status = kernel_set_video(640, 480, 32, 1);
					CERROR();
					success = TRUE;
				}
			}else{
				success = TRUE;
			}
		}
		arch_sleep(200);

		if(table->flags & UBI_FLAGS_VIDEO_CLEAR_SCREEN)
			ubi_clearScreen = TRUE;
	}

	// table settings set in post init

	reloc_ptr((void**) &btable->framebufferAddress);

	_end:
	return status;
}


status_t ubi_load_module(list_array* modlist, char* path, void* loadAddress){
	status_t status = 0;
	size_t readpathlen = strlen(path) + 16;
	char* readpath = kmalloc(readpathlen);
	if(!readpath)
		FERROR(TSX_OUT_OF_MEMORY);
	snprintf(readpath, readpathlen, "%s%s", kernelPartition, path);

	log_info("Loading %s ", readpath);
	size_t size = 0;
	status = vfs_get_file_size(readpath, &size);
	CERROR();
	void* addr = loadAddress;
	if(addr){
		ubi_alloc_virtual(&addr, size);
	}else{
		addr = kmalloc_aligned(size);
	}
	if(!addr)
		FERROR(TSX_OUT_OF_MEMORY);

	status = vfs_read_file(readpath, (size_t) addr);
	CERROR();
	printf("\n");

	ubi_b_module_entry* mentry = kmalloc(sizeof(ubi_b_module_entry));
	mentry->path = path;
	mentry->loadAddress = addr;
	mentry->size = size;
	list_array_push(modlist, mentry);
	_end:
	if(readpath)
		kfree(readpath, readpathlen);
	return status;
}

status_t ubi_create_module_table(ubi_k_module_table* table){
	status_t status = 0;

	list_array* modlist = NULL;
	size_t configListLen = 0;
	char* configList = NULL;

	ubi_b_module_table* btable = kmalloc(sizeof(ubi_b_module_table));
	if(!btable)
		FERROR(TSX_OUT_OF_MEMORY);
	memset(btable, 0, sizeof(ubi_b_module_table));
	btable->hdr.magic = UBI_B_MODULES_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	modlist = list_array_create(0);
	if(!modlist){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	ubi_b_module_entry* kentry = kmalloc(sizeof(ubi_b_module_entry));
	if(!kentry)
		FERROR(TSX_OUT_OF_MEMORY);
	kentry->path = kernelPath;
	kentry->loadAddress = (void*) kernelImgLocation;
	kentry->size = kernelImgSize;
	list_array_push(modlist, kentry);

	if(table){
		log_debug("Table %Y @ %Y\n", (size_t) table->hdr.magic, (size_t) table);
		for(size_t i = 0; i < table->length; i++){
			char* akpath = NULL;
			if(table->modules[i].path == 0 && ubi_kernel_type == 1){
				akpath = ubi_get_file_addr(ubi_get_elf_reldyn_var_addr_f((size_t) (&table->modules[i].path)));
			}else{
				akpath = ubi_get_file_addr((size_t) (table->modules[i].path));
			}
			status = ubi_load_module(modlist, akpath, table->modules[i].loadAddress);
			CERROR();
		}
	}
	char* configListO = parse_get_option(configData, "modules");
	if(configListO){
		configListLen = strlen(configListO) + 1;
		configList = kmalloc(configListLen);
		if(!configList)
			FERROR(TSX_OUT_OF_MEMORY);
		memcpy(configList, configListO, configListLen);
		char* current = configList;
		while(current){
			char* next = strchr(current, ':');
			if(next){
				*next = 0;
				next++;
			}
			status = ubi_load_module(modlist, current, NULL);
			CERROR();
			current = next;
		}
		// strings in configList will be referenced by UBI table
	}

	btable->length = modlist->length;
	btable->modules = kmalloc(modlist->length * sizeof(ubi_b_module_entry));
	if(!btable->modules)
		FERROR(TSX_OUT_OF_MEMORY);
	reloc_ptr((void**) &btable->modules);
	for(size_t i = 0; i < modlist->length; i++){
		ubi_b_module_entry* entry = list_array_get(modlist, i);
		btable->modules[i].path = entry->path;
		btable->modules[i].loadAddress = entry->loadAddress;
		btable->modules[i].size = entry->size;
		reloc_ptr((void**) &btable->modules[i].path);
		reloc_ptr((void**) &btable->modules[i].loadAddress);
	}

	_end:
	if(modlist){
		for(size_t i = 0; i < modlist->length; i++){
			kfree(list_array_get(modlist, i), sizeof(ubi_b_module_entry));
		}
		list_array_delete(modlist);
	}
	if(status != TSX_SUCCESS && configList){
		kfree(configList, configListLen);
	}
	printNlnr();
	return status;
}


status_t ubi_create_system_table(){
	status_t status = 0;

	ubi_b_system_table* btable = kmalloc(sizeof(ubi_b_system_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_system_table));
	btable->hdr.magic = UBI_B_SYS_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

#ifdef ARCH_UPSTREAM_x86
	void* smbios = util_search_mem("_SM3_", 0xf0000, 0xffff, 16);
	if(smbios){
		btable->flags |= 3;
	}else{
		smbios = util_search_mem("_SM_", 0xf0000, 0xffff, 16);
		if(smbios){
			btable->flags |= 2;
		}
	}
	btable->smbiosAddress = smbios;
	log_debug("SMBIOS table at %Y\n", (size_t) smbios);

	void* acpi = util_search_mem("RSD PTR ", 0xe0000, 0x1ffff, 16);
	if(!acpi)
		acpi = util_search_mem("RSD PTR ", 0x80000, 0x1000, 16);
	btable->rsdpAddress = acpi;
	log_debug("ACPI RSDP table at %Y\n", (size_t) acpi);
#endif

	if(s1data->bootFlags & S1BOOT_DATA_BOOT_FLAGS_UEFI){
		btable->uefiSystemTable = (void*) s1data->uefiSystemTable;
		log_debug("UEFI system table at %Y\n", (size_t) btable->uefiSystemTable);
	}

	_end:
	return status;
}


status_t ubi_create_memmap_table(){
	status_t status = 0;

	ubi_b_memmap_table* btable = kmalloc(sizeof(ubi_b_memmap_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_memmap_table));
	btable->hdr.magic = UBI_B_MEMMAP_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	// initialization is done in ubi_post_init

	_end:
	return status;
}


status_t ubi_create_loader_table(){
	status_t status = 0;

	ubi_b_loader_table* btable = kmalloc(sizeof(ubi_b_loader_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_loader_table));
	btable->hdr.magic = UBI_B_LOADER_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	char* name;
	char* version;
	kernel_get_brand(&name, NULL, &version);
	char* brandname = kmalloc(MMGR_BLOCK_SIZE);
	snprintf(brandname, MMGR_BLOCK_SIZE, "%s version %s", name, version);
	btable->name = brandname;

	reloc_ptr((void**) &btable->name);

	_end:
	return status;
}


status_t ubi_create_cmd_table(){
	status_t status = 0;
	if(!kernel_args)
		goto _end;

	ubi_b_cmd_table* btable = kmalloc(sizeof(ubi_b_cmd_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_cmd_table));
	btable->hdr.magic = UBI_B_CMD_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	btable->cmd = kernel_args;

	reloc_ptr((void**) &btable->cmd);

	_end:
	return status;
}


status_t ubi_create_bdrive_table(){
	status_t status = 0;

	ubi_b_bdrive_table* btable = kmalloc(sizeof(ubi_b_bdrive_table));
	if(!btable){
		FERROR(TSX_OUT_OF_MEMORY);
	}
	memset(btable, 0, sizeof(ubi_b_bdrive_table));
	btable->hdr.magic = UBI_B_BDRIVE_MAGIC;
	reloc_ptr((void**) &btable->hdr.nextTable);
	lastTable->nextTable = (ubi_b_table_header*) btable;
	lastTable = (ubi_b_table_header*) btable;

	memcpy(&btable->type, kernel_get_boot_drive_type(), 5);
	btable->other = s1data->bootDrive;

	_end:
	return status;
}


status_t ubi_post_init(){
	status_t status = 0;

	log_debug("ubi_b_root_table=%Y\n", (size_t) ubi_root);

	((ubi_b_mem_table*) ubi_get_boot_table(UBI_B_MEM_MAGIC))->kernelBase = (void*) (ubi_kernel_base + ubi_kernel_offset);

	if(ubi_clearScreen){
		clearScreen(0x7);
		((ubi_b_video_table*) ubi_get_boot_table(UBI_B_VID_MAGIC))->flags |= UBI_FLAGS_VIDEO_CLEAR_SCREEN;
	}


	status = ubi_recreate_memmap();
	CERROR();
	log_debug("Memory map contains %u entries\n", ((ubi_b_memmap_table*) ubi_get_boot_table(UBI_B_MEMMAP_MAGIC))->length);


	ubi_b_video_table* videoTable = ((ubi_b_video_table*) ubi_get_boot_table(UBI_B_VID_MAGIC));

	uint8_t mode;
	size_t width, height, bpp, pitch, cursorPosX, cursorPosY;
	stdio64_get_mode(&mode, &width, &height, &bpp, &pitch, &videoTable->framebufferAddress);
	videoTable->width = width;
	videoTable->height = height;
	videoTable->bpp = bpp;
	videoTable->pitch = pitch;
	videoTable->flags |= mode == STDIO64_MODE_GRAPHICS ? UBI_FLAGS_VIDEO_GRAPHICS : UBI_FLAGS_VIDEO_TEXT;

	stdio64_get_cursor_pos(&cursorPosX, &cursorPosY);
	videoTable->cursorPosX = cursorPosX;
	videoTable->cursorPosY = cursorPosY;
	if(mode == STDIO64_MODE_GRAPHICS){
		videoTable->cursorPosX *= STDIO64_GRAPHICS_CHAR_WIDTH;
		videoTable->cursorPosY *= STDIO64_GRAPHICS_CHAR_HEIGHT;
	}


	ubi_b_table_header* table = (ubi_b_table_header*) ubi_root;
	while(table){
		ubi_set_checksum(table, ubi_get_table_size(table->magic));
		table = table->nextTable;
	}
	arch_disable_hw_interrupts();
	stdio64_update_screen();
	_end:
	return status;
}

static size_t ubi_last_memmap_blen = 0;

status_t ubi_recreate_memmap(){
	ubi_b_memmap_table* memmaptable = (ubi_b_memmap_table*) ubi_get_boot_table(UBI_B_MEMMAP_MAGIC);
	if(memmaptable->entries && ubi_last_memmap_blen){
		kfree(memmaptable->entries, ubi_last_memmap_blen);
	}
	size_t totallen = mmgr_gen_mmap(NULL, 0, NULL) + 1; // assume there can be another entry after we kmalloc()
	mmap_entry* buf = kmalloc(totallen * sizeof(mmap_entry));
	if(!buf)
		return TSX_OUT_OF_MEMORY;
	ubi_last_memmap_blen = totallen * sizeof(mmap_entry);
	size_t wrlen = 0;
	totallen = mmgr_gen_mmap(buf, totallen * sizeof(mmap_entry), &wrlen);
	if(wrlen < totallen)
		return TSX_ERROR;

	// rewrite type values
	for(size_t i = 0; i < totallen; i++){
		buf[i].type = ubi_convert_to_ubi_memtype(buf[i].type);
	}

	memmaptable->entries = (void*) buf;
	memmaptable->length = wrlen;
	return TSX_SUCCESS;
}

#if defined(ARCH_amd64)
#define UBI_ELF_CALLCONV __attribute__((sysv_abi))
#define UBI_PE_CALLCONV __attribute__((ms_abi))
#elif defined(ARCH_i386)
#define UBI_ELF_CALLCONV
#define UBI_PE_CALLCONV
#endif

ubi_status_t ubi_call_kernel(){
	if(ubi_kernel_type == 1){
		elf_file* file = ubi_kernel_location;
		return ((ubi_status_t (UBI_ELF_CALLCONV *) (ubi_b_root_table*))(file->e_entry + ubi_kernel_offset))(ubi_root);
	}else if(ubi_kernel_type == 2){
		pe_file* file = mz_get_pe(ubi_kernel_location);
		return ((ubi_status_t (UBI_PE_CALLCONV *) (ubi_b_root_table*))((size_t) file->po_entry))(ubi_root);
	}
	return UBI_STATUS_ERROR;
}


void* ubi_get_file_addr(size_t vaddr){
	if(vaddr == 0)
		return NULL;
	if(ubi_kernel_type == 1){
		elf_file* file = ubi_kernel_location;
		elf_ph* ph = elf_get_ph(file);
		for(int i = 0; i < file->e_phnum; i++){
			if(ph[i].p_type != ELF_PH_TYPE_LOAD)
				continue;
			if(vaddr >= ph[i].p_vaddr && vaddr <= ph[i].p_vaddr + ph[i].p_memsz){
				return (void*) (vaddr - ph[i].p_vaddr + ph[i].p_offset + (size_t) file);
			}
		}
	}else if(ubi_kernel_type == 2){
		pe_file* file = mz_get_pe(ubi_kernel_location);
		pe_section_header* sections = pe_get_sections(file);
		for(size_t i = 0; i < file->p_sections; i++){
			if(vaddr >= sections[i].ps_vaddr && vaddr <= sections[i].ps_vaddr + sections[i].ps_vsize){
				return (void*) (vaddr - sections[i].ps_vaddr + sections[i].ps_fileoff + (size_t) ubi_kernel_location);
			}
		}
	}
	return NULL;
}

size_t ubi_get_elf_reldyn_var_addr_f(size_t addr){ // gets rela addend for a variable at addr in file image
	elf_file* file = ubi_kernel_location;
	elf_ph* ph = elf_get_ph(file);

	size_t vaddr = 0;
	for(int i = 0; i < file->e_phnum; i++){
		if(ph[i].p_type != ELF_PH_TYPE_LOAD)
			continue;
		if(addr >= ph[i].p_offset + (size_t) file && addr <= ph[i].p_offset + (size_t) file + ph[i].p_filesz){
			vaddr = (addr - ph[i].p_offset - (size_t) file + ph[i].p_vaddr);
		}
	}
	if(!vaddr)
		return 0;

	return ubi_get_elf_reldyn_var_addr(vaddr);
}

size_t ubi_get_elf_reldyn_var_addr(size_t addr){ // gets rela addend for a variable at final address addr
	elf_file* file = ubi_kernel_location;
	elf_sh* reldynsec = elf_get_sh_entry(file, ".rela.dyn");
	if(!reldynsec)
		return 0;
	dynl_rela* rela = (dynl_rela*) (reldynsec->sh_offset + (size_t) file);
	for(int i = 0; i < reldynsec->sh_size / sizeof(dynl_rela); i++){
		if(rela[i].r_offset == addr){
			return rela[i].r_addend;
		}
	}
	return 0;
}


void ubi_set_checksum(ubi_b_table_header* table, size_t totalTableSize){
	uint32_t val = 0;
	for(size_t addr = (size_t) table + sizeof(ubi_b_table_header); addr < (size_t) table + totalTableSize; addr++){
		val += *((uint8_t*) (addr));
	}
	table->checksum = 0x100000000 - val;
}


void ubi_alloc_virtual(void** addr, size_t size){
	size_t vaddr = (size_t) (*addr);
	if(vaddr == 0)
		goto anyAddr;
	bool used = FALSE;
	for(size_t addr = vaddr; addr < vaddr + size; addr += VMMGR_PAGE_SIZE){
		if(vmmgr_is_address_accessible(addr)){
			used = TRUE;
			break;
		}
	}
	if(used){
		anyAddr:
		*addr = kmalloc(size);
		mmgr_reserve_mem_region(vmmgr_get_physical((size_t) (*addr)), size, MMGR_MEMTYPE_OS);
	}else{
		vmmgr_map_pages_req(mmgr_get_used_blocks() * MMGR_BLOCK_SIZE + size);
		size_t paddr = (size_t) mmgr_alloc_block_sequential(size);
		if(paddr){
			mmgr_reserve_mem_region(paddr, size, MMGR_MEMTYPE_OS);
			for(size_t addr = 0; addr < size; addr += VMMGR_PAGE_SIZE){
				vmmgr_map_page(paddr + addr, vaddr + addr);
			}
		}else{
			*addr = NULL;
		}
	}
}


ubi_table_header* ubi_get_kernel_table(uint64_t magic){
	ubi_table_header* table = (ubi_table_header*) ubi_kernel;
	while(table){
		if(table->magic == magic)
			return table;
		if(table->nextTable == 0 && ubi_kernel_type == 1){ // is ELF file and the address is written to addends in .rela (not here, because it is 0)
			table = ubi_get_file_addr(ubi_get_elf_reldyn_var_addr_f((size_t) (&table->nextTable)));
		}else{
			table = ubi_get_file_addr((size_t) (table->nextTable));
		}
	}
	return NULL;
}

ubi_b_table_header* ubi_get_boot_table(uint64_t magic){
	ubi_b_table_header* table = (ubi_b_table_header*) ubi_root;
	while(table){
		if(table->magic == magic)
			return table;
		table = table->nextTable;
	}
	return NULL;
}


size_t ubi_get_table_size(uint64_t magic){
	switch(magic){
		case UBI_K_ROOT_MAGIC: return sizeof(ubi_k_root_table);
		case UBI_K_MEM_MAGIC: return sizeof(ubi_k_mem_table);
		case UBI_K_VID_MAGIC: return sizeof(ubi_k_video_table);
		case UBI_K_MODULES_MAGIC: return sizeof(ubi_k_module_table);
		case UBI_B_ROOT_MAGIC: return sizeof(ubi_b_root_table);
		case UBI_B_MEM_MAGIC: return sizeof(ubi_b_mem_table);
		case UBI_B_VID_MAGIC: return sizeof(ubi_b_video_table);
		case UBI_B_MODULES_MAGIC: return sizeof(ubi_b_module_table);
		case UBI_B_SYS_MAGIC: return sizeof(ubi_b_system_table);
		case UBI_B_MEMMAP_MAGIC: return sizeof(ubi_b_memmap_table);
		case UBI_B_LOADER_MAGIC: return sizeof(ubi_b_loader_table);
		case UBI_B_CMD_MAGIC: return sizeof(ubi_b_cmd_table);
		case UBI_B_BDRIVE_MAGIC: return sizeof(ubi_b_bdrive_table);
		default: return sizeof(ubi_table_header);
	}
}

uint32_t ubi_convert_to_ubi_memtype(uint32_t memtype){
	switch(memtype){
		case MMGR_MEMTYPE_USABLE: return UBI_MEMTYPE_USABLE;
		case MMGR_MEMTYPE_RESERVED: return UBI_MEMTYPE_RESERVED;
		case MMGR_MEMTYPE_ACPI_RECLAIM: return UBI_MEMTYPE_ACPI_RECLAIM;
		case MMGR_MEMTYPE_ACPI_NVS: return UBI_MEMTYPE_ACPI_NVS;
		case MMGR_MEMTYPE_BAD: return UBI_MEMTYPE_BAD;
		case MMGR_MEMTYPE_UEFI_RUNTIME: return UBI_MEMTYPE_UEFI_RSRV;
		case MMGR_MEMTYPE_UEFI_BOOT: return UBI_MEMTYPE_UEFI_BSRV;
		case MMGR_MEMTYPE_BOOTLOADER:
		case MMGR_MEMTYPE_BOOTLOADER_DATA: return UBI_MEMTYPE_BOOTLOADER;
		case MMGR_MEMTYPE_PAGING: return UBI_MEMTYPE_PAGING;
		case MMGR_MEMTYPE_OS: return UBI_MEMTYPE_OS;
		default: return UBI_MEMTYPE_RESERVED;
	}
}

size_t ubi_get_random_kernel_offset(size_t kernelBase, size_t kaslrSize){
	size_t offset = arch_rand(kaslrSize - (ubi_kernel_top - ubi_kernel_base));
	offset -= offset & 0xfff; // randomization is page aligned
	return kernelBase + offset;
}


