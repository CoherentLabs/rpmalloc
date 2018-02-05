/* rpmalloc.h  -  Memory allocator  -  Public Domain  -  2016 Mattias Jansson / Rampant Pixels
* Copyright 2017 Stoyan Nikolov, Coherent Labs
*
* This library provides a cross-platform lock free thread caching malloc implementation in C11.
* The latest source code is always available at
*
* https://github.com/rampantpixels/rpmalloc
* https://github.com/CoherentLabs/rpmalloc
*
* This library is put in the public domain; you can redistribute it and/or modify it without any restrictions.
*
*/

#include "rpmalloc.h"

#ifndef ATOMIC_HEADER_INCLUDED
#include <atomic>
#endif

#include <stdint.h>
#include <string.h>

// Platform and arch specifics

#if defined(_WIN32)
#define RPMALLOC_PLATFORM_WIN 1
#elif defined(__linux__)
#define RPMALLOC_PLATFORM_LINUX 1
#if defined(__ANDROID__)
#define RPMALLOC_PLATFORM_ANDROID 1
#endif
#elif defined(__APPLE_CC__) || defined(__APPLE_CPP__)
#include "TargetConditionals.h"
#if (defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE) || \
			(defined(TARGET_IPHONE_SIMULATOR) && TARGET_IPHONE_SIMULATOR)
#define RPMALLOC_PLATFORM_IOS 1
#elif defined(TARGET_OS_MAC) && TARGET_OS_MAC
#define RPMALLOC_PLATFORM_MACOSX 1
#else
#error "Could not determine platform"
#endif
#elif defined(__ORBIS__)
#define RPMALLOC_PLATFORM_ORBIS 1
#endif

#if defined(RPMALLOC_PLATFORM_ANDROID) || defined(RPMALLOC_PLATFORM_IOS)
#define RPMALLOC_USE_PTHREADS_OVER_TLS 1
#endif

#ifdef RPMALLOC_USE_PTHREADS_OVER_TLS
#define THREAD_LOCAL static_assert(false, "Thread Local Storage is unsupported on this platform! Use Pthreads.");
#else
#define RPMALLOC_THREAD_LOCAL thread_local
#endif

#if defined(_MSC_VER)
#define RPMALLOC_ALIGN(ARG) __declspec(align(ARG))
#else
#define RPMALLOC_ALIGN(ARG) alignas(ARG)
#endif

#if defined(_MSC_VER)
#define RPMALLOC_FORCEINLINE __forceinline
#else
#define RPMALLOC_FORCEINLINE inline
#endif

#ifdef RPMALLOC_USE_PTHREADS_OVER_TLS
#include <pthread.h>
#define RPMALLOC_DEFINE_THREAD_LOCAL(TYPE, NAME) \
		pthread_key_t NAME; \
		static pthread_once_t NAME##OnceControl = PTHREAD_ONCE_INIT; \
		void Initialize##NAME##TLS() { pthread_key_create(&NAME, nullptr); }
#define RPMALLOC_INITIALIZE_THREAD_LOCAL(NAME) pthread_once(&NAME##OnceControl, &Initialize##NAME##TLS);
#define RPMALLOC_FREE_THREAD_LOCAL(NAME) pthread_key_delete(NAME);
#define RPMALLOC_GET_THREAD_LOCAL(TYPE, NAME) ((TYPE)pthread_getspecific(NAME))
#define RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(TYPE, NAME) ((TYPE)((size_t)pthread_getspecific(NAME)))
#define RPMALLOC_SET_THREAD_LOCAL(NAME, VALUE) pthread_setspecific(NAME, (void*)VALUE);
#else
#define RPMALLOC_DEFINE_THREAD_LOCAL(TYPE, NAME) RPMALLOC_THREAD_LOCAL TYPE NAME
#define RPMALLOC_INITIALIZE_THREAD_LOCAL(NAME)
#define RPMALLOC_FREE_THREAD_LOCAL(NAME)
#define RPMALLOC_GET_THREAD_LOCAL(TYPE, NAME) NAME
#define RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(TYPE, NAME) NAME
#define RPMALLOC_SET_THREAD_LOCAL(NAME, VALUE) NAME = VALUE
#endif

#if defined( __x86_64__ ) || defined( _M_AMD64 ) || defined( _M_X64 ) || defined( _AMD64_ ) || defined( __arm64__ ) || defined( __aarch64__ )
#  define ARCH_64BIT 1
#else
#  define ARCH_64BIT 0
#endif

#if ENABLE_ASSERTS
#  include <assert.h>
#else
#  define assert(x)
#endif

// Build time configurable limits

// Presets, if none is defined it will default to performance priority
//#define ENABLE_UNLIMITED_CACHE
//#define DISABLE_CACHE
#define ENABLE_SPACE_PRIORITY_CACHE

// Presets for cache limits
#if defined(ENABLE_UNLIMITED_CACHE)
// Unlimited caches
#define MIN_SPAN_CACHE_RELEASE 16
#define MAX_SPAN_CACHE_DIVISOR 1
#elif defined(DISABLE_CACHE)
//Disable cache
#define MIN_SPAN_CACHE_RELEASE 1
#define MAX_SPAN_CACHE_DIVISOR 0
#elif defined(ENABLE_SPACE_PRIORITY_CACHE)
// Space priority cache limits
#define MIN_SPAN_CACHE_SIZE 8
#define MIN_SPAN_CACHE_RELEASE 8
#define MAX_SPAN_CACHE_DIVISOR 16
#define GLOBAL_SPAN_CACHE_MULTIPLIER 1
#else
// Default - performance priority cache limits
//! Limit of thread cache in number of spans for each page count class (undefine for unlimited cache - i.e never release spans to global cache unless thread finishes)
//! Minimum cache size to remain after a release to global cache
#define MIN_SPAN_CACHE_SIZE 8
//! Minimum number of spans to transfer between thread and global cache
#define MIN_SPAN_CACHE_RELEASE 16
//! Maximum cache size divisor (max cache size will be max allocation count divided by this divisor)
#define MAX_SPAN_CACHE_DIVISOR 8
//! Multiplier for global span cache limit (max cache size will be calculated like thread cache and multiplied with this)
#define GLOBAL_SPAN_CACHE_MULTIPLIER 4
#endif

//! Size of heap hashmap
#define HEAP_ARRAY_SIZE           79

#ifndef ENABLE_VALIDATE_ARGS
//! Enable validation of args to public entry points
#define ENABLE_VALIDATE_ARGS      0
#endif

#if ENABLE_VALIDATE_ARGS && defined(RPMALLOC_PLATFORM_WIN)
#include <Intsafe.h>
#endif

#ifndef ENABLE_STATISTICS
//! Enable statistics collection
#define ENABLE_STATISTICS         0
#endif

#ifndef ENABLE_ASSERTS
//! Enable asserts
#define ENABLE_ASSERTS            0
#endif

//! Memory page size
#define RPMALLOC_PAGE_SIZE                 512

//! Granularity of all memory page spans for small & medium block allocations
#define SPAN_ADDRESS_GRANULARITY  8192
//! Maximum size of a span of memory pages
#define SPAN_MAX_SIZE             (SPAN_ADDRESS_GRANULARITY)
//! Mask for getting the start of a span of memory pages
#define SPAN_MASK                 (~((uintptr_t)SPAN_MAX_SIZE - 1))
//! Maximum number of memory pages in a span
#define SPAN_MAX_PAGE_COUNT       (SPAN_MAX_SIZE / RPMALLOC_PAGE_SIZE)

#define QUICK_ALLOCATION_PAGES_COUNT SPAN_MAX_PAGE_COUNT
#define SPANS_PER_SEGMENT 32

//! Granularity of a small allocation block
#define SMALL_GRANULARITY         16
//! Small granularity shift count
#define SMALL_GRANULARITY_SHIFT   4
//! Number of small block size classes
#define SMALL_CLASS_COUNT         (((RPMALLOC_PAGE_SIZE - SPAN_HEADER_SIZE) >> 1) >> SMALL_GRANULARITY_SHIFT)
//! Maximum size of a small block
#define SMALL_SIZE_LIMIT          (SMALL_CLASS_COUNT * SMALL_GRANULARITY)

//! Granularity of a medium allocation block
#define MEDIUM_GRANULARITY        32
//! Medimum granularity shift count
#define MEDIUM_GRANULARITY_SHIFT  5
//! Number of medium block size classes
#define MEDIUM_CLASS_COUNT        60
//! Maximum size of a medium block
#define MEDIUM_SIZE_LIMIT         (SMALL_SIZE_LIMIT + (MEDIUM_GRANULARITY * MEDIUM_CLASS_COUNT) - SPAN_HEADER_SIZE)

