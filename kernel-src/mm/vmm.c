#include <kernel/vmm.h>
#include <logging.h>
#include <kernel/pmm.h>
#include <util.h>
#include <arch/cpu.h>
#include <limine.h>
#include <string.h>
#include <kernel/slab.h>
#include <kernel/vmmcache.h>

#define RANGE_TOP(x) (void *)((uintptr_t)x->start + x->size)

static vmmcache_t *newcache() {
	vmmcache_t *ptr = pmm_allocpage(PMM_SECTION_DEFAULT);
	if (ptr == NULL)
		return NULL;
	ptr = MAKE_HHDM(ptr);
	ptr->header.freecount = VMM_RANGES_PER_CACHE; 
	ptr->header.firstfree = 0;
	ptr->header.next = NULL;
	MUTEX_INIT(&ptr->header.lock);

	for (uintmax_t i = 0; i < VMM_RANGES_PER_CACHE; ++i) {
		ptr->ranges[i].size = 0;
		ptr->ranges[i].next = NULL;
	}

	return ptr;
}

// entries are allocated directly from the pmm into these caches
static vmmcache_t *cachelist;

// only supposed to be called on locked, not fully used caches
static uintmax_t getentrynumber(vmmcache_t *cache) {
	for (uintmax_t i = cache->header.firstfree; i < VMM_RANGES_PER_CACHE; ++i) {
		if (cache->ranges[i].size == 0)
			return i;
	}
	__assert(!"get entry number called from full cache");
}

static vmmrange_t *allocrange() {
	vmmcache_t *cache = cachelist;
	vmmrange_t *range = NULL;
	while (cache) {
		MUTEX_ACQUIRE(&cache->header.lock, false);
		if (cache->header.freecount > 0) {
			--cache->header.freecount;
			uintmax_t r = getentrynumber(cache);
			cache->header.firstfree = r + 1;
			range = &cache->ranges[r];
			range->size = -1; // set as allocated temporarily
			MUTEX_RELEASE(&cache->header.lock);
			break;
		} else if (cache->header.next == NULL)
			cache->header.next = newcache();

		vmmcache_t *next = cache->header.next;
		MUTEX_RELEASE(&cache->header.lock);
		cache = next;
	}

	return range;
}

static void freerange(vmmrange_t *range) {
	vmmcache_t *cache = (vmmcache_t *)ROUND_DOWN((uintptr_t)range, PAGE_SIZE);
	MUTEX_ACQUIRE(&cache->header.lock, false);

	int rangeoffset = ((uintptr_t)range - (uintptr_t)cache->ranges) / sizeof(vmmrange_t);
	range->size = 0;
	++cache->header.freecount;
	if (cache->header.firstfree > rangeoffset)
		cache->header.firstfree = rangeoffset;

	MUTEX_RELEASE(&cache->header.lock);
}

// ranges are separated into kernel and user. the kernel has a temporary user context
vmmcontext_t vmm_kernelctx;
static vmmspace_t kernelspace = {
	.start = KERNELSPACE_START,
	.end = KERNELSPACE_END
};

// returns a pointer to the space of vaddr
static vmmspace_t *getspace(void *vaddr) {
	if (USERSPACE_START <= vaddr && vaddr < USERSPACE_END)
		return &_cpu()->vmmctx->space;
	else if (KERNELSPACE_START <= vaddr && vaddr < KERNELSPACE_END)
		return &kernelspace;
	else
		return NULL;
}


// get a range from an address
static vmmrange_t *getrange(vmmspace_t *space, void *addr) {
	vmmrange_t *range = space->ranges;
	while (range) {
		if (addr >= range->start && addr < RANGE_TOP(range))
			break;
		range = range->next;
	}
	return range;
}

