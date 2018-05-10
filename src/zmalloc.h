#ifndef __ZMALLOC_H
#define __ZMALLOC_H

#include "config.h"

#if defined(USE_TCMALLOC)
#include <google/tcmalloc.h>
#if (TC_VERSION_MAJOR == 1 && TC_VERSION_MINOR >= 6) || (TC_VERSION_MAJOR > 1)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) tc_malloc_size(p)
#else
#error "Newer version of tcmalloc required"
#endif
#elif defined(USE_JEMMALOC)
#include <jemalloc/jemalloc.h>
#if (JEMALLOC_VERSION_MAJOR == 2 && JEMALLOC_VERSION_MINOR >= 1) || (JEMALLOC_VERSION_MAJOR > 2)
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) je_malloc_usable_size(p)
#else
#error "Newer version of jemalloc required"
#endif
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define HAVE_MALLOC_SIZE 1
#define zmalloc_size(p) malloc_size(p)
#endif

#ifndef ZMALLOC_LIB
#define ZMALLOC_LIB "libc"
#endif

/* We can enable the defrag capabilities only if we are using Jemalloc
 * and the version used is our special version modified for having
 * the ability to return per-allocation fragmentation hints. */
#if defined(USE_JEMALLOC) && defined(JEMALLOC_FRAG_HINT)
#define HAVE_DEFRAG
#endif

void *zmalloc(size_t size);
void *zcalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
char *zstrdup(const char *s);
size_t zmalloc_used_memory(void);
size_t zmalloc_get_memory_size(void);

void *debug_zmalloc(size_t size, const char *file, int line, const char *func);
void debug_zfree(void *ptr, const char *file, int line, const char *func);

#ifdef DEBUG_ZMALLOC
#define zmalloc(X) (debug_zmalloc(X, __FILE__, __LINE__, __FUNCTION__))
#define zfree(X) (debug_zfree(X, __FILE__, __LINE__, __FUNCTION__))
#endif

#endif /* __ZMALLOC_H */
