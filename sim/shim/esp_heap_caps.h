#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_free(p) free(p)