// get start of range that fits specific size from specific offset
static void *getfreerange(vmmspace_t *space, void *addr, size_t size) {
	vmmrange_t *range = space->ranges;
	if (addr == NULL)
		addr = space->start;

	// if theres no ranges
	if (range == NULL)
		return addr;

	// if theres free space before the first range
	if (range->start != space->start && addr < range->start && (uintptr_t)range->start - (uintptr_t)addr >= size)
		return addr;

	while (range->next) {
		void *rangetop = RANGE_TOP(range);
		if (addr < rangetop)
			addr = rangetop;

		if (addr < range->next->start) {
			size_t freesize = (uintptr_t)range->next->start - (uintptr_t)addr;
			if (freesize >= size)
				return addr;
		}
		range = range->next;
	}

	// if theres free space after the last range
	void *rangetop = RANGE_TOP(range);
	if (addr < rangetop)
		addr = rangetop;

	if (addr != space->end && (uintptr_t)space->end - (uintptr_t)addr >= size)
		return addr;

	return NULL;
}

static void insertrange(vmmspace_t *space, vmmrange_t *newrange) {
	vmmrange_t *range = space->ranges;

	// space has no other ranges
	if (range == NULL) {
		space->ranges = newrange;
		newrange->next = NULL;
		newrange->prev = NULL;
		return;
	}

	void *newrangetop = RANGE_TOP(newrange);

	// range is before the first space range
	if (newrangetop <= range->start) {
		space->ranges = newrange;
		newrange->next = range;
		range->prev = newrange;
		newrange->prev = NULL;
		goto fragcheck;
	}

	while (range->next) {
		if (newrange->start >= RANGE_TOP(range) && newrange->start < range->next->start) { // space inbetween two other ranges
			newrange->next = range->next;
			newrange->prev = range;
			range->next = newrange;
			goto fragcheck;
		}
		range = range->next;
	}

	// space after the second range
	range->next = newrange;
	newrange->prev = range;
	newrange->next = NULL;

	fragcheck:
	// join new range and the next
	if (newrange->next && newrange->next->start == newrangetop && newrange->flags == newrange->next->flags && newrange->mmuflags == newrange->next->mmuflags
		&& ((newrange->flags & VMM_FLAGS_FILE) == 0 || (newrange->vnode == newrange->next->vnode && newrange->offset + newrange->size == newrange->next->offset))) {
		vmmrange_t *oldrange = newrange->next;
		newrange->size += oldrange->size;
		newrange->next = oldrange->next;
		if (oldrange->next)
			oldrange->next->prev = newrange;

		freerange(oldrange);
		if (newrange->flags & VMM_FLAGS_FILE) {
			VOP_RELEASE(newrange->vnode);
		}
	}

	// join new range and the previous
	if (newrange->prev && RANGE_TOP(newrange->prev) == newrange->start && newrange->flags == newrange->prev->flags && newrange->mmuflags == newrange->prev->mmuflags
		&& ((newrange->flags & VMM_FLAGS_FILE) == 0 || (newrange->vnode == newrange->prev->vnode && newrange->prev->offset + newrange->prev->size == newrange->offset))) {
		vmmrange_t *oldrange = newrange->prev;
		oldrange->size += newrange->size;
		oldrange->next = newrange->next;

		if (newrange->next)
			newrange->next->prev = oldrange;

		freerange(newrange);
		if (oldrange->flags & VMM_FLAGS_FILE) {
			VOP_RELEASE(oldrange->vnode);
		}
	}
}

