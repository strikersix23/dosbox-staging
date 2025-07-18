// SPDX-FileCopyrightText:  2020-2025 The DOSBox Staging Team
// SPDX-FileCopyrightText:  2002-2021 The DOSBox Team
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cassert>
#include <cerrno>
#include <new>
#include <type_traits>

#include "mem_unaligned.h"
#include "paging.h"
#include "types.h"

#if defined(HAVE_MMAP)
#include <sys/mman.h>
#endif

#if defined(HAVE_PTHREAD_WRITE_PROTECT_NP)
#include <pthread.h>
#endif

#if defined(HAVE_SYS_ICACHE_INVALIDATE)
#include <libkern/OSCacheControl.h>
#endif

#if defined(WIN32)
#include <memoryapi.h>
#include <processthreadsapi.h>
#endif

class CodePageHandler;

// basic cache block representation
class CacheBlock {
public:
	CacheBlock() = default;
	~CacheBlock();

	// Prevent copy and move construction and assignment
	CacheBlock(const CacheBlock&) = delete;
	CacheBlock(CacheBlock&&) = delete;
	CacheBlock& operator=(const CacheBlock&) = delete;
	CacheBlock& operator=(CacheBlock&&) = delete;

	void Clear();

	// link this cache block to another block, index specifies the code
	// path (always zero for unconditional links, 0/1 for conditional ones
	void LinkTo(Bitu index, CacheBlock *toblock)
	{
		assert(toblock);
		link[index].to=toblock;
		link[index].next = toblock->link[index].from; // set target block
		toblock->link[index].from = this; // remember who links me
	}

	struct Page {
		uint16_t start = 0;
		uint16_t end   = 0; // where in the page is the original code

		CodePageHandler* handler = {}; // page containing this code
	} page = {};

	struct Cache {
		const uint8_t* start = {}; // where in the cache are we

		Bitu size        = 0;
		CacheBlock* next = {};
		// writemap masking maskpointer/start/length to allow holes in
		// the writemap
		uint8_t* wmapmask = {};
		uint16_t maskstart = 0;
		uint16_t masklen   = 0;

		// Manage the write mask
		void DeleteWriteMask();
		inline void AddByteToWriteMaskAt(const size_t page_index);
		inline void AddWordToWriteMaskAt(const size_t page_index);
		inline void AddDwordToWriteMaskAt(const size_t page_index);

	private:
		inline void GrowWriteMask(const uint16_t new_mask_len);
		size_t GrowMaskForTypeAt(const uint8_t type_size,
		                         const size_t page_index);
	} cache = {};

	struct Hash {
		Bitu index = 0;

		CacheBlock* next = {};
	} hash = {};

	struct Link {
		CacheBlock* to = {}; // this block can transfer control to the
		                     // to-block
		CacheBlock* next = {};
		CacheBlock* from = {}; // the from-block can transfer control
		                       // to this block
	} link[2] = {};                // maximum two links (conditional jumps)

	CacheBlock* crossblock = {};
};

static_assert(std::is_standard_layout_v<CacheBlock::Page>, "standard-layout is required for offsetof");
static_assert(std::is_standard_layout_v<CacheBlock::Cache>, "standard-layout is required for offsetof");
static_assert(std::is_standard_layout_v<CacheBlock::Hash>, "standard-layout is required for offsetof");
static_assert(std::is_standard_layout_v<CacheBlock::Link>, "standard-layout is required for offsetof");
static_assert(std::is_standard_layout_v<CacheBlock>, "standard-layout is required for offsetof");

static struct {
	struct {
		CacheBlock* first   = {}; // the first cache block in the list
		CacheBlock* active  = {}; // the current cache block
		CacheBlock* free    = {}; // pointer to the free list
		CacheBlock* running = {}; // the last block that was entered for
		                          // execution
	} block = {};

	// TODO field pos used to be a normal pointer, but upstream
	// changed it to const pointer in r4424 (perhaps by mistake or as WIP
	// change). Once transition to W^X will be done, decide if this
	// should be const pointer or not and remove this comment.
	//
  	//uint8_t *pos;              // position in the cache block
	const uint8_t* pos = {}; // position in the cache block

	CodePageHandler* free_pages = {}; // pointer to the free list
	CodePageHandler* used_pages = {}; // pointer to the list of used pages
	CodePageHandler* last_page  = {}; // the last used page
} cache = {};

