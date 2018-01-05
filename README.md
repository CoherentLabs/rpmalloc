# coherent-rpmalloc - Coherent Labs' fork of the Rampant Pixels Memory Allocator
This library provides a public domain cross platform lock free thread caching 16-byte aligned memory allocator implemented in C++.
This version is a fork of https://github.com/rampantpixels/rpmalloc.

Platforms currently supported:

- Windows
- MacOS
- iOS
- Linux
- Android
- PlayStation 4
- Xbox One

This library is put in the public domain; you can redistribute it and/or modify it without any restrictions. Or, if you choose, you can use it under the MIT license.

Originally created by Mattias Jansson ([@maniccoder](https://twitter.com/maniccoder)) / Rampant Pixels - http://www.rampantpixels.com

Fork and modifications by Coherent Labs ([@CoherentLabs](https://twitter.com/CoherentLabs)) - http://www.coherent-labs.com

# Implementation

A detailed description of the rpmalloc original allocator is available at the GitHub page of the creator https://github.com/rampantpixels/rpmalloc. This version has significant changes and modifications.

The allocator provides generic C++ high-performance memory management across multiple platforms.

Key features include:

- High performance in multi-threaded environments
- Very low memory overhead for book-keeping
- Embeddability - does not depend on the OS allocator
- Cross-platform - requires a C++11 atomic STL implementation, no other external dependencies

# Differences from the main rpmalloc

coherent-rpmalloc removes some significant requirements of the original implementation.

- No need to align external allocations to 64KB.
- Reduced the allocation granularities and how much memory the allocator "keeps", which in turn reduces the overall footprint of memory managed by it.
- Removed span classes, because all spans have the same number of pages.
- Moved to C++11 in order to easily port on multiple platforms that have no compiler support for C11.
- Relaxed the performance impact of thread initialization/deinitialization. When needing to allocate memory though coherent-rpmalloc, the user has to call *rpmalloc_thread_initialize*, when she is done, she can call *rpmalloc_thread_reset*. Both operations are very light-weight as the internal structures will be re-used.

# Usage

The embedding application must implement the *rpmalloc_allocate_memory_external* and *rpmalloc_deallocate_memory_external* functions that will be called when the allocator needs new space to use or returns some to the OS.

The allocator has to be initialized and deinitialized with the *rpmalloc_initialize* and *rpmalloc_finalize* functions.

After that allocations can be performed with *rpmalloc* and family functions. On each thread that will use the allocator, the user has to call *rpmalloc_thread_initialize*. When done with allocations on a thread, the user should call *rpmalloc_thread_reset*. Both operations are very light-weight, so it's usually OK to call them often  - for instance when spawning short-lived threads.