//! Total number of small + medium size classes
#define SIZE_CLASS_COUNT          (SMALL_CLASS_COUNT + MEDIUM_CLASS_COUNT)

//! Number of large block size classes
#define LARGE_CLASS_COUNT         4
//! Maximum number of memory pages in a large block
#define LARGE_MAX_PAGES           (SPAN_MAX_PAGE_COUNT * LARGE_CLASS_COUNT)
//! Maximum size of a large block
#define LARGE_SIZE_LIMIT          ((LARGE_MAX_PAGES * RPMALLOC_PAGE_SIZE) - SPAN_HEADER_SIZE)

#define SPAN_LIST_LOCK_TOKEN      ((void*)1)

#define SINGLE_SEGMENT_MARKER 1

#define pointer_offset(ptr, ofs) (void*)((char*)(ptr) + (ptrdiff_t)(ofs))
#define pointer_diff(first, second) (ptrdiff_t)((const char*)(first) - (const char*)(second))

//! Size of a span header
#define SPAN_HEADER_SIZE          48 // Not exactly right, but keeps memory 16-aligned

#if ARCH_64BIT
typedef int64_t offset_t;
#else
typedef int32_t offset_t;
#endif
typedef uint32_t count_t;

#if ENABLE_VALIDATE_ARGS
//! Maximum allocation size to avoid integer overflow
#define MAX_ALLOC_SIZE            (((size_t)-1) - RPMALLOC_PAGE_SIZE)
#endif