static void destroyrange(vmmrange_t *range, uintmax_t _offset, size_t size, int flags) {
	uintmax_t top = _offset + size;

	for (uintmax_t offset = _offset; offset < top; offset += PAGE_SIZE) {
		void *vaddr = (void *)((uintptr_t)range->start + offset);
		void *physical = arch_mmu_getphysical(_cpu()->vmmctx->pagetable, vaddr);
		if (physical == NULL)
			continue;

		thread_t *thread = _cpu()->thread;
		proc_t *proc = thread ? thread->proc : NULL;
		cred_t *cred = proc ? &proc->cred : NULL;

		if ((range->flags & VMM_FLAGS_FILE) && ((range->flags & VMM_FLAGS_SHARED) || range->vnode->type == V_TYPE_CHDEV)) {
			// shared file mapping or character device mapping
			if (range->vnode->type == V_TYPE_CHDEV) {
				// character device mapping
				__assert(VOP_MUNMAP(range->vnode, vaddr, range->offset + offset, mmuflagstovnodeflags(range->mmuflags) | (range->flags & VMM_FLAGS_SHARED ? V_FFLAGS_SHARED : 0), cred) == 0);
			} else if (arch_mmu_iswritable(_cpu()->vmmctx->pagetable, vaddr) && arch_mmu_isdirty(_cpu()->vmmctx->pagetable, vaddr)) {
				// dirty page cache mapping
				arch_mmu_unmap(_cpu()->vmmctx->pagetable, vaddr);
				vmmcache_makedirty(pmm_getpage(physical));
				pmm_release(physical);
			} else {
				// non dirty page mapping
				arch_mmu_unmap(_cpu()->vmmctx->pagetable, vaddr);
				pmm_release(physical);
			}
		} else {
			// anonymous, physical or private non character device mapping
			arch_mmu_unmap(_cpu()->vmmctx->pagetable, vaddr);
			if ((range->flags & VMM_FLAGS_PHYSICAL) == 0)
				pmm_release(physical);
		}
	}

	if ((range->flags & VMM_FLAGS_FILE) && range->size == size)
		VOP_RELEASE(range->vnode);
}

static void unmap(vmmspace_t *space, void *address, size_t size) {
	void *top = (void *)((uintptr_t)address + size);
	vmmrange_t *range = space->ranges;
	while (range) {
		__assert(range != space->ranges || range->prev == NULL);
		void *rangetop = RANGE_TOP(range);
		// completely unmapped
		if (range->start >= address && rangetop <= top) {
			if (range->prev)
				range->prev->next = range->next;
			else
				space->ranges = range->next;

			if (range->next)
				range->next->prev = range->prev;

			destroyrange(range, 0, range->size, 0);
			freerange(range);
		} else if (address > range->start && top < rangetop) { // split
			vmmrange_t *new = allocrange();
			__assert(new); // XXX deal with this better
			*new = *range;

			destroyrange(range, (uintptr_t)address - (uintptr_t)range->start, size, 0);

			new->start = top;
			new->size = (uintptr_t)rangetop - (uintptr_t)new->start;
			range->size = (uintptr_t)address - (uintptr_t)range->start;

			if (range->next)
				range->next->prev = new;

			// new->next is set by the copy in *new = *range
			new->prev = range;
			range->next = new;

			if (range->flags & VMM_FLAGS_FILE) {
				VOP_HOLD(range->vnode);
				new->offset += range->size + size;

			}
		} else if (top > range->start && range->start >= address) { // partially unmap from start
			size_t difference = (uintptr_t)top - (uintptr_t)range->start;
			destroyrange(range, 0, difference, 0);
			range->start = (void *)((uintptr_t)range->start + difference);
			range->size -= difference;

			if (range->flags & VMM_FLAGS_FILE)
				range->offset += difference;
		} else if (address < rangetop && rangetop <= top){ // partially unmap from end
			size_t difference = (uintptr_t)rangetop - (uintptr_t)address;
			range->size -= difference;
			destroyrange(range, range->size, difference, 0);
		}

		range = range->next;
	}

}

static void printspace(vmmspace_t *space) {
	printf("vmm: ranges:\n");
	vmmrange_t *range = space->ranges;
	while (range) {
		printf("vmm: address %p size %lx flags %x\n", range->start, range->size, range->flags);
		range = range->next;
	}
}

static void *zeropage;

