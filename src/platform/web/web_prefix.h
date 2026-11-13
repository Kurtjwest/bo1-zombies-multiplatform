// Included first in every file of the browser build (cmake/web.cmake): what the build depends on, checked in each file.
#pragma once

// long double is the Windows game's double (-mlong-double-64, cmake/web.cmake): the engine's structures with one in
// them (the menu parser's tokens) must have the same layout in every file
#if defined(__cplusplus)
static_assert(sizeof(long double) == 8, "the browser build needs -mlong-double-64 (cmake/web.cmake)");
#else
_Static_assert(sizeof(long double) == 8, "the browser build needs -mlong-double-64 (cmake/web.cmake)");
#endif