namespace coherent_rpmalloc
{

static void
thread_yield(void);

void _acquire_segments_lock_write();
void _release_segments_lock_write();

// Data types

//! A memory heap, per thread
typedef struct heap_t heap_t;
//! Span of memory pages
typedef struct span_t span_t;
//! Size class definition
typedef struct size_class_t size_class_t;
//! Span block bookkeeping 
typedef struct span_block_t span_block_t;
//! Span data union, usage depending on span state
typedef union span_data_t span_data_t;
//! Cache data
typedef struct span_counter_t span_counter_t;
//! Segment - holding spans
typedef struct segment_t segment_t;

struct span_block_t {
	//! Free list
	uint16_t    free_list;
	//! First autolinked block
	uint16_t    first_autolink;
	//! Free count
	uint16_t    free_count;
	//! Padding
	uint16_t    padding;
};

union span_data_t {
	//! Span data
	span_block_t block;
	//! List size (used when span is part of a list)
	uint32_t list_size;
};

struct span_t {
	//!	Heap ID
	std::atomic<int32_t>  heap_id;
	//! Size class
	count_t     size_class;
	//! Span data
	span_data_t data;
	//! Next span
	span_t*     next_span;
	//! Previous span
	span_t*     prev_span;
	//! The segment that owns this span
	//  or SINGLE_SEGMENT_MARKER if the span is not part of a segment
	segment_t* owner_segment;
};
static_assert(sizeof(span_t) <= SPAN_HEADER_SIZE, "span size mismatch");

struct span_counter_t {
	//! Allocation high water mark
	uint32_t  max_allocations;
	//! Current number of allocations
	uint32_t  current_allocations;
	//! Cache limit
	uint32_t  cache_limit;
};

struct heap_t {
	//! Heap ID
	int32_t      id;
	//! Deferred deallocation
	std::atomic<void*>  defer_deallocate;
	//! Free count for each size class active span
	span_block_t active_block[SIZE_CLASS_COUNT];
	//! Active span for each size class
	span_t*      active_span[SIZE_CLASS_COUNT];
	//! List of demi-used spans with free blocks for each size class (double linked list)
	span_t*      size_cache[SIZE_CLASS_COUNT];
	//! List of free spans for each page count (single linked list)
	span_t*      span_cache;
	//! Allocation counters
	span_counter_t span_counter;
	//! List of free spans for each large class count (single linked list)
	span_t*      large_cache[LARGE_CLASS_COUNT];
	//! Allocation counters for large blocks
	span_counter_t large_counter[LARGE_CLASS_COUNT];
#if ENABLE_STATISTICS
	//! Number of bytes currently reqeusted in allocations
	size_t       requested;
	//! Number of bytes current allocated
	size_t       allocated;
	//! Number of bytes transitioned thread -> global
	size_t       thread_to_global;
	//! Number of bytes transitioned global -> thread
	size_t       global_to_thread;
#endif
};

struct size_class_t {
	//! Size of blocks in this class
	uint16_t size;
	//! Number of pages to allocate for a chunk
	uint16_t page_count;
	//! Number of blocks in each chunk
	uint16_t block_count;
	//! Class index this class is merged with
	uint16_t class_idx;
};
static_assert(sizeof(size_class_t) == 8, "Size class size mismatch");

struct segment_t {
	std::atomic<void*> next_segment;
	std::atomic<uint32_t> free_markers;
	span_t* first_span;
};

//! Global size classes
static size_class_t _memory_size_class[SIZE_CLASS_COUNT];

//! Heap ID counter
static std::atomic<int32_t> _memory_heap_id;

//! Heaps array lock
static std::atomic_flag _heaps_lock = ATOMIC_FLAG_INIT;

//! Free segments
static std::atomic<void*> _segments_head;

//! Segment RW lock
static std::atomic<int32_t> _segments_rw_lock;

//! Global span cache
static std::atomic<void*> _memory_span_cache;

//! Global large cache
static std::atomic<void*> _memory_large_cache[LARGE_CLASS_COUNT];

//! Current thread heap
RPMALLOC_DEFINE_THREAD_LOCAL(heap_t*, _memory_thread_heap);

//! Preferred heap of this thread
RPMALLOC_DEFINE_THREAD_LOCAL(uint32_t, _memory_preferred_heap);

//! All heaps
static std::atomic<void*> _memory_heaps[HEAP_ARRAY_SIZE];

//! Adaptive cache max allocation count
static uint32_t _memory_max_allocation;

//! Adaptive cache max allocation count
static uint32_t _memory_max_allocation_large[LARGE_CLASS_COUNT];

#if ENABLE_STATISTICS
//! Total number of mapped memory pages
static std::atomic<int32_t> _mapped_pages;
//! Running counter of total number of mapped memory pages since start
static std::atomic<int32_t> _mapped_total;
//! Running counter of total number of unmapped memory pages since start
static std::atomic<int32_t> _unmapped_total;
#endif

static span_t*
_memory_map(size_t page_count);

static void
_memory_unmap(span_t* ptr);

static void*
_memory_allocate_external(size_t bytes);

static void
_memory_deallocate_external(void* ptr);

static int
_memory_deallocate_deferred(heap_t* heap, size_t size_class);

heap_t* _get_heap_ptr(void* heap) {
	return (heap_t*)((size_t)heap & ~1);
}

//! Lookup a memory heap from heap ID
static heap_t*
_memory_heap_lookup(int32_t id) {
	return _get_heap_ptr(_memory_heaps[id].load());
}

//! Increase an allocation counter
static void
_memory_counter_increase(span_counter_t* counter, uint32_t* global_counter) {
	if (++counter->current_allocations > counter->max_allocations) {
		counter->max_allocations = counter->current_allocations;
#if MAX_SPAN_CACHE_DIVISOR > 0
		counter->cache_limit = counter->max_allocations / MAX_SPAN_CACHE_DIVISOR;
#endif
		if (counter->max_allocations > *global_counter)
			*global_counter = counter->max_allocations;
	}
}

//! Insert the given list of memory page spans in the global cache for small/medium blocks
static void
_memory_global_cache_insert(span_t* first_span, size_t list_size) {
	assert((list_size == 1) || (first_span->next_span != 0));
#if MAX_SPAN_CACHE_DIVISOR > 0
	while (1) {
		void* global_span_ptr = _memory_span_cache.load();
		if (global_span_ptr != SPAN_LIST_LOCK_TOKEN) {
			uintptr_t global_list_size = (uintptr_t)global_span_ptr & ~SPAN_MASK;
			span_t* global_span = (span_t*)((void*)((uintptr_t)global_span_ptr & SPAN_MASK));

#ifdef GLOBAL_SPAN_CACHE_MULTIPLIER
			size_t cache_limit = GLOBAL_SPAN_CACHE_MULTIPLIER * (_memory_max_allocation / MAX_SPAN_CACHE_DIVISOR);
			if ((global_list_size >= cache_limit) && (global_list_size > MIN_SPAN_CACHE_SIZE))
				break;
#endif
			//We only have 16 bits for size of list, avoid overflow
			if ((global_list_size + list_size) > 0xFFFF)
				break;

			//Use prev_span as skip pointer over this sublist range of spans
			first_span->data.list_size = (uint32_t)list_size;
			first_span->prev_span = global_span;

			//Insert sublist into global cache
			global_list_size += list_size;
			void* first_span_ptr = (void*)((uintptr_t)first_span | global_list_size);
			if (std::atomic_compare_exchange_strong(&_memory_span_cache, &global_span_ptr, first_span_ptr))
				return;
		}
		else {
			//Atomic operation failed, yield timeslice and retry
			thread_yield();
			std::atomic_thread_fence(std::memory_order_acquire);
		}
	}
#endif
	//Global cache full, release pages
	for (size_t ispan = 0; ispan < list_size; ++ispan) {
		assert(first_span);
		span_t* next_span = first_span->next_span;
		_memory_unmap(first_span);
		first_span = next_span;
	}
}

//! Extract a number of memory page spans from the global cache for small/medium blocks
static span_t*
_memory_global_cache_extract() {
	span_t* span = 0;
	std::atomic<void*>* cache = &_memory_span_cache;
	std::atomic_thread_fence(std::memory_order_acquire);
	void* global_span_ptr = cache->load();
	while (global_span_ptr) {
		if ((global_span_ptr != SPAN_LIST_LOCK_TOKEN) &&
			std::atomic_compare_exchange_strong(cache, &global_span_ptr, SPAN_LIST_LOCK_TOKEN)) {
			//Grab a number of thread cache spans, using the skip span pointer
			//stored in prev_span to quickly skip ahead in the list to get the new head
			uintptr_t global_span_count = (uintptr_t)global_span_ptr & ~SPAN_MASK;
			span = (span_t*)((void*)((uintptr_t)global_span_ptr & SPAN_MASK));
			assert((span->data.list_size == 1) || (span->next_span != 0));

			span_t* new_global_span = span->prev_span;
			global_span_count -= span->data.list_size;

			//Set new head of global cache list
			void* new_cache_head = global_span_count ?
				((void*)((uintptr_t)new_global_span | global_span_count)) :
				0;
			*cache = new_cache_head;
			std::atomic_thread_fence(std::memory_order_release);
			break;
		}

		//List busy, yield timeslice and retry
		thread_yield();
		std::atomic_thread_fence(std::memory_order_acquire);
		global_span_ptr = cache->load();
	}

	return span;
}

/*! Insert the given list of memory page spans in the global cache for large blocks,
similar to _memory_global_cache_insert */
static void
_memory_global_cache_large_insert(span_t* span_list, size_t list_size, size_t span_count) {
	assert((list_size == 1) || (span_list->next_span != 0));
	assert(span_list->size_class == (SIZE_CLASS_COUNT + (span_count - 1)));
#if MAX_SPAN_CACHE_DIVISOR > 0
	std::atomic<void*>* cache = &_memory_large_cache[span_count - 1];
	while (1) {
		void* global_span_ptr = cache->load();
		if (global_span_ptr != SPAN_LIST_LOCK_TOKEN) {
			uintptr_t global_list_size = (uintptr_t)global_span_ptr & ~SPAN_MASK;
			span_t* global_span = (span_t*)((void*)((uintptr_t)global_span_ptr & SPAN_MASK));

#ifdef GLOBAL_SPAN_CACHE_MULTIPLIER
			size_t cache_limit = GLOBAL_SPAN_CACHE_MULTIPLIER * (_memory_max_allocation_large[span_count - 1] / MAX_SPAN_CACHE_DIVISOR);
			if ((global_list_size >= cache_limit) && (global_list_size > MIN_SPAN_CACHE_SIZE))
				break;
#endif
			if ((global_list_size + list_size) > 0xFFFF)
				break;

			span_list->data.list_size = (uint32_t)list_size;
			span_list->prev_span = global_span;

			global_list_size += list_size;
			void* new_global_span_ptr = (void*)((uintptr_t)span_list | global_list_size);
			if (std::atomic_compare_exchange_strong(cache, &global_span_ptr, new_global_span_ptr))
				return;
		}
		else {
			thread_yield();
			std::atomic_thread_fence(std::memory_order_acquire);
		}
	}
#endif
	//Global cache full, release spans
	for (size_t ispan = 0; ispan < list_size; ++ispan) {
		assert(span_list);
		span_t* next_span = span_list->next_span;
		_memory_unmap(span_list);
		span_list = next_span;
	}
}

/*! Extract a number of memory page spans from the global cache for large blocks,
similar to _memory_global_cache_extract */
static span_t*
_memory_global_cache_large_extract(size_t span_count) {
	span_t* span = 0;
	std::atomic<void*>* cache = &_memory_large_cache[span_count - 1];
	std::atomic_thread_fence(std::memory_order_acquire);
	void* global_span_ptr = cache->load();
	while (global_span_ptr) {
		if ((global_span_ptr != SPAN_LIST_LOCK_TOKEN) &&
			std::atomic_compare_exchange_strong(cache, &global_span_ptr, SPAN_LIST_LOCK_TOKEN)) {
			uintptr_t global_list_size = (uintptr_t)global_span_ptr & ~SPAN_MASK;
			span = (span_t*)((void*)((uintptr_t)global_span_ptr & SPAN_MASK));
			assert((span->data.list_size == 1) || (span->next_span != 0));
			assert(span->size_class == (SIZE_CLASS_COUNT + (span_count - 1)));

			span_t* new_global_span = span->prev_span;
			global_list_size -= span->data.list_size;

			void* new_global_span_ptr = global_list_size ?
				((void*)((uintptr_t)new_global_span | global_list_size)) :
				0;
			*cache = new_global_span_ptr;
			std::atomic_thread_fence(std::memory_order_release);
			break;
		}

		thread_yield();
		std::atomic_thread_fence(std::memory_order_acquire);
		global_span_ptr = cache->load();
	}
	return span;
}

//! Allocate a small/medium sized memory block from the given heap
static void*
_memory_allocate_from_heap(heap_t* heap, size_t size) {
#if ENABLE_STATISTICS
	//For statistics we need to store the requested size in the memory block
	size += sizeof(size_t);
#endif

	//Calculate the size class index and do a dependent lookup of the final class index (in case of merged classes)
	const size_t class_idx = _memory_size_class[(size <= SMALL_SIZE_LIMIT) ?
		((size + (SMALL_GRANULARITY - 1)) >> SMALL_GRANULARITY_SHIFT) - 1 :
		SMALL_CLASS_COUNT + ((size - SMALL_SIZE_LIMIT + (MEDIUM_GRANULARITY - 1)) >> MEDIUM_GRANULARITY_SHIFT) - 1].class_idx;

	span_block_t* active_block = heap->active_block + class_idx;
	size_class_t* size_class = _memory_size_class + class_idx;
	const count_t class_size = size_class->size;

#if ENABLE_STATISTICS
	heap->allocated += class_size;
	heap->requested += size;
#endif

	//Step 1: Try to get a block from the currently active span. The span block bookkeeping
	//        data for the active span is stored in the heap for faster access
use_active:
	if (active_block->free_count) {
		//Happy path, we have a span with at least one free block
		span_t* span = heap->active_span[class_idx];
		count_t offset = class_size * active_block->free_list;
		uint32_t* block = static_cast<uint32_t*>(pointer_offset(span, SPAN_HEADER_SIZE + offset));
		assert(span);

		--active_block->free_count;
		if (!active_block->free_count) {
			//Span is now completely allocated, set the bookkeeping data in the
			//span itself and reset the active span pointer in the heap
			span->data.block.free_count = 0;
			span->data.block.first_autolink = (uint16_t)size_class->block_count;
			heap->active_span[class_idx] = 0;
		}
		else {
			//Get the next free block, either from linked list or from auto link
			if (active_block->free_list < active_block->first_autolink) {
				active_block->free_list = (uint16_t)(*block);
			}
			else {
				++active_block->free_list;
				++active_block->first_autolink;
			}
			assert(active_block->free_list < size_class->block_count);
		}

#if ENABLE_STATISTICS
		//Store the requested size for statistics
		*(size_t*)pointer_offset(block, class_size - sizeof(size_t)) = size;
#endif

		return block;
	}

	//Step 2: No active span, try executing deferred deallocations and try again if there
	//        was at least one of the reqeusted size class
	if (_memory_deallocate_deferred(heap, class_idx)) {
		if (active_block->free_count)
			goto use_active;
	}

	//Step 3: Check if there is a semi-used span of the requested size class available
	if (heap->size_cache[class_idx]) {
		//Promote a pending semi-used span to be active, storing bookkeeping data in
		//the heap structure for faster access
		span_t* span = heap->size_cache[class_idx];
		*active_block = span->data.block;
		assert(active_block->free_count > 0);
		span_t* next_span = span->next_span;
		heap->size_cache[class_idx] = next_span;
		heap->active_span[class_idx] = span;
		goto use_active;
	}

	//Step 4: No semi-used span available, try grab a span from the thread cache
	span_t* span = heap->span_cache;
	if (!span) {
		//Step 5: No span available in the thread cache, try grab a list of spans from the global cache
		span = _memory_global_cache_extract();
#if ENABLE_STATISTICS
		if (span)
			heap->global_to_thread += (size_t)span->data.list_size * size_class->page_count * RPMALLOC_PAGE_SIZE;
#endif
	}
	if (span) {
		if (span->data.list_size > 1) {
			//We got a list of spans, we will use first as active and store remainder in thread cache
			span_t* next_span = span->next_span;
			assert(next_span);
			next_span->data.list_size = span->data.list_size - 1;
			heap->span_cache = next_span;
		}
		else {
			heap->span_cache = 0;
		}
	}
	else {
		//Step 6: All caches empty, map in new memory pages
		span = _memory_map(size_class->page_count);
	}

	//Mark span as owned by this heap and set base data
	span->heap_id = heap->id;
	std::atomic_thread_fence(std::memory_order_release);

	span->size_class = (count_t)class_idx;

	//If we only have one block we will grab it, otherwise
	//set span as new span to use for next allocation
	if (size_class->block_count > 1) {
		//Reset block order to sequential auto linked order
		active_block->free_count = (uint16_t)(size_class->block_count - 1);
		active_block->free_list = 1;
		active_block->first_autolink = 1;
		heap->active_span[class_idx] = span;
	}
	else {
		span->data.block.free_count = 0;
		span->data.block.first_autolink = (uint16_t)size_class->block_count;
	}

	//Track counters
	_memory_counter_increase(&heap->span_counter, &_memory_max_allocation);

#if ENABLE_STATISTICS
	//Store the requested size for statistics
	*(size_t*)pointer_offset(span, SPAN_HEADER_SIZE + class_size - sizeof(size_t)) = size;
#endif

	//Return first block if memory page span
	return pointer_offset(span, SPAN_HEADER_SIZE);
}

//! Allocate a large sized memory block from the given heap
static void*
_memory_allocate_large_from_heap(heap_t* heap, size_t size) {
	//Calculate number of needed max sized spans (including header)
	size += SPAN_HEADER_SIZE;
	size_t num_spans = size / SPAN_MAX_SIZE;
	if (size % SPAN_MAX_SIZE)
		++num_spans;
	size_t idx = num_spans - 1;

	if (!idx) {
		span_t* span = heap->span_cache;
		if (!span) {
			_memory_deallocate_deferred(heap, 0);
			span = heap->span_cache;
		}
		if (!span) {
			//Step 5: No span available in the thread cache, try grab a list of spans from the global cache
			span = _memory_global_cache_extract();
#if ENABLE_STATISTICS
			if (span)
				heap->global_to_thread += (size_t)span->data.list_size * SPAN_MAX_PAGE_COUNT * RPMALLOC_PAGE_SIZE;
#endif
		}
		if (span) {
			if (span->data.list_size > 1) {
				//We got a list of spans, we will use first as active and store remainder in thread cache
				span_t* next_span = span->next_span;
				assert(next_span);
				next_span->data.list_size = span->data.list_size - 1;
				heap->span_cache = next_span;
			}
			else {
				heap->span_cache = 0;
			}
		}
		else {
			//Step 6: All caches empty, map in new memory pages
			span = _memory_map(SPAN_MAX_PAGE_COUNT);
		}

		//Mark span as owned by this heap and set base data
		span->heap_id = heap->id;
		std::atomic_thread_fence(std::memory_order_release);

		span->size_class = SIZE_CLASS_COUNT;

		//Track counters
		_memory_counter_increase(&heap->span_counter, &_memory_max_allocation);

		return pointer_offset(span, SPAN_HEADER_SIZE);
	}

use_cache:
	//Step 1: Check if cache for this large size class (or the following, unless first class) has a span
	while (!heap->large_cache[idx] && (idx < (LARGE_CLASS_COUNT - 1)) && (idx < (num_spans + 1)))
		++idx;
	span_t* span = heap->large_cache[idx];
	if (span) {
		//Happy path, use from cache
		if (span->data.list_size > 1) {
			span_t* new_head = span->next_span;
			assert(new_head);
			new_head->data.list_size = span->data.list_size - 1;
			heap->large_cache[idx] = new_head;
		}
		else {
			heap->large_cache[idx] = 0;
		}

		span->size_class = SIZE_CLASS_COUNT + (count_t)idx;
		
		// Mark span as owned by this heap
		span->heap_id = heap->id;
		std::atomic_thread_fence(std::memory_order_release);

		//Increase counter
		_memory_counter_increase(&heap->large_counter[idx], &_memory_max_allocation_large[idx]);

		return pointer_offset(span, SPAN_HEADER_SIZE);
	}

	//Restore index, we're back to smallest fitting span count
	idx = num_spans - 1;

	//Step 2: Process deferred deallocation
	if (_memory_deallocate_deferred(heap, SIZE_CLASS_COUNT + idx))
		goto use_cache;
	assert(!heap->large_cache[idx]);

	//Step 3: Extract a list of spans from global cache
	span = _memory_global_cache_large_extract(num_spans);
	if (span) {
#if ENABLE_STATISTICS
		heap->global_to_thread += (size_t)span->data.list_size * num_spans * SPAN_MAX_SIZE;
#endif
		//We got a list from global cache, store remainder in thread cache
		if (span->data.list_size > 1) {
			span_t* new_head = span->next_span;
			assert(new_head);
			new_head->prev_span = 0;
			new_head->data.list_size = span->data.list_size - 1;
			heap->large_cache[idx] = new_head;
		}
	}
	else {
		//Step 4: Map in more memory pages
		span = _memory_map(num_spans * SPAN_MAX_PAGE_COUNT);
	}
	//Mark span as owned by this heap
	span->heap_id = heap->id;
	std::atomic_thread_fence(std::memory_order_release);

	span->size_class = SIZE_CLASS_COUNT + (count_t)idx;

	//Increase counter
	_memory_counter_increase(&heap->large_counter[idx], &_memory_max_allocation_large[idx]);

	return pointer_offset(span, SPAN_HEADER_SIZE);
}

//! Allocate a new heap
static heap_t*
_memory_allocate_heap(void) {
	heap_t* heap;
	//Try getting an orphaned heap
	std::atomic_thread_fence(std::memory_order_acquire);

	//Map in pages for a new heap
	heap = static_cast<heap_t*>(_memory_allocate_external(sizeof(heap_t)));
	memset(heap, 0, sizeof(heap_t));

	//Get a new heap ID
	do {
		heap->id = (++_memory_heap_id);
		if (_memory_heap_lookup(heap->id))
			heap->id = 0;
	} while (!heap->id);
	return heap;
}

//! Add a span to a double linked list
static void
_memory_list_add(span_t** head, span_t* span) {
	if (*head) {
		(*head)->prev_span = span;
		span->next_span = *head;
	}
	else {
		span->next_span = 0;
	}
	*head = span;
}

//! Remove a span from a double linked list
static void
_memory_list_remove(span_t** head, span_t* span) {
	if (*head == span) {
		*head = span->next_span;
	}
	else {
		if (span->next_span)
			span->next_span->prev_span = span->prev_span;
		span->prev_span->next_span = span->next_span;
	}
}

//! Insert span into thread cache, releasing to global cache if overflow
static void
_memory_heap_cache_insert(heap_t* heap, span_t* span) {
#if MAX_SPAN_CACHE_DIVISOR == 0
	(void)sizeof(heap);
	_memory_global_cache_insert(span, 1);
#else
	span_t** cache = &heap->span_cache;
	span->next_span = *cache;
	if (*cache)
		span->data.list_size = (*cache)->data.list_size + 1;
	else
		span->data.list_size = 1;
	*cache = span;
#if MAX_SPAN_CACHE_DIVISOR > 1
	//Check if cache exceeds limit
	if ((span->data.list_size >= (MIN_SPAN_CACHE_RELEASE + MIN_SPAN_CACHE_SIZE)) &&
		(span->data.list_size > heap->span_counter.cache_limit)) {
		//Release to global cache
		count_t list_size = 1;
		span_t* next = span->next_span;
		span_t* last = span;
		while (list_size < MIN_SPAN_CACHE_RELEASE) {
			last = next;
			next = next->next_span;
			++list_size;
		}
		next->data.list_size = span->data.list_size - list_size;
		last->next_span = 0; //Terminate list
		*cache = next;
		_memory_global_cache_insert(span, list_size);
#if ENABLE_STATISTICS
		heap->thread_to_global += list_size * QUICK_ALLOCATION_PAGES_COUNT * RPMALLOC_PAGE_SIZE;
#endif
	}
#endif
#endif
}

//! Deallocate the given small/medium memory block from the given heap
static void
_memory_deallocate_to_heap(heap_t* heap, span_t* span, void* p) {
	//Check if span is the currently active span in order to operate
	//on the correct bookkeeping data
	const count_t class_idx = span->size_class;
	size_class_t* size_class = _memory_size_class + class_idx;
	int is_active = (heap->active_span[class_idx] == span);
	span_block_t* block_data = is_active ?
		heap->active_block + class_idx :
		&span->data.block;

#if ENABLE_STATISTICS
	heap->allocated -= size_class->size;
	heap->requested -= *(size_t*)pointer_offset(p, size_class->size - sizeof(size_t));
#endif

	//Check if the span will become completely free
	if (block_data->free_count == ((count_t)size_class->block_count - 1)) {
		//Track counters
		assert(heap->span_counter.current_allocations > 0);
		--heap->span_counter.current_allocations;

		//If it was active, reset counter. Otherwise, if not active, remove from
		//partial free list if we had a previous free block (guard for classes with only 1 block)
		if (is_active)
			block_data->free_count = 0;
		else if (block_data->free_count > 0)
			_memory_list_remove(&heap->size_cache[class_idx], span);

		//Add to span cache
		_memory_heap_cache_insert(heap, span);
		return;
	}

	//Check if first free block for this span (previously fully allocated)
	if (block_data->free_count == 0) {
		//add to free list and disable autolink
		_memory_list_add(&heap->size_cache[class_idx], span);
		block_data->first_autolink = (uint16_t)size_class->block_count;
	}
	++block_data->free_count;
	//Span is not yet completely free, so add block to the linked list of free blocks
	uint32_t* block = static_cast<uint32_t*>(p);
	*block = block_data->free_list;
	count_t block_offset = (count_t)pointer_diff(block, span) - SPAN_HEADER_SIZE;
	count_t block_idx = block_offset / (count_t)size_class->size;
	block_data->free_list = (uint16_t)block_idx;
}

//! Deallocate the given large memory block from the given heap
static void
_memory_deallocate_large_to_heap(heap_t* heap, span_t* span) {
	//Check if aliased with 64KiB small/medium spans
	if (span->size_class == SIZE_CLASS_COUNT) {
		//Track counters
		--heap->span_counter.current_allocations;
		//Add to span cache
		_memory_heap_cache_insert(heap, span);
		return;
	}

	//Decrease counter
	size_t idx = span->size_class - SIZE_CLASS_COUNT;
	span_counter_t* counter = heap->large_counter + idx;
	assert(counter->current_allocations > 0);
	--counter->current_allocations;

#if MAX_SPAN_CACHE_DIVISOR == 0
	_memory_global_cache_large_insert(span, 1, idx + 1);
#else
	//Insert into cache list
	span_t** cache = heap->large_cache + idx;
	span->next_span = *cache;
	if (*cache)
		span->data.list_size = (*cache)->data.list_size + 1;
	else
		span->data.list_size = 1;
	*cache = span;
#if MAX_SPAN_CACHE_DIVISOR > 1
	//Check if cache exceeds limit
	if ((span->data.list_size >= (MIN_SPAN_CACHE_RELEASE + MIN_SPAN_CACHE_SIZE)) &&
		(span->data.list_size > counter->cache_limit)) {
		//Release to global cache
		count_t list_size = 1;
		span_t* next = span->next_span;
		span_t* last = span;
		while (list_size < MIN_SPAN_CACHE_RELEASE) {
			last = next;
			next = next->next_span;
			++list_size;
		}
		assert(next->next_span);
		next->data.list_size = span->data.list_size - list_size;
		last->next_span = 0; //Terminate list
		*cache = next;
		_memory_global_cache_large_insert(span, list_size, idx + 1);
#if ENABLE_STATISTICS
		heap->thread_to_global += list_size * (idx + 1) * SPAN_MAX_SIZE;
#endif
	}
#endif
#endif
}

//! Process pending deferred cross-thread deallocations
static int
_memory_deallocate_deferred(heap_t* heap, size_t size_class) {
	//Grab the current list of deferred deallocations
	std::atomic_thread_fence(std::memory_order_acquire);
	void* p = heap->defer_deallocate.load();
	if (!p)
		return 0;
	if (!std::atomic_compare_exchange_strong(&heap->defer_deallocate, &p, (void*)0))
		return 0;
	//Keep track if we deallocate in the given size class
	int got_class = 0;
	do {
		void* next = *(void**)p;
		//Get span and check which type of block
		span_t* span = static_cast<span_t*>((void*)((uintptr_t)p & SPAN_MASK));
		if (span->size_class < SIZE_CLASS_COUNT) {
			//Small/medium block
			got_class |= (span->size_class == size_class);
			_memory_deallocate_to_heap(heap, span, p);
		}
		else {
			//Large block
			got_class |= ((span->size_class >= size_class) && (span->size_class <= (size_class + 2)));
			_memory_deallocate_large_to_heap(heap, span);
		}
		//Loop until all pending operations in list are processed
		p = next;
	} while (p);
	return got_class;
}

//! Defer deallocation of the given block to the given heap
static void
_memory_deallocate_defer(int32_t heap_id, void* p) {
	//Get the heap and link in pointer in list of deferred opeations
	heap_t* heap = _memory_heap_lookup(heap_id);
	void* last_ptr = heap->defer_deallocate.load();
	do {
		*(void**)p = last_ptr; //Safe to use block, it's being deallocated
	} while (!std::atomic_compare_exchange_weak(&heap->defer_deallocate, &last_ptr, p));
}

//! Allocate a block of the given size
static void*
_memory_allocate(size_t size) {
	if (size <= MEDIUM_SIZE_LIMIT)
		return _memory_allocate_from_heap(RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap), size);
	else if (size <= LARGE_SIZE_LIMIT)
		return _memory_allocate_large_from_heap(RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap), size);

	//Oversized, allocate pages directly
	size += SPAN_HEADER_SIZE;
	size_t num_pages = size / RPMALLOC_PAGE_SIZE;
	if (size % RPMALLOC_PAGE_SIZE)
		++num_pages;
	span_t* span = _memory_map(num_pages);
	span->heap_id = 0;
	//Store page count in next_span
	span->next_span = (span_t*)((uintptr_t)num_pages);

	return pointer_offset(span, SPAN_HEADER_SIZE);
}