bool vmm_pagefault(void *addr, bool user, int actions) {
	if (user == false && addr > USERSPACE_END)
		return false;

	addr = (void *)ROUND_DOWN((uintptr_t)addr, PAGE_SIZE);

	vmmspace_t *space = getspace(addr);

	if (space == NULL || (space == &kernelspace && user))
		return false;

	MUTEX_ACQUIRE(&space->pflock, false);
	MUTEX_ACQUIRE(&space->lock, false);
	vmmrange_t *range = getrange(space, addr);

	bool status = false;

	if (range == NULL)
		goto cleanup;

	// check if valid

	int invalidactions = 0;

	if ((range->mmuflags & ARCH_MMU_FLAGS_READ) == 0)
		invalidactions |= VMM_ACTION_READ;
	
	if ((range->mmuflags & ARCH_MMU_FLAGS_WRITE) == 0)
		invalidactions |= VMM_ACTION_WRITE;
	
	if ((range->mmuflags & ARCH_MMU_FLAGS_NOEXEC))
		invalidactions |= VMM_ACTION_EXEC;

	if (invalidactions & actions)
		goto cleanup;

	thread_t *thread = _cpu()->thread;
	proc_t *proc = thread ? thread->proc : NULL;
	cred_t *cred = proc ? &proc->cred : NULL;

	if (arch_mmu_ispresent(_cpu()->vmmctx->pagetable, addr) == false) {
		// page not present in the page tables
		if (range->flags & VMM_FLAGS_FILE) {
			uintmax_t mapoffset = (uintptr_t)addr - (uintptr_t)range->start;
			if (range->vnode->type != V_TYPE_BLKDEV && range->vnode->type != V_TYPE_REGULAR) {
				// map non cacheable vnodes
				__assert(VOP_MMAP(range->vnode, addr, range->offset + mapoffset, mmuflagstovnodeflags(range->mmuflags) | (range->flags & VMM_FLAGS_SHARED ? V_FFLAGS_SHARED : 0), cred) == 0);
				status = true;
			} else {
				// cacheable vnode
				page_t *res = NULL;
				int error = vmmcache_getpage(range->vnode, range->offset + mapoffset, &res);
				if (error == ENXIO) {
					// address is past the last page of the file
					signal_signalthread(_cpu()->thread, SIGBUS, true);
					status = true;
				} else if (error) {
					printf("vmm: error on vmmcache_getpage(): %d\n", error);
					status = false;
				} else {
					status = arch_mmu_map(_cpu()->vmmctx->pagetable, pmm_getpageaddress(res), addr, range->mmuflags & ~ARCH_MMU_FLAGS_WRITE);
					if (!status) {
						printf("vmm: out of memory to map file into address space\n");
						pmm_release(pmm_getpageaddress(res));
					}
				}
			}
		} else {
			// anonymous memory. map the zero'd page
			status = arch_mmu_map(_cpu()->vmmctx->pagetable, zeropage, addr, range->mmuflags & ~ARCH_MMU_FLAGS_WRITE);
			if (!status) {
				printf("vmm: out of memory to map zero page into address space\n");
			} else {
				pmm_hold(zeropage);
			}
		}
	} else if (arch_mmu_iswritable(_cpu()->vmmctx->pagetable, addr) == false) {
		// page present but not writeable in the page tables
		void *oldphys = arch_mmu_getphysical(_cpu()->vmmctx->pagetable, addr);
		if ((range->flags & VMM_FLAGS_FILE) && (range->flags & VMM_FLAGS_SHARED)) {
			arch_mmu_remap(_cpu()->vmmctx->pagetable, oldphys, addr, range->mmuflags);
			if (range->vnode->type != V_TYPE_CHDEV)
				vmmcache_makedirty(pmm_getpage(oldphys));

			status = true;
		} else {
			// do copy on write
			void *newphys = pmm_allocpage(PMM_SECTION_DEFAULT);
			if (newphys == NULL) {
				printf("vmm: out of memory to do copy on write on address space\n");
				status = false;
			} else {
				memcpy(MAKE_HHDM(newphys), MAKE_HHDM(oldphys), PAGE_SIZE);
				arch_mmu_remap(_cpu()->vmmctx->pagetable, newphys, addr, range->mmuflags);
				if ((range->flags & VMM_FLAGS_FILE) == 0 || range->vnode->type != V_TYPE_CHDEV)
					pmm_release(oldphys);

				status = true;
			}
		}
	} else {
		// another thread already did the work, so just return success
		status = true;
	}

	cleanup:
	MUTEX_RELEASE(&space->lock);
	MUTEX_RELEASE(&space->pflock);
	return status;
}

