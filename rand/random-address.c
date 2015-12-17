#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/uio.h>
#include <sys/mman.h>	// mprotect

#include "arch.h"	// KERNEL_ADDR etc
#include "log.h"
#include "random.h"
#include "sanitise.h"
#include "maps.h"
#include "shm.h"
#include "tables.h"
#include "utils.h"

void * get_writable_address(unsigned long size)
{
	struct map *map;
	void *addr = NULL;

retry:
	map = get_map();

	if (map->size < size)
		goto retry;

	addr = map->ptr;
	mprotect(addr, map->size, PROT_READ | PROT_WRITE);
	map->prot = PROT_READ | PROT_WRITE;

	return addr;
}

void * get_non_null_address(void)
{
	//TODO: use different sizes.
	return get_writable_address(page_size);
}

void * get_address(void)
{
	if (ONE_IN(100))
		return NULL;

	return get_non_null_address();
}

static bool is_arg_address(enum argtype argtype)
{
	if (argtype == ARG_ADDRESS)
		return TRUE;
	if (argtype == ARG_NON_NULL_ADDRESS)
		return TRUE;
	return FALSE;
}

unsigned long find_previous_arg_address(struct syscallrecord *rec, unsigned int argnum)
{
	struct syscallentry *entry;
	unsigned long addr = 0;
	unsigned int call;

	call = rec->nr;
	entry = syscalls[call].entry;

	if (argnum > 1)
		if (is_arg_address(entry->arg1type) == TRUE)
			addr = rec->a1;

	if (argnum > 2)
		if (is_arg_address(entry->arg2type) == TRUE)
			addr = rec->a2;

	if (argnum > 3)
		if (is_arg_address(entry->arg3type) == TRUE)
			addr = rec->a3;

	if (argnum > 4)
		if (is_arg_address(entry->arg4type) == TRUE)
			addr = rec->a4;

	if (argnum > 5)
		if (is_arg_address(entry->arg5type) == TRUE)
			addr = rec->a5;

	return addr;
}


struct iovec * alloc_iovec(unsigned int num)
{
	struct iovec *iov;
	unsigned int i;

	iov = zmalloc(num * sizeof(struct iovec));	/* freed by generic_free_arg */

	for (i = 0; i < num; i++) {
		struct map *map;

		map = get_map();
		iov[i].iov_base = map->ptr;
		iov[i].iov_len = rand() % map->size;
	}

	return iov;
}