//! Deallocate the given block
static void
_memory_deallocate(void* p) {
	if (!p)
		return;

	//Grab the span (always at start of span, using 64KiB alignment)
	span_t* span = static_cast<span_t*>((void*)((uintptr_t)p & SPAN_MASK));
	int32_t heap_id = span->heap_id.load();
	heap_t* heap = RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap);
	//Check if block belongs to this heap or if deallocation should be deferred
	if (heap_id == heap->id) {
		if (span->size_class < SIZE_CLASS_COUNT)
			_memory_deallocate_to_heap(heap, span, p);
		else
			_memory_deallocate_large_to_heap(heap, span);
	}
	else if (heap_id > 0) {
		_memory_deallocate_defer(heap_id, p);
	}
	else {
		_memory_unmap(span);
	}
}

//! Reallocate the given block to the given size
static void*
_memory_reallocate(void* p, size_t size, size_t oldsize, unsigned int flags) {
	if (p) {
		//Grab the span (always at start of span, using 64KiB alignment)
		span_t* span = static_cast<span_t*>((void*)((uintptr_t)p & SPAN_MASK));
		int32_t heap_id = span->heap_id.load();
		if (heap_id) {
			if (span->size_class < SIZE_CLASS_COUNT) {
				//Small/medium sized block
				size_class_t* size_class = _memory_size_class + span->size_class;
				if ((size_t)size_class->size >= size)
					return p; //Still fits in block, never mind trying to save memory
				if (!oldsize)
					oldsize = size_class->size;
			}
			else {
				//Large block
				size_t total_size = size + SPAN_HEADER_SIZE;
				size_t num_spans = total_size / SPAN_MAX_SIZE;
				if (total_size % SPAN_MAX_SIZE)
					++num_spans;
				size_t current_spans = (span->size_class - SIZE_CLASS_COUNT) + 1;
				if ((current_spans >= num_spans) && (num_spans >= (current_spans / 2)))
					return p; //Still fits and less than half of memory would be freed
				if (!oldsize)
					oldsize = (current_spans * (size_t)SPAN_MAX_SIZE) - SPAN_HEADER_SIZE;
			}
		}
		else {
			//Oversized block
			size_t total_size = size + SPAN_HEADER_SIZE;
			size_t num_pages = total_size / RPMALLOC_PAGE_SIZE;
			if (total_size % RPMALLOC_PAGE_SIZE)
				++num_pages;
			//Page count is stored in next_span
			size_t current_pages = (size_t)span->next_span;
			if ((current_pages >= num_pages) && (num_pages >= (current_pages / 2)))
				return p; //Still fits and less than half of memory would be freed
			if (!oldsize)
				oldsize = (current_pages * (size_t)RPMALLOC_PAGE_SIZE) - SPAN_HEADER_SIZE;
		}
	}

	//Size is greater than block size, need to allocate a new block and deallocate the old
	//Avoid hysteresis by overallocating if increase is small (below 37%)
	size_t lower_bound = oldsize + (oldsize >> 2) + (oldsize >> 3);
	void* block = _memory_allocate(size > lower_bound ? size : lower_bound);
	if (p) {
		if (!(flags & RPMALLOC_NO_PRESERVE))
			memcpy(block, p, oldsize < size ? oldsize : size);
		_memory_deallocate(p);
	}

	return block;
}