void *vmm_getphysical(void *addr) {
	addr = (void *)ROUND_DOWN((uintptr_t)addr, PAGE_SIZE);

	vmmspace_t *space = getspace(addr);
	if (space == NULL)
		return NULL;

	MUTEX_ACQUIRE(&space->lock, false);

	void *physical = arch_mmu_getphysical(_cpu()->vmmctx->pagetable, addr);

	MUTEX_RELEASE(&space->lock);
	return physical + ((uintptr_t)addr - ROUND_DOWN((uintptr_t)addr, PAGE_SIZE));
}


void *vmm_map(void *addr, volatile size_t size, int flags, mmuflags_t mmuflags, void *private) {
	if (addr == NULL)
		addr = KERNELSPACE_START;

	// XXX maps where addr is not page aligned can break
	addr = (void *)ROUND_DOWN((uintptr_t)addr, PAGE_SIZE);
	if (flags & VMM_FLAGS_PAGESIZE)
		size *= PAGE_SIZE;
	else
		size = ROUND_UP(size, PAGE_SIZE);

	if (size == 0)
		return NULL;

	vmmspace_t *space = getspace(addr);
	if (space == NULL)
		return NULL;

	MUTEX_ACQUIRE(&space->lock, false);
	vmmrange_t *range = NULL;

	void *start = getfreerange(space, addr, size);
	void *retaddr = NULL;
	if (((flags & VMM_FLAGS_EXACT) && start != addr) || start == NULL)
		goto cleanup;

	range = allocrange();
	if (range == NULL)
		goto cleanup;

	if (flags & VMM_FLAGS_REPLACE) {
		__assert(addr);
		retaddr = addr;
		range->start = addr;
		range->size = size;
		range->flags = VMM_PERMANENT_FLAGS_MASK & flags;
		range->mmuflags = mmuflags;
		__assert((flags & (VMM_FLAGS_ALLOCATE | VMM_FLAGS_PHYSICAL)) == 0);
		unmap(space, addr, size);
	} else {
		retaddr = start;
		range->start = start;
		range->size = size;
		range->flags = VMM_PERMANENT_FLAGS_MASK & flags;
		range->mmuflags = mmuflags;
	}

	if (flags & VMM_FLAGS_FILE) {
		// XXX make sure that writes can't happen to executable memory mapped files and check file permissions
		vmmfiledesc_t *desc = private;
		__assert((desc->offset % PAGE_SIZE) == 0);
		range->vnode = desc->node;
		range->offset = desc->offset;
		VOP_HOLD(desc->node);
	}

	if (flags & VMM_FLAGS_PHYSICAL) {
		// map to allocated virtual memory
		for (uintmax_t i = 0; i < size; i += PAGE_SIZE) {
			if (arch_mmu_map(_cpu()->vmmctx->pagetable, (void *)((uintptr_t)private + i), (void *)((uintptr_t)start + i), mmuflags) == false) {
				for (uintmax_t j = 0; j < size; j += PAGE_SIZE)
					arch_mmu_unmap(_cpu()->vmmctx->pagetable, (void *)((uintptr_t)start + j));

				retaddr = NULL;
				goto cleanup;
			}
		}
	} else if (flags & VMM_FLAGS_ALLOCATE) {
		// allocate to virtual memory
		for (uintmax_t i = 0; i < size; i += PAGE_SIZE) {
			void *allocated = pmm_allocpage(PMM_SECTION_DEFAULT);
			if (allocated == NULL) {
				retaddr = NULL;
				goto cleanup;
			}

			if (arch_mmu_map(_cpu()->vmmctx->pagetable, allocated, (void *)((uintptr_t)start + i), mmuflags) == false) {
				for (uintmax_t j = 0; j < size; j += PAGE_SIZE) {
						void *virt = (void *)((uintptr_t)start + i);
						void *physical = arch_mmu_getphysical(_cpu()->vmmctx->pagetable, virt);

						if (physical) {
							pmm_release(physical);
							arch_mmu_unmap(_cpu()->vmmctx->pagetable, virt);
						}
				}

				retaddr = NULL;
				goto cleanup;
			}
			memset(MAKE_HHDM(allocated), 0, PAGE_SIZE);
		}
	}

	insertrange(space, range);
	cleanup:
	if (start == NULL && range)
		freerange(range);

	MUTEX_RELEASE(&space->lock);
	return retaddr;
}