// cache memory pointers, to be malloc'd later
static uint8_t* cache_code_start_ptr   = {};
static uint8_t* cache_code             = {};
static uint8_t* cache_code_link_blocks = {};

static std::vector<CacheBlock> cache_blocks(CACHE_BLOCKS);
static CacheBlock link_blocks[2] = {}; // default linking (specially marked)

// the CodePageHandler class provides access to the contained
// cache blocks and intercepts writes to the code for special treatment
class CodePageHandler final : public PageHandler {
public:
	CodePageHandler() = default;

	CodePageHandler(const CodePageHandler &) = delete; // prevent copying
	CodePageHandler &operator=(const CodePageHandler &) = delete; // prevent assignment

	void SetupAt(Bitu _phys_page,PageHandler * _old_pagehandler) {
		// initialize this codepage handler
		phys_page=_phys_page;
		// save the old pagehandler to provide direct read access to the
		// memory, and to be able to restore it later on
		old_pagehandler=_old_pagehandler;

		// adjust flags
		flags=old_pagehandler->flags|(cpu.code.big ? PFLAG_HASCODE32:PFLAG_HASCODE16);
		flags&=~PFLAG_WRITEABLE;

		active_blocks=0;
		active_count=16;

		// initialize the maps with zero (no cache blocks as well as
		// code present)
		memset(&hash_map,0,sizeof(hash_map));
		memset(&write_map,0,sizeof(write_map));
		if (invalidation_map) {
			delete [] invalidation_map;
			invalidation_map = nullptr;
		}
	}

	// clear out blocks that contain code which has been modified
	bool InvalidateRange(Bitu start, Bitu end)
	{
		Bits index=1+(end>>DYN_HASH_SHIFT);
		bool is_current_block = false; // if the current block is
		                               // modified, it has to be exited
		                               // as soon as possible

		uint32_t ip_point=SegPhys(cs)+reg_eip;
		ip_point = (PAGING_GetPhysicalPage(ip_point) -
		            check_cast<uint32_t>(phys_page << 12)) +
		           (ip_point & 0xfff);
		while (index >= 0) {
			Bitu map=0;
			// see if there is still some code in the range
			for (Bitu count=start;count<=end;count++) map+=write_map[count];
			if (!map)
				return is_current_block; // no more code, finished

			CacheBlock *block = hash_map[index];
			while (block) {
				CacheBlock *nextblock = block->hash.next;
				// test if this block is in the range
				if (start<=block->page.end && end>=block->page.start) {
					if (ip_point<=block->page.end && ip_point>=block->page.start) is_current_block=true;
					block->Clear(); // clear the block,
					                // decrements the
					                // write_map accordingly
				}
				block=nextblock;
			}
			index--;
		}
		return is_current_block;
	}

	uint8_t *alloc_invalidation_map() const
	{
		constexpr size_t map_size = 4096;
		uint8_t *map = new (std::nothrow) uint8_t[map_size];
		if (!map) {
			E_Exit("failed to allocate invalidation_map");
		}
		memset(map, 0, map_size);
		return map;
	}

	// the following functions will clean all cache blocks that are invalid
	// now due to the write