//! Get the usable size of the given block
static size_t
_memory_usable_size(void* p) {
	//Grab the span (always at start of span, using 64KiB alignment)
	span_t* span = static_cast<span_t*>((void*)((uintptr_t)p & SPAN_MASK));
	int32_t heap_id = span->heap_id.load();
	if (heap_id) {
		if (span->size_class < SIZE_CLASS_COUNT) {
			//Small/medium block
			size_class_t* size_class = _memory_size_class + span->size_class;
			return size_class->size;
		}

		//Large block
		size_t current_spans = (span->size_class - SIZE_CLASS_COUNT) + 1;
		return (current_spans * (size_t)SPAN_MAX_SIZE) - SPAN_HEADER_SIZE;
	}

	//Oversized block, page count is stored in next_span
	size_t current_pages = (size_t)span->next_span;
	return (current_pages * (size_t)RPMALLOC_PAGE_SIZE) - SPAN_HEADER_SIZE;
}

//! Adjust and optimize the size class properties for the given class
static void
_memory_adjust_size_class(size_t iclass) {
	//Calculate how many pages are needed for 255 blocks
	size_t block_size = _memory_size_class[iclass].size;

	//TODO: STNK Hardcode always 16 pages
	size_t page_count = QUICK_ALLOCATION_PAGES_COUNT;
	size_t block_count = ((page_count * RPMALLOC_PAGE_SIZE) - SPAN_HEADER_SIZE) / block_size;
	//Store the final configuration
	_memory_size_class[iclass].page_count = (uint16_t)page_count;
	_memory_size_class[iclass].block_count = (uint16_t)block_count;
	_memory_size_class[iclass].class_idx = (uint16_t)iclass;

	//Check if previous size classes can be merged
	size_t prevclass = iclass;
	while (prevclass > 0) {
		--prevclass;
		//A class can be merged if number of pages and number of blocks are equal
		if ((_memory_size_class[prevclass].page_count == _memory_size_class[iclass].page_count) &&
			(_memory_size_class[prevclass].block_count == _memory_size_class[iclass].block_count)) {
			memcpy(_memory_size_class + prevclass, _memory_size_class + iclass, sizeof(_memory_size_class[iclass]));
		}
		else {
			break;
		}
	}
}