void vmm_unmap(void *addr, size_t size, int flags) {
	// XXX maps where addr is not page aligned can break
	addr = (void *)ROUND_DOWN((uintptr_t)addr, PAGE_SIZE);

	if (flags & VMM_FLAGS_PAGESIZE)
		size *= PAGE_SIZE;
	else
		size = ROUND_UP(size, PAGE_SIZE);

	if (size == 0)
		return;

	vmmspace_t *space = getspace(addr);
	if (space == NULL)
		return;

	// the pagefault handler lock is acquired
	// in order to prevent some consistency issues
	// when other threads are running on the same address space.
	// the lock ordering is still pflock -> lock on both
	// vmm_pagefault and vmm_unmap.
	MUTEX_ACQUIRE(&space->pflock, false);
	MUTEX_ACQUIRE(&space->lock, false);
	unmap(space, addr, size);
	MUTEX_RELEASE(&space->lock);
	MUTEX_RELEASE(&space->pflock);
}

static scache_t *ctxcache;

static void ctxctor(scache_t *cache, void *obj) {
	vmmcontext_t *ctx = obj;
	ctx->space.start = USERSPACE_START;
	ctx->space.end = USERSPACE_END;
	MUTEX_INIT(&ctx->space.lock);
	MUTEX_INIT(&ctx->space.pflock);
	ctx->space.ranges = NULL;
}

vmmcontext_t *vmm_newcontext() {
	if (ctxcache == NULL) {
		ctxcache = slab_newcache(sizeof(vmmcontext_t), 0, ctxctor, ctxctor);
		__assert(ctxcache);
	}

	vmmcontext_t *ctx = slab_allocate(ctxcache);
	if (ctx == NULL)
		return NULL;

	ctx->pagetable = arch_mmu_newtable();
	if (ctx->pagetable == NULL) {
		slab_free(ctxcache, ctx);
		return NULL;
	}

	return ctx;
}

void vmm_destroycontext(vmmcontext_t *context) {
	vmmcontext_t *oldctx = _cpu()->thread->vmmctx;
	vmm_switchcontext(context);
	vmm_unmap(context->space.start, context->space.end - context->space.start, 0);
	vmm_switchcontext(oldctx);
	arch_mmu_destroytable(context->pagetable);
	slab_free(ctxcache, context);
}