	void writeb(PhysPt addr, const uint8_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("wb:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readb(hostmem + addr) == val)
			return;
		host_writeb(hostmem + addr, val);
		// see if there's code where we are writing to
		if (!write_map[addr]) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		invalidation_map[addr]++;
		InvalidateRange(addr,addr);
	}

	void writew(PhysPt addr, const uint16_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("ww:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readw(hostmem + addr) == val)
			return;
		host_writew(hostmem + addr, val);
		// see if there's code where we are writing to
		if (!read_unaligned_uint16(&write_map[addr])) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		host_addw(&invalidation_map[addr], 0x0101);
		InvalidateRange(addr,addr+1);
	}

	void writed(PhysPt addr, const uint32_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("wd:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readd(hostmem + addr) == val)
			return;
		host_writed(hostmem + addr, val);
		// see if there's code where we are writing to
		if (!read_unaligned_uint32(&write_map[addr])) {
			if (active_blocks)
				return; // still some blocks in this page
			active_count--;
			if (!active_count)
				Release(); // delay page releasing until
				           // active_count is zero
			return;
		} else if (!invalidation_map) {
			invalidation_map = alloc_invalidation_map();
		}
		host_addd(&invalidation_map[addr], 0x01010101);
		InvalidateRange(addr,addr+3);
	}

	bool writeb_checked(PhysPt addr, const uint8_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return false;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("cb:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readb(hostmem + addr) == val)
			return false;
		// see if there's code where we are writing to
		if (!write_map[addr]) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			invalidation_map[addr]++;
			if (InvalidateRange(addr,addr)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writeb(hostmem+addr,val);
		return false;
	}

	bool writew_checked(PhysPt addr, const uint16_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return false;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("cw:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readw(hostmem + addr) == val)
			return false;
		// see if there's code where we are writing to
		if (!read_unaligned_uint16(&write_map[addr])) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			host_addw(&invalidation_map[addr], 0x0101);
			if (InvalidateRange(addr,addr+1)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writew(hostmem+addr,val);
		return false;
	}

	bool writed_checked(PhysPt addr, const uint32_t val) override
	{
		if (old_pagehandler->flags & PFLAG_HASROM) {
			return false;
		}
		if ((old_pagehandler->flags & PFLAG_READABLE) != PFLAG_READABLE) {
			E_Exit("cd:non-readable code page found that is no ROM page");
		}
		addr&=4095;
		if (host_readd(hostmem + addr) == val)
			return false;
		// see if there's code where we are writing to
		if (!read_unaligned_uint32(&write_map[addr])) {
			if (!active_blocks) {
				// no blocks left in this page, still delay
				// the page releasing a bit
				active_count--;
				if (!active_count) Release();
			}
		} else {
			if (!invalidation_map)
				invalidation_map = alloc_invalidation_map();

			host_addd(&invalidation_map[addr], 0x01010101);
			if (InvalidateRange(addr,addr+3)) {
				cpu.exception.which=SMC_CURRENT_BLOCK;
				return true;
			}
		}
		host_writed(hostmem+addr,val);
		return false;
	}

	// add a cache block to this page and note it in the hash map
	void AddCacheBlock(CacheBlock *block)
	{
		Bitu index=1+(block->page.start>>DYN_HASH_SHIFT);
		block->hash.next = hash_map[index]; // link to old block at
		                                    // index from the new block
		block->hash.index=index;
		hash_map[index] = block; // put new block at hash position
		block->page.handler=this;
		active_blocks++;
	}

	// there's a block whose code started in a different page
	void AddCrossBlock(CacheBlock *block)
	{
		block->hash.next=hash_map[0];
		block->hash.index=0;
		hash_map[0]=block;
		block->page.handler=this;
		active_blocks++;
	}

	// remove a cache block
	void DelCacheBlock(CacheBlock* block)
	{
		active_blocks--;
		active_count       = 16;
		CacheBlock** where = &hash_map[block->hash.index];
		while (*where != block) {
			where = &((*where)->hash.next);
			// Will crash if a block isn't found, which should never
			// happen.
		}
		*where = block->hash.next;

		// remove the cleared block from the write map
		if (block->cache.wmapmask) {
			// first part is not influenced by the mask
			for (Bitu i = block->page.start; i < block->cache.maskstart;
			     i++) {
				if (write_map[i]) {
					write_map[i]--;
				}
			}
			Bitu maskct = 0;
			// last part sticks to the writemap mask
			for (Bitu i = block->cache.maskstart; i <= block->page.end;
			     i++, maskct++) {
				if (write_map[i]) {
					// only adjust writemap if it isn't masked
					if ((maskct >= block->cache.masklen) ||
					    (!block->cache.wmapmask[maskct])) {
						write_map[i]--;
					}
				}
			}
			block->cache.DeleteWriteMask();
		} else {
			for (Bitu i = block->page.start; i <= block->page.end; i++) {
				if (write_map[i]) {
					write_map[i]--;
				}
			}
		}
	}

	void Release()
	{
		// revert to old handler
		MEM_SetPageHandler(phys_page,1,old_pagehandler);
		PAGING_ClearTLB();

		// remove page from the lists
		if (prev) prev->next=next;
		else cache.used_pages=next;
		if (next) next->prev=prev;
		else cache.last_page=prev;
		next=cache.free_pages;
		cache.free_pages=this;
		prev=nullptr;
	}

	void ClearRelease()
	{
		// clear out all cache blocks in this page
		Bitu count=active_blocks;
		CacheBlock **map=hash_map;
		for (CacheBlock * block=*map;count;count--) {
			while (block==nullptr)
				block=*++map;
			CacheBlock * nextblock=block->hash.next;
			block->page.handler=nullptr;			// no need, full clear
			block->Clear();
			block=nextblock;
		}
		Release(); // now can release this page
	}

	CacheBlock *FindCacheBlock(Bitu start)
	{
		CacheBlock *block = hash_map[1 + (start >> DYN_HASH_SHIFT)];
		// see if there's a cache block present at the start address
		while (block) {
			if (block->page.start == start)
				return block; // found
			block=block->hash.next;
		}
		return nullptr; // none found
	}

	HostPt GetHostReadPt(Bitu phys_page) override
	{
		hostmem = old_pagehandler->GetHostReadPt(phys_page);
		return hostmem;
	}

	HostPt GetHostWritePt(Bitu phys_page) override
	{
		return GetHostReadPt(phys_page);
	}

public:
	// the write map, there are write_map[i] cache blocks that cover
	// the byte at address i
	uint8_t write_map[4096] = {};
	uint8_t *invalidation_map = nullptr;

	CodePageHandler *prev = nullptr;
	CodePageHandler *next = nullptr;

private:
	PageHandler *old_pagehandler = nullptr;

	// hash map to quickly find the cache blocks in this page
	CacheBlock *hash_map[1 + DYN_PAGE_HASH] = {};

	Bitu active_blocks = 0; // the number of cache blocks in this page
	Bitu active_count = 0;  // delaying parameter to not immediately release
	                        // a page
	HostPt hostmem = nullptr;
	Bitu phys_page = 0;
};

static inline void cache_add_unused_block(CacheBlock *block)
{
	// block has become unused, add it to the freelist
	block->cache.next = cache.block.free;
	cache.block.free = block;
}

static CacheBlock *cache_getblock()
{
	// get a free cache block and advance the free pointer
	CacheBlock *ret = cache.block.free;
	if (!ret)
		E_Exit("Ran out of CacheBlocks");
	cache.block.free=ret->cache.next;
	ret->cache.next=nullptr;
	return ret;
}

CacheBlock::~CacheBlock() {
	cache.DeleteWriteMask();
}

void CacheBlock::Cache::DeleteWriteMask()
{
	delete[] wmapmask;
	wmapmask = {};
	masklen  = 0;
}

inline void CacheBlock::Cache::GrowWriteMask(const uint16_t new_mask_len)
{
	// This function is only called to increase the mask
	assert(new_mask_len > masklen);

	// Allocate the new mask
	auto new_mask = new uint8_t[new_mask_len];
	memset(new_mask, 0, new_mask_len);

	// Copy the current into the new
	std::copy(wmapmask, wmapmask + masklen, new_mask);

	// Update the current
	delete[] wmapmask;
	wmapmask = new_mask;

	masklen = new_mask_len;
}

// Grow the mask to accomodate the given type size at the give page index.
// Returns the offset into the write mask for incoming index.
size_t CacheBlock::Cache::GrowMaskForTypeAt(const uint8_t type_size,
                                            const size_t page_index)
{
	size_t map_offset = 0;

	// Make the map mask if needed
	if (!wmapmask) {
		constexpr uint8_t initial_mask_len = 64;
		GrowWriteMask(initial_mask_len);
		maskstart = check_cast<uint16_t>(page_index);
	}
	// Do we need a larger mask to accomodate the added type?
	else {
		map_offset = page_index - maskstart;
		const size_t map_offset_end = map_offset + type_size;
		if (map_offset_end >= masklen) {
			size_t new_mask_len = masklen * 4;
			if (new_mask_len < map_offset_end) {
				new_mask_len = ((map_offset_end) & ~3) * 2;
			}
			GrowWriteMask(check_cast<uint16_t>(new_mask_len));
		}
	}
	assert(map_offset + type_size < masklen);
	return map_offset;
}

inline void CacheBlock::Cache::AddByteToWriteMaskAt(const size_t page_index)
{
	const auto map_offset = GrowMaskForTypeAt(sizeof(uint8_t), page_index);
	wmapmask[map_offset] += 0x01;
}

inline void CacheBlock::Cache::AddWordToWriteMaskAt(const size_t page_index)
{
	const auto map_offset = GrowMaskForTypeAt(sizeof(uint16_t), page_index);
	add_to_unaligned_uint16(wmapmask + map_offset, 0x0101);
}

inline void CacheBlock::Cache::AddDwordToWriteMaskAt(const size_t page_index)
{
	const auto map_offset = GrowMaskForTypeAt(sizeof(uint32_t), page_index);
	add_to_unaligned_uint32(wmapmask + map_offset, 0x01010101);
}

void CacheBlock::Clear()
{
	Bitu ind;
	// check if this is not a cross page block
	if (hash.index) for (ind=0;ind<2;ind++) {
		CacheBlock * fromlink=link[ind].from;
		link[ind].from=nullptr;
		while (fromlink) {
			CacheBlock * nextlink=fromlink->link[ind].next;
			// clear the next-link and let the block point to the
			// standard linkcode
			fromlink->link[ind].next=nullptr;
			fromlink->link[ind].to=&link_blocks[ind];

			fromlink=nextlink;
		}
		if (link[ind].to!=&link_blocks[ind]) {
			// not linked to the standard linkcode, find the block
			// that links to this block
			CacheBlock **wherelink = &link[ind].to->link[ind].from;
			while (*wherelink != this && *wherelink) {
				wherelink = &(*wherelink)->link[ind].next;
			}
			// now remove the link
			if (*wherelink)
				*wherelink = (*wherelink)->link[ind].next;
			else
				LOG(LOG_CPU, LOG_ERROR)("Cache anomaly. please investigate");
		}
	} else {
		cache_add_unused_block(this);
	}
	if (crossblock) {
		// clear out the crossblock (in the page before) as well
		crossblock->crossblock=nullptr;
		crossblock->Clear();
		crossblock=nullptr;
	}
	if (page.handler) {
		// clear out the code page handler
		page.handler->DelCacheBlock(this);
		page.handler=nullptr;
	}
	cache.DeleteWriteMask();
}

static CacheBlock *cache_openblock()
{
	CacheBlock *block = cache.block.active;
	// check for enough space in this block
	Bitu size=block->cache.size;
	CacheBlock *nextblock = block->cache.next;
	if (block->page.handler)
		block->Clear();
	// block size must be at least CACHE_MAXSIZE
	while (size<CACHE_MAXSIZE) {
		if (!nextblock)
			goto skipresize;
		// merge blocks
		size+=nextblock->cache.size;
		CacheBlock *tempblock = nextblock->cache.next;
		if (nextblock->page.handler)
			nextblock->Clear();
		// block is free now
		cache_add_unused_block(nextblock);
		nextblock=tempblock;
	}
skipresize:
	// adjust parameters and open this block
	block->cache.size=size;
	block->cache.next=nextblock;
	cache.pos=block->cache.start;
	return block;
}

static void cache_closeblock()
{
	CacheBlock *block = cache.block.active;
	// links point to the default linking code
	block->link[0].to=&link_blocks[0];
	block->link[1].to=&link_blocks[1];
	block->link[0].from=nullptr;
	block->link[1].from=nullptr;
	block->link[0].next=nullptr;
	block->link[1].next=nullptr;
	// close the block with correct alignment
	Bitu written = (Bitu)(cache.pos - block->cache.start);
	if (written>block->cache.size) {
		if (!block->cache.next) {
			if (written > block->cache.size + CACHE_MAXSIZE)
				E_Exit("CacheBlock overrun 1 %" PRIuPTR,
				       written - block->cache.size);
		} else {
			E_Exit("CacheBlock overrun 2 written %" PRIuPTR " size %" PRIuPTR,
			       written, block->cache.size);
		}
	} else {
		Bitu new_size;
		Bitu left=block->cache.size-written;
		// smaller than cache align then don't bother to resize
		if (left>CACHE_ALIGN) {
			new_size=((written-1)|(CACHE_ALIGN-1))+1;
			CacheBlock *newblock = cache_getblock();
			// align block now to CACHE_ALIGN
			newblock->cache.start=block->cache.start+new_size;
			newblock->cache.size=block->cache.size-new_size;
			newblock->cache.next=block->cache.next;
			block->cache.next=newblock;
			block->cache.size=new_size;
		}
	}
	// advance the active block pointer
#if (C_DYNAMIC_X86)
	const bool cache_is_full = !block->cache.next;
#elif (C_DYNREC)
	const uint8_t *limit = (cache_code_start_ptr + CACHE_TOTAL - CACHE_MAXSIZE);
	const bool cache_is_full = (!block->cache.next ||
	                            (block->cache.next->cache.start > limit));
#endif
	if (cache_is_full) {
		// LOG_DEBUG("Cache full; restarting");
		cache.block.active=cache.block.first;
	} else {
		cache.block.active=block->cache.next;
	}
}

// TODO functions cache_addb, cache_addw, cache_addd, cache_addq definitely
// should NOT use const pointer pos (because they treat this point as writable
// destination), but upstream made it a const pointer in r4424 (perhaps by
// mistake or as WIP change) and relies on silently C-casting the constness
// away.
//
// Replaced silent C-casting with explicit const-casting; when upstream will
// revert this change bring back the previous version and remove this comment.
//

// place an 8bit value into the cache

static inline void cache_addb(uint8_t val, const uint8_t *pos)
{
	*const_cast<uint8_t *>(pos) = val;
}

static inline void cache_addb(uint8_t val)
{
	cache_addb(val, cache.pos);
	cache.pos += sizeof(uint8_t);
}

// place a 16bit value into the cache

static inline void cache_addw(uint16_t val, const uint8_t *pos)
{
	write_unaligned_uint16(const_cast<uint8_t *>(pos), val);
}

static inline void cache_addw(uint16_t val)
{
	cache_addw(val, cache.pos);
	cache.pos += sizeof(uint16_t);
}

// place a 32bit value into the cache

static inline void cache_addd(uint32_t val, const uint8_t *pos)
{
	write_unaligned_uint32(const_cast<uint8_t *>(pos), val);
}

static inline void cache_addd(uint32_t val)
{
	cache_addd(val, cache.pos);
	cache.pos += sizeof(uint32_t);
}

// place a 64bit value into the cache

static inline void cache_addq(uint64_t val, const uint8_t *pos)
{
	write_unaligned_uint64(const_cast<uint8_t *>(pos), val);
}

static inline void cache_addq(uint64_t val)
{
	cache_addq(val, cache.pos);
	cache.pos += sizeof(uint64_t);
}

#if (C_DYNAMIC_X86)
static void gen_return(BlockReturn retcode);
#elif (C_DYNREC)
BlockReturn generate_run_code(const uint8_t* code);
static void dyn_return(BlockReturn retcode, bool ret_exception);
static void dyn_run_code();
static void cache_block_before_close();
static void cache_block_closing(const uint8_t *block_start, Bitu block_size);
#endif

static constexpr size_t cache_code_size = CACHE_TOTAL + CACHE_MAXSIZE + HostPageSize - 1 + HostPageSize;
constexpr bool is_64bit_platform = sizeof(void *) == 8;

static inline void dyn_mem_adjust(void *&ptr, size_t &size)
{
#if (HostPageSize == 65536)
	// Use different code on 64K page systems (currently just ppc64le).
	// The other code will sometimes underrun our pointer into unmapped
	// memory and mprotect() will then fail.
	const uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
	const auto align_adjust = p % HostPageSize;
	const auto p_aligned = p - align_adjust;
	assert((p_aligned % HostPageSize) == 0);

	const auto new_size = size + align_adjust;
	const auto new_size_adjust = new_size % HostPageSize;
	if (new_size <= HostPageSize) {
		size = HostPageSize;
	} else if (new_size_adjust) {
		size = (new_size - new_size_adjust) + HostPageSize;
		assert((size % HostPageSize) == 0);
	}
#else
	// Align to page boundary and adjust size. The -1/+1 voodoo
	// is required to avoid segfaults on 32-bit builds.
	const auto p = reinterpret_cast<uintptr_t>(ptr) - 1;
	const auto align_adjust = p % HostPageSize;
	const auto p_aligned = p - align_adjust;
	size += align_adjust + 1;
#endif
	ptr = reinterpret_cast<void *>(p_aligned);
}

static inline void dyn_mem_set_access([[maybe_unused]] void *ptr,
                                      [[maybe_unused]] size_t size,
                                      [[maybe_unused]] const bool execute)
{
#if defined(HAVE_PTHREAD_WRITE_PROTECT_NP)
#if defined(HAVE_BUILTIN_AVAILABLE)
	if (__builtin_available(macOS 11.0, *))
#endif
		pthread_jit_write_protect_np(execute);

#elif defined(HAVE_MPROTECT)
	dyn_mem_adjust(ptr, size);
	const int flags = (execute ? PROT_EXEC : PROT_WRITE) | PROT_READ;
	[[maybe_unused]] const int mp_res = mprotect(ptr, size, flags);
	assert(mp_res == 0);

#elif defined(WIN32)
	if (CPU_UseRwxMemProtect)
		return; // the cache block is already marked RWX
	dyn_mem_adjust(ptr, size);
	DWORD old_protect = 0;
	const DWORD flags = (execute ? PAGE_EXECUTE_READ : PAGE_READWRITE);
	[[maybe_unused]] const auto vp_res = VirtualProtect(ptr, size, flags,
	                                                    &old_protect);
	assert(vp_res != 0);
#else
#error "no dynamic memory protection on this platform: please report this"
#endif
}

static inline void dyn_mem_execute(void *ptr, size_t size)
{
#if defined(C_PER_PAGE_W_OR_X)
	dyn_mem_set_access(ptr, size, true);
#else
	// Skip per-page execute-flagging
#endif
}

static inline void dyn_mem_write(void *ptr, size_t size)
{
#if defined(C_PER_PAGE_W_OR_X)
	dyn_mem_set_access(ptr, size, false);
#else
	// Skip per-page write-flagging
#endif
}

static inline void dyn_cache_invalidate([[maybe_unused]] void *ptr,
                                        [[maybe_unused]] size_t size)
{
#if defined(C_PER_PAGE_W_OR_X)
#	if defined(HAVE_BUILTIN_CLEAR_CACHE)
	const auto start     = static_cast<char*>(ptr);
	const auto start_val = reinterpret_cast<uintptr_t>(start);
	const auto end_val = start_val + size;
	const auto end = reinterpret_cast<char *>(end_val);
	__builtin___clear_cache(start, end);
#elif defined(HAVE_SYS_ICACHE_INVALIDATE)
#if defined(HAVE_BUILTIN_AVAILABLE)
	        if (__builtin_available(macOS 11.0, *))
#endif
		sys_icache_invalidate(ptr, size);
#elif defined(WIN32)
	        if (CPU_UseRwxMemProtect)
		return;
	FlushInstructionCache(GetCurrentProcess(), ptr, size);
#else
#error "Don't know how to clear the cache on this platform: please report this"
#endif
#else
	// Skip per-page invalidation
#endif
}

static bool cache_initialized = false;

static void cache_init(bool enable) {
	if (enable) {
		// see if cache is already initialized
		if (cache_initialized) {
			return;
		}
		cache_initialized = true;
		cache.block.free = &cache_blocks[0];
		// initialize the cache blocks
		for (int i=0;i<CACHE_BLOCKS-1;i++) {
			cache_blocks[i].link[0].to = (CacheBlock *)1;
			cache_blocks[i].link[1].to = (CacheBlock *)1;
			cache_blocks[i].cache.next = &cache_blocks[i + 1];
		}
		if (cache_code_start_ptr == nullptr) {
			// allocate the code cache memory
#if defined (WIN32)
			LPVOID lp_vmem = nullptr;
			if (CPU_UseRwxMemProtect) {
				lp_vmem = VirtualAlloc(nullptr, cache_code_size,
				                       MEM_COMMIT,
				                       PAGE_EXECUTE_READWRITE); // all operations allowed
			} else {
				lp_vmem = VirtualAlloc(nullptr, cache_code_size,
				                       MEM_COMMIT | MEM_RESERVE,
				                       PAGE_READWRITE); // needs on-going management
			}
			assert(lp_vmem);
			cache_code_start_ptr = static_cast<uint8_t *>(lp_vmem);
#elif defined(HAVE_MMAP)
			int map_flags = MAP_PRIVATE | MAP_ANON;
			int prot_flags = PROT_READ | PROT_WRITE | PROT_EXEC;
#if defined(HAVE_MAP_JIT)
			map_flags |= MAP_JIT;
#endif
			cache_code_start_ptr=static_cast<uint8_t *>(mmap(nullptr, cache_code_size, prot_flags, map_flags, -1, 0));
			if (cache_code_start_ptr == MAP_FAILED) {
				E_Exit("DYNCACHE: Failed memory-mapping cache memory because: %s", strerror(errno));
			}
#else
			cache_code_start_ptr=static_cast<uint8_t *>(malloc(cache_code_size));
			if (!cache_code_start_ptr) {
				E_Exit("DYNCACHE: Failed allocating cache memory because: %s", strerror(errno));
			}
#endif
			// align the cache at a page boundary
			cache_code = reinterpret_cast<uint8_t *>(
			    (reinterpret_cast<uintptr_t>(cache_code_start_ptr) +
			    static_cast<size_t>(HostPageSize) - 1) & ~(static_cast<size_t>(HostPageSize) - 1));

			cache_code_link_blocks=cache_code;
			cache_code=cache_code+HostPageSize;
			CacheBlock *block = cache_getblock();
			cache.block.first=block;
			cache.block.active=block;
			block->cache.start=&cache_code[0];
			block->cache.size=CACHE_TOTAL;
			block->cache.next = nullptr; // last block in the list
		}

		auto cache_addr = static_cast<void *>(cache_code);
		constexpr size_t cache_bytes = CACHE_MAXSIZE;

		dyn_mem_write(cache_addr, cache_bytes);

		auto close_link_block_num_at_code_pos = [&](const uint8_t block_num,
		                                            const uint16_t code_pos) {
			// setup the default blocks for block linkage returns
			cache.pos = &cache_code_link_blocks[code_pos];
			link_blocks[block_num].cache.start = cache.pos;
			// link code that returns with a special return code
			// must be less than 32 bytes
			dyn_return(block_num == 0 ? BR_Link1 : BR_Link2, false);
#if C_DYNREC
			cache_block_before_close();
			cache_block_closing(link_blocks[block_num].cache.start,
			                    cache.pos -
			                            link_blocks[block_num].cache.start);
#endif
		};

#if C_DYNAMIC_X86
		close_link_block_num_at_code_pos(0, 0);
		close_link_block_num_at_code_pos(1, 32);

#elif C_DYNREC
		cache.pos = &cache_code_link_blocks[0];
		using generate_run_code_f = decltype(&generate_run_code);
		core_dynrec.runcode = (generate_run_code_f)cache.pos;
		dyn_run_code(); // writes up to HostPageSize - 64 bytes

		cache_block_before_close();
		cache_block_closing(cache_code_link_blocks,
		                    cache.pos - cache_code_link_blocks);

		close_link_block_num_at_code_pos(0, HostPageSize - 64);
		close_link_block_num_at_code_pos(1, HostPageSize - 32);
#endif

		dyn_mem_execute(cache_addr, cache_bytes);
		dyn_cache_invalidate(cache_addr, cache_bytes);

		cache.free_pages=nullptr;
		cache.last_page=nullptr;
		cache.used_pages=nullptr;
		// setup the code pages
		for (int i=0;i<CACHE_PAGES;i++) {
			auto newpage = new (std::nothrow) CodePageHandler();
			if (!newpage) {
				E_Exit("DYN_CACHE: Failed to allocate code-page handler");
			}
			newpage->next = cache.free_pages;
			cache.free_pages=newpage;
		}
	}
}

static void cache_close(void) {
/*	for (;;) {
		if (cache.used_pages) {
			CodePageHandler * cpage=cache.used_pages;
			CodePageHandler * npage=cache.used_pages->next;
			cpage->ClearRelease();
			delete cpage;
			cache.used_pages=npage;
		} else break;
	}
	if (cache_blocks != NULL) {
		free(cache_blocks);
		cache_blocks = NULL;
	}
	if (cache_code_start_ptr != NULL) {
		### care: under windows VirtualFree() has to be used if
		###       VirtualAlloc was used for memory allocation
		free(cache_code_start_ptr);
		cache_code_start_ptr = NULL;
	}
	cache_code = NULL;
	cache_code_link_blocks = NULL;
	cache_initialized = false; */
}