//! Initialize the allocator and setup global data
int
rpmalloc_initialize(void) {
	RPMALLOC_INITIALIZE_THREAD_LOCAL(_memory_thread_heap);
	RPMALLOC_INITIALIZE_THREAD_LOCAL(_memory_preferred_heap);
	_memory_heap_id = 0;

	//Setup all small and medium size classes
	size_t iclass;
	for (iclass = 0; iclass < SMALL_CLASS_COUNT; ++iclass) {
		size_t size = (iclass + 1) * SMALL_GRANULARITY;
		_memory_size_class[iclass].size = (uint16_t)size;
		_memory_adjust_size_class(iclass);
	}
	for (iclass = 0; iclass < MEDIUM_CLASS_COUNT; ++iclass) {
		size_t size = SMALL_SIZE_LIMIT + ((iclass + 1) * MEDIUM_GRANULARITY);
		if (size > MEDIUM_SIZE_LIMIT)
			size = MEDIUM_SIZE_LIMIT;
		_memory_size_class[SMALL_CLASS_COUNT + iclass].size = (uint16_t)size;
		_memory_adjust_size_class(SMALL_CLASS_COUNT + iclass);
	}

	_segments_head = 0;
	_segments_rw_lock = 0;
	
	//Initialize this thread
	rpmalloc_thread_initialize();
	return 0;
}

//! Finalize the allocator
void
rpmalloc_finalize(void) {
	std::atomic_thread_fence(std::memory_order_acquire);

	//Free all thread caches
	for (size_t list_idx = 0; list_idx < HEAP_ARRAY_SIZE; ++list_idx) {
		heap_t* heap = _get_heap_ptr(_memory_heaps[list_idx].load());
		if (heap) {
			_memory_deallocate_deferred(heap, 0);

			{
				const size_t page_count = QUICK_ALLOCATION_PAGES_COUNT;
				span_t* span = heap->span_cache;
				unsigned int span_count = span ? span->data.list_size : 0;
				for (unsigned int ispan = 0; ispan < span_count; ++ispan) {
					span_t* next_span = span->next_span;
					_memory_unmap(span);
					span = next_span;
				}
			}

			//Free large spans
			for (size_t iclass = 0; iclass < LARGE_CLASS_COUNT; ++iclass) {
				const size_t span_count = iclass + 1;
				span_t* span = heap->large_cache[iclass];
				while (span) {
					span_t* next_span = span->next_span;
					_memory_unmap(span);
					span = next_span;
				}
			}
			_memory_deallocate_external(heap);
		}

		_memory_heaps[list_idx] = 0;
	}

	{
		//Free global caches
		void* span_ptr = _memory_span_cache.load();
		size_t cache_count = (uintptr_t)span_ptr & ~SPAN_MASK;
		span_t* span = (span_t*)((void*)((uintptr_t)span_ptr & SPAN_MASK));
		while (cache_count) {
			span_t* skip_span = span->prev_span;
			unsigned int span_count = span->data.list_size;
			for (unsigned int ispan = 0; ispan < span_count; ++ispan) {
				span_t* next_span = span->next_span;
				_memory_unmap(span);
				span = next_span;
			}
			span = skip_span;
			cache_count -= span_count;
		}
		_memory_span_cache = 0;
	}

	for (size_t iclass = 0; iclass < LARGE_CLASS_COUNT; ++iclass) {
		void* span_ptr = _memory_large_cache[iclass].load();
		size_t cache_count = (uintptr_t)span_ptr & ~SPAN_MASK;
		span_t* span = (span_t*)((void*)((uintptr_t)span_ptr & SPAN_MASK));
		while (cache_count) {
			span_t* skip_span = span->prev_span;
			unsigned int span_count = span->data.list_size;
			for (unsigned int ispan = 0; ispan < span_count; ++ispan) {
				span_t* next_span = span->next_span;
				_memory_unmap(span);
				span = next_span;
			}
			span = skip_span;
			cache_count -= span_count;
		}
		_memory_large_cache[iclass] = 0;
	}

	// Free all segments
	_acquire_segments_lock_write();

	segment_t* head = static_cast<segment_t*>(_segments_head.load());
	segment_t* next;
	while (head)
	{
		next = static_cast<segment_t*>(head->next_segment.load());
		_memory_deallocate_external(head);
		head = next;
	}

	_release_segments_lock_write();

	RPMALLOC_FREE_THREAD_LOCAL(_memory_thread_heap);
	RPMALLOC_FREE_THREAD_LOCAL(_memory_preferred_heap);
	
	std::atomic_thread_fence(std::memory_order_release);
}

