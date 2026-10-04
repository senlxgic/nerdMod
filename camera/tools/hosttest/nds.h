// Minimal stand-in for <nds.h> so the container code can be tested on a PC.
#pragma once
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
inline void DC_FlushRange(const void *, u32) {}