vmmcontext_t *vmm_fork(vmmcontext_t *oldcontext) {
	vmmcontext_t *newcontext = vmm_newcontext();
	if (newcontext == NULL)
		return NULL;

	MUTEX_ACQUIRE(&oldcontext->space.lock, false);

	vmmrange_t *range = oldcontext->space.ranges;

	while (range) {
		vmmrange_t *newrange = allocrange();
		if (newrange == NULL)
			goto error;

		memcpy(newrange, range, sizeof(vmmrange_t));

		insertrange(&newcontext->space, newrange);
		if (range->flags & VMM_FLAGS_FILE)
			VOP_HOLD(range->vnode);

		// copy any pages that are mapped

		for (uintptr_t offset = 0; offset < newrange->size; offset += PAGE_SIZE) {
			// XXX some types of mappings, like framebuffer shared mappings, will break if done this way
			void *vaddr = (void *)((uintptr_t)newrange->start + offset);
			void *phys = arch_mmu_getphysical(oldcontext->pagetable, vaddr);
			if (phys == NULL)
				continue;

			if (arch_mmu_map(newcontext->pagetable, phys, vaddr, newrange->mmuflags & ~ARCH_MMU_FLAGS_WRITE) == false)
				goto error;
			pmm_hold(phys);
			arch_mmu_remap(oldcontext->pagetable, phys, vaddr, newrange->mmuflags & ~ARCH_MMU_FLAGS_WRITE);
		}

		range = range->next;
	}

	MUTEX_RELEASE(&oldcontext->space.lock);
	return newcontext;
	error:
	MUTEX_RELEASE(&oldcontext->space.lock);
	vmm_destroycontext(newcontext);
	return NULL;
}

void vmm_switchcontext(vmmcontext_t *ctx) {
	if (_cpu()->thread)
		_cpu()->thread->vmmctx = ctx;
	_cpu()->vmmctx = ctx;
	arch_mmu_switch(ctx->pagetable);
}

extern void *_text_start;
extern void *_data_start;
extern void *_rodata_start;
extern void *_text_end;
extern void *_data_end;
extern void *_rodata_end;

extern volatile struct limine_memmap_request pmm_liminemap;

void vmm_init() {
	// set up initial state
	__assert(sizeof(vmmcache_t) <= PAGE_SIZE);
	MUTEX_INIT(&kernelspace.lock);
	MUTEX_INIT(&kernelspace.pflock);

	cachelist = newcache();
	vmm_kernelctx.pagetable = arch_mmu_newtable();
	__assert(cachelist && vmm_kernelctx.pagetable);

	vmm_kernelctx.space.start = USERSPACE_START;
	vmm_kernelctx.space.end = USERSPACE_END;

	vmm_switchcontext(&vmm_kernelctx);

	// map HHDM
	for (uint64_t i = 0; i < pmm_liminemap.response->entry_count; ++i) {
		struct limine_memmap_entry *e = pmm_liminemap.response->entries[i];

		if (e->type != LIMINE_MEMMAP_USABLE && e->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE && e->type != LIMINE_MEMMAP_KERNEL_AND_MODULES && e->type != LIMINE_MEMMAP_FRAMEBUFFER)
			continue;

		mmuflags_t mmuflags = ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_NOEXEC;
		__assert(vmm_map(MAKE_HHDM(e->base), e->length, VMM_FLAGS_EXACT, mmuflags, NULL));
	}

	// map kernel
	__assert(vmm_map(&_text_start, (uintptr_t)&_text_end - (uintptr_t)&_text_start, VMM_FLAGS_EXACT, ARCH_MMU_FLAGS_READ, NULL));
	__assert(vmm_map(&_rodata_start, (uintptr_t)&_rodata_end - (uintptr_t)&_rodata_start, VMM_FLAGS_EXACT, ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_NOEXEC, NULL));
	__assert(vmm_map(&_data_start, (uintptr_t)&_data_end - (uintptr_t)&_data_start, VMM_FLAGS_EXACT, ARCH_MMU_FLAGS_READ | ARCH_MMU_FLAGS_WRITE | ARCH_MMU_FLAGS_NOEXEC, NULL));

	// null page
	vmm_map(MAKE_HHDM(NULL), PAGE_SIZE, VMM_FLAGS_EXACT, ARCH_MMU_FLAGS_NOEXEC, NULL);

	// zero page
	zeropage = pmm_allocpage(PMM_SECTION_DEFAULT);
	memset(MAKE_HHDM(zeropage), 0, PAGE_SIZE);

	printspace(&kernelspace);
}

void vmm_apinit() {
	vmm_switchcontext(&vmm_kernelctx);
}