int _is_heap_in_use(void* heap) {
	return ((size_t)heap & 1);
}

void* _mark_heap_in_use(void* heap) {
	return (void*)((size_t)heap | 1);
}

void* _unmark_heap_in_use(void* heap) {
	return (void*)((size_t)heap & ~1);
}

void _acquire_heaps_lock()
{
	while (_heaps_lock.test_and_set(std::memory_order_acquire));
}

void _release_heaps_lock()
{
	_heaps_lock.clear(std::memory_order_release);
}

//! Initialize thread, assign heap
void
rpmalloc_thread_initialize(void) {
	if (!RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap)) {
		_acquire_heaps_lock();

		void* heap_ptr = _memory_heaps[RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(uint32_t, _memory_preferred_heap)].load();

		for (uint32_t id = RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(uint32_t, _memory_preferred_heap);
			id < HEAP_ARRAY_SIZE + RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(uint32_t, _memory_preferred_heap);
			++id)
		{
			heap_ptr = _memory_heaps[id % HEAP_ARRAY_SIZE].load();
			if (heap_ptr && !_is_heap_in_use(heap_ptr))
			{
				RPMALLOC_SET_THREAD_LOCAL(_memory_thread_heap, _get_heap_ptr(heap_ptr));
				_memory_heaps[id % HEAP_ARRAY_SIZE] = _mark_heap_in_use(heap_ptr);
				RPMALLOC_SET_THREAD_LOCAL(_memory_preferred_heap, (id % HEAP_ARRAY_SIZE));
				break;
			}
		}

		if (!RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap))
		{
			// We have to allocate a new heap
			heap_t* heap = _memory_allocate_heap();

			int32_t id = heap->id;
			RPMALLOC_SET_THREAD_LOCAL(_memory_thread_heap, heap);
			_memory_heaps[id] = _mark_heap_in_use(heap);
			RPMALLOC_SET_THREAD_LOCAL(_memory_preferred_heap, id);
		}

		_release_heaps_lock();

#if ENABLE_STATISTICS
		RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap)->thread_to_global = 0;
		RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap)->global_to_thread = 0;
#endif
	}
	assert(RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap));
}

void
rpmalloc_thread_reset(void) {
	if (!RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap))
		return;

	_acquire_heaps_lock();
	void* heap_ptr = _memory_heaps[RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(uint32_t, _memory_preferred_heap)].load();
	assert(_is_heap_in_use(heap_ptr));
	_memory_heaps[RPMALLOC_GET_THREAD_LOCAL_LITERAL_VALUE(uint32_t, _memory_preferred_heap)] = _unmark_heap_in_use(heap_ptr);
	_release_heaps_lock();

	RPMALLOC_SET_THREAD_LOCAL(_memory_thread_heap, nullptr);
}

int
rpmalloc_is_thread_initialized(void) {
	return (RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap) != 0) ? 1 : 0;
}

RPMALLOC_FORCEINLINE int _get_readers_count(int32_t l)
{
	return l & 0x7FFFFFFF;
}

void _acquire_segments_lock_read()
{
	int32_t l = _segments_rw_lock.load();
	int32_t new_l = 0;
	do {
		// Try to increase the readers count
		new_l = _get_readers_count(l) + 1;
	} while (!std::atomic_compare_exchange_weak(&_segments_rw_lock, &l, new_l));
}

void _release_segments_lock_read()
{
	int32_t l = _segments_rw_lock.load();
	int32_t new_l = 0;
	do {
		// Try to decrease the readers count
		new_l = _get_readers_count(l) - 1;
	} while (!std::atomic_compare_exchange_weak(&_segments_rw_lock, &l, new_l));
}

void _acquire_segments_lock_write()
{
	int32_t exp = 0;
	while (!std::atomic_compare_exchange_weak(&_segments_rw_lock, &exp, 1))
	{
		exp = 0;
	}
}

void _release_segments_lock_write()
{
	int32_t exp = 1;
	while (!std::atomic_compare_exchange_weak(&_segments_rw_lock, &exp, 0))
	{
		exp = 1;
	}
}

void _publish_segment_on_global_list(segment_t* new_segment)
{
	_acquire_segments_lock_write();

	segment_t* head = static_cast<segment_t*>(_segments_head.load());
	new_segment->next_segment = head;
	_segments_head = new_segment;

	_release_segments_lock_write();
}

span_t* _get_span_from_segment(segment_t* new_segment) {
	int slot = SPANS_PER_SEGMENT + 1;
	while (slot == SPANS_PER_SEGMENT + 1) {
		uint32_t slots = new_segment->free_markers.load();
		#if SPANS_PER_SEGMENT == 32
		if (slots == 0xFFFFFFFF) {
			return 0; // It's full
		}
		#elif SPANS_PER_SEGMENT == 16
		if (slots == 0x0000FFFF) {
			return 0; // It's full
		}
		#else
			#pragma error Unsupported spans per-segment
		#endif
		// Find a free slot
		for (int b = 0; b < SPANS_PER_SEGMENT; ++b) {
			const uint32_t bit_mask = (1 << b);
			if ((slots & bit_mask) == 0)
			{
				uint32_t new_slots = slots | bit_mask;
				// Try to mark the slot as used
				if (std::atomic_compare_exchange_strong(&new_segment->free_markers, &slots, new_slots))
				{
					slot = b;
					break;
				}
			}
		}
	}
	span_t* span = static_cast<span_t*>(pointer_offset(new_segment->first_span, slot * SPAN_MAX_SIZE));

	span->owner_segment = new_segment;
	// Calculate the address of the slot
	return span;
}

void _return_span_to_segment(segment_t* segment, span_t* span) {
	span_t* first_span = segment->first_span;
	uint32_t slot = (uint32_t)(pointer_diff(span, first_span) / SPAN_MAX_SIZE);

	assert(slot < SPANS_PER_SEGMENT);

	uint32_t slots = segment->free_markers.load();
	uint32_t new_slots;
	do
	{
		new_slots = slots & ~(1 << slot);
	} while (!std::atomic_compare_exchange_weak(&segment->free_markers, &slots, new_slots));

	// Check if it's completely empty and we should free it
	if (new_slots == 0)
	{
		_acquire_segments_lock_write();

		// Find the segment and delete it (if still empty)
		segment_t* prev = 0;
		segment_t* head = static_cast<segment_t*>(_segments_head.load());
		while (head && head != segment)
		{
			prev = head;
			head = static_cast<segment_t*>(head->next_segment.load());
		}

		if (head)
		{
			slots = head->free_markers.load();
			new_slots = slots & ~(1 << slot);

			// Still empty?
			if (new_slots == 0)
			{
				assert(head == segment);

				// It's the head
				if (!prev)
				{
					_segments_head = head->next_segment.load();
				}
				else
				{
					prev->next_segment = head->next_segment.load();
				}
			}
			else
			{
				head = 0;
			}
		}

		_release_segments_lock_write();

		// Now free if everything went ok - the segment has been unlinked
		if (head)
		{
#if ENABLE_STATISTICS
			_mapped_pages -= QUICK_ALLOCATION_PAGES_COUNT;
			_unmapped_total -= QUICK_ALLOCATION_PAGES_COUNT;
#endif
			_memory_deallocate_external(head);
		}
	}
}

