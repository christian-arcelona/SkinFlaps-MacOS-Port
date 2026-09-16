// Cross-platform aligned allocation helpers (plumbing only)
#pragma once

#include <cstddef>
#include <cstdlib>

#if defined(_WIN32)
#  include <malloc.h>
#endif

namespace pd {

inline void* aligned_malloc(size_t alignment, size_t size) {
  if (size == 0) return nullptr;
#if defined(_WIN32)
  return _aligned_malloc(size, alignment);
#else
  // posix_memalign requires alignment to be a multiple of sizeof(void*) and power of two
  if (alignment < sizeof(void*)) alignment = sizeof(void*);
  void* p = nullptr;
  if (posix_memalign(&p, alignment, size) != 0) return nullptr;
  return p;
#endif
}

inline void aligned_free(void* p) {
  if (!p) return;
#if defined(_WIN32)
  _aligned_free(p);
#else
  free(p);
#endif
}

} // namespace pd

