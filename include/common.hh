#pragma once

#include <cstddef>

const size_t CACHE_LINE_SIZE = 64;

#define CACHE_LINE_ALIGN __attribute__((aligned(CACHE_LINE_SIZE)))