//! Map new pages to virtual memory
static span_t*
_memory_map(size_t page_count) {
	span_t* span = 0;
	if (page_count <= QUICK_ALLOCATION_PAGES_COUNT)
	{
		int should_release = 1;
		_acquire_segments_lock_read();
		// Look for a span of correct size within the cache
		segment_t* global_segment = static_cast<segment_t*>(_segments_head.load());
		for (;;)
		{
			if (!global_segment)
			{
				// Create a new cachable segment and put it on the cache
				size_t size = (SPAN_MAX_SIZE * SPANS_PER_SEGMENT) + sizeof(segment_t) + SPAN_ADDRESS_GRANULARITY;
				segment_t* segment = static_cast<segment_t*>(_memory_allocate_external(size));
				memset(segment, 0, sizeof(segment_t));
				span = static_cast<span_t*>(pointer_offset(segment, sizeof(segment_t)));
				// Align to the required size of the spans
				if ((uintptr_t)span & (SPAN_ADDRESS_GRANULARITY - 1))
					span = static_cast<span_t*>((void*)(((uintptr_t)span & ~((uintptr_t)SPAN_ADDRESS_GRANULARITY - 1)) + SPAN_ADDRESS_GRANULARITY));
				span->owner_segment = segment;

				segment->first_span = span;
				// Directly use this first span
				segment->free_markers = 1;

				_release_segments_lock_read();
				should_release = 0;

				_publish_segment_on_global_list(segment);
			}
			else
			{
				span = _get_span_from_segment(global_segment);
				if (!span)
				{
					// Try the next one
					global_segment = static_cast<segment_t*>(global_segment->next_segment.load());
				}
			}

			if (span)
			{
				break;
			}
		}

		if (should_release)
		{
			_release_segments_lock_read();
		}
	}
	// Allocate a segment of the minimal required size
	else
	{
		size_t size = page_count * RPMALLOC_PAGE_SIZE + sizeof(segment_t) + SPAN_ADDRESS_GRANULARITY;
		segment_t* segment = static_cast<segment_t*>(_memory_allocate_external(size));
		span = static_cast<span_t*>(pointer_offset(segment, sizeof(segment_t)));
		// Align to the required size of the spans
		if ((uintptr_t)span & (SPAN_ADDRESS_GRANULARITY - 1))
			span = static_cast<span_t*>((void*)(((uintptr_t)span & ~((uintptr_t)SPAN_ADDRESS_GRANULARITY - 1)) + SPAN_ADDRESS_GRANULARITY));

		segment->first_span = span;
		segment->next_segment = (void*)SINGLE_SEGMENT_MARKER;
		
		// Allocate a segment that will not be re-used
		span->owner_segment = segment;
	}
	assert(span && span->owner_segment);
	return span;
}

//! Unmap pages from virtual memory
static void
_memory_unmap(span_t* span) {
	segment_t* segment = span->owner_segment;
	assert(segment);
	segment_t* nn = static_cast<segment_t*>(segment->next_segment.load());
	if (nn == (void*)SINGLE_SEGMENT_MARKER)
	{
		_memory_deallocate_external(segment);

#if ENABLE_STATISTICS
		_mapped_pages -= QUICK_ALLOCATION_PAGES_COUNT;
		_unmapped_total -= QUICK_ALLOCATION_PAGES_COUNT;
#endif
	}
	else
	{
		_return_span_to_segment(segment, span);
	}
}

static void*
_memory_allocate_external(size_t bytes)
{
#if ENABLE_STATISTICS
	_mapped_total += (int)(bytes / RPMALLOC_PAGE_SIZE);
	_mapped_pages += (int)(bytes / RPMALLOC_PAGE_SIZE);
#endif
	return rpmalloc_allocate_memory_external(bytes);
}

static void
_memory_deallocate_external(void* ptr)
{
	rpmalloc_deallocate_memory_external(ptr);
}

//! Yield the thread remaining timeslice
static void
thread_yield(void) {}

// Extern interface

RPMALLOC_CALL void*
rpmalloc(size_t size) {
#if ENABLE_VALIDATE_ARGS
	if (size >= MAX_ALLOC_SIZE) {
		errno = EINVAL;
		return 0;
	}
#endif
	return _memory_allocate(size);
}

void
rpfree(void* ptr) {
	_memory_deallocate(ptr);
}

RPMALLOC_CALL void*
rpcalloc(size_t num, size_t size) {
	size_t total;
#if ENABLE_VALIDATE_ARGS
#ifdef RPMALLOC_PLATFORM_WIN
	int err = SizeTMult(num, size, &total);
	if ((err != S_OK) || (total >= MAX_ALLOC_SIZE)) {
		errno = EINVAL;
		return 0;
	}
#else
	int err = __builtin_umull_overflow(num, size, &total);
	if (err || (total >= MAX_ALLOC_SIZE)) {
		errno = EINVAL;
		return 0;
	}
#endif
#else
	total = num * size;
#endif
	void* ptr = _memory_allocate(total);
	memset(ptr, 0, total);
	return ptr;
}

void*
rprealloc(void* ptr, size_t size) {
#if ENABLE_VALIDATE_ARGS
	if (size >= MAX_ALLOC_SIZE) {
		errno = EINVAL;
		return ptr;
	}
#endif
	return _memory_reallocate(ptr, size, 0, 0);
}

void*
rpaligned_realloc(void* ptr, size_t alignment, size_t size, size_t oldsize,
	unsigned int flags) {
	(void)alignment; // Silence warning
#if ENABLE_VALIDATE_ARGS
	if (size + alignment < size) {
		errno = EINVAL;
		return 0;
	}
#endif
	//TODO: If alignment > 16, we need to copy to new aligned position
	(void)sizeof(alignment);
	return _memory_reallocate(ptr, size, oldsize, flags);
}

RPMALLOC_CALL void*
rpaligned_alloc(size_t alignment, size_t size) {
	if (alignment <= 16)
		return rpmalloc(size);

#if ENABLE_VALIDATE_ARGS
	if (size + alignment < size) {
		errno = EINVAL;
		return 0;
	}
#endif

	if (alignment >= (SPAN_MAX_SIZE - SPAN_HEADER_SIZE))
	{
		assert(false && "Alignment is too large - not supported!");
		return 0;
	}

	void* ptr = rpmalloc(size + alignment);
	if ((uintptr_t)ptr & (alignment - 1))
		ptr = (void*)(((uintptr_t)ptr & ~((uintptr_t)alignment - 1)) + alignment);

	return ptr;
}

size_t
rpmalloc_usable_size(void* ptr) {
	return ptr ? _memory_usable_size(ptr) : 0;
}

void
rpmalloc_thread_collect(void) {
	_memory_deallocate_deferred(RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap), 0);
}

void
rpmalloc_thread_statistics(rpmalloc_thread_statistics_t* stats) {
	memset(stats, 0, sizeof(rpmalloc_thread_statistics_t));
	heap_t* heap = RPMALLOC_GET_THREAD_LOCAL(heap_t*, _memory_thread_heap);
#if ENABLE_STATISTICS
	stats->allocated = heap->allocated;
	stats->requested = heap->requested;
#endif
	void* p = heap->defer_deallocate.load();
	while (p) {
		void* next = *(void**)p;
		span_t* span = static_cast<span_t*>((void*)((uintptr_t)p & SPAN_MASK));
		stats->deferred += _memory_size_class[span->size_class].size;
		p = next;
	}

	for (size_t isize = 0; isize < SIZE_CLASS_COUNT; ++isize) {
		if (heap->active_block[isize].free_count)
			stats->active += heap->active_block[isize].free_count * _memory_size_class[heap->active_span[isize]->size_class].size;

		span_t* cache = heap->size_cache[isize];
		while (cache) {
			stats->sizecache = cache->data.block.free_count * _memory_size_class[cache->size_class].size;
			cache = cache->next_span;
		}
	}

	if (heap->span_cache)
		stats->spancache = (size_t)heap->span_cache->data.list_size * QUICK_ALLOCATION_PAGES_COUNT * RPMALLOC_PAGE_SIZE;
}

void
rpmalloc_global_statistics(rpmalloc_global_statistics_t* stats) {
	memset(stats, 0, sizeof(rpmalloc_global_statistics_t));
#if ENABLE_STATISTICS
	stats->mapped = _mapped_pages * RPMALLOC_PAGE_SIZE;
	stats->mapped_total = _mapped_total * RPMALLOC_PAGE_SIZE;
	stats->unmapped_total = _unmapped_total * RPMALLOC_PAGE_SIZE;
#endif
	{
		void* global_span_ptr = _memory_span_cache.load();
		while (global_span_ptr == SPAN_LIST_LOCK_TOKEN) {
			thread_yield();
			global_span_ptr = _memory_span_cache.load();
		}
		uintptr_t global_span_count = (uintptr_t)global_span_ptr & ~SPAN_MASK;
		size_t list_bytes = global_span_count * QUICK_ALLOCATION_PAGES_COUNT * RPMALLOC_PAGE_SIZE;
		stats->cached += list_bytes;
	}

	for (size_t iclass = 0; iclass < LARGE_CLASS_COUNT; ++iclass) {
		void* global_span_ptr = _memory_large_cache[iclass].load();
		while (global_span_ptr == SPAN_LIST_LOCK_TOKEN) {
			thread_yield();
			global_span_ptr = _memory_large_cache[iclass].load();
		}
		uintptr_t global_span_count = (uintptr_t)global_span_ptr & ~SPAN_MASK;
		size_t list_bytes = global_span_count * (iclass + 1) * SPAN_MAX_PAGE_COUNT * RPMALLOC_PAGE_SIZE;
		stats->cached_large += list_bytes;
	}
}

}