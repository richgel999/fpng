// fpng.cpp - Copyright (C) 2021 Richard Geldreich, Jr. - See Apache 2.0 license at the end of this file.
// Fast 24/32bpp .PNG image writer/reader. 
//
// Important: Must link with Crc32.cpp.
// The only external dependency is to an external fast CRC-32 function, which is provided in Crc32.cpp/.h. See fpng_crc32() below to replace this function with something else.
//
// Uses code from the simple PNG writer function by Alex Evans, 2011. Released into the public domain: https://gist.github.com/908299
// Some low-level Deflate/Huffman functions derived from the original 2011 Google Code version of miniz (public domain by R. Geldreich, Jr.): https://code.google.com/archive/p/miniz/
// Low-level Huffman code size function: public domain, originally written by: Alistair Moffat, alistair@cs.mu.oz.au, Jyrki Katajainen, jyrki@diku.dk, November 1996.
#include "fpng.h"
#include <assert.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning (disable:4127) // conditional expression is constant
#endif
// This module relies on the fast CRC-32 function in Crc32.cpp/.h.
#include "Crc32.h"

#ifndef FPNG_NO_STDIO
#include <stdio.h>
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
	#ifndef __LITTLE_ENDIAN
	#define __LITTLE_ENDIAN 1234
	#endif
	#ifndef __BIG_ENDIAN
	#define __BIG_ENDIAN 4321
	#endif

	// Assume little endian on Windows.
	#define __BYTE_ORDER __LITTLE_ENDIAN
#elif defined(__APPLE__)
	#define __BYTE_ORDER __BYTE_ORDER__
	#define __LITTLE_ENDIAN __LITTLE_ENDIAN__
	#define __BIG_ENDIAN __BIG_ENDIAN__
#else
	// for __BYTE_ORDER (__LITTLE_ENDIAN or __BIG_ENDIAN)
	#include <sys/param.h>

	#ifndef __LITTLE_ENDIAN
	#define __LITTLE_ENDIAN 1234
	#endif
	#ifndef __BIG_ENDIAN
	#define __BIG_ENDIAN 4321
	#endif
#endif

#if !defined(__BYTE_ORDER)
	#error __BYTE_ORDER undefined. Compile with -D__BYTE_ORDER=1234 for little endian or -D__BYTE_ORDER=4321 for big endian.
#endif

// Allow the disabling of the chunk data CRC32 checks, for fuzz testing of the decoder
#define FPNG_DISABLE_DECODE_CRC32_CHECKS (0)

namespace fpng
{
	static const int FPNG_FALSE = 0, FPNG_ADLER32_INIT = 1;
	static const uint8_t FPNG_FDEC_VERSION = 0;
	static const uint32_t FPNG_MAX_SUPPORTED_DIM = 1 << 24;

	template <typename S> static inline S maximum(S a, S b) { return (a > b) ? a : b; }
	template <typename S> static inline S minimum(S a, S b) { return (a < b) ? a : b; }
		
	static inline uint16_t simple_swap16(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
	static inline uint32_t simple_swap32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0x0000FF00) | ((x << 8) & 0x00FF0000) | (x << 24); }
	static inline uint64_t simple_swap64(uint64_t x) { return (((uint64_t)simple_swap32((uint32_t)x)) << 32U) | simple_swap32((uint32_t)(x >> 32U)); }

	static inline uint16_t swap16(uint16_t x)
	{
#if defined(__GNUC__) || defined(__clang__)
		return __builtin_bswap16(x);
#else
		return simple_swap16(x);
#endif
	}

	static inline uint32_t swap32(uint32_t x)
	{
#if defined(__GNUC__) || defined(__clang__)
		return __builtin_bswap32(x);
#else
		return simple_swap32(x);
#endif
	}

	static inline uint64_t swap64(uint64_t x)
	{
#if defined(__GNUC__) || defined(__clang__)
		return __builtin_bswap64(x);
#else
		return simple_swap64(x);
#endif
	}

#if __BYTE_ORDER == __BIG_ENDIAN
	#define READ_LE16(p) swap16(*reinterpret_cast<const uint16_t *>(p))
	#define READ_LE32(p) swap32(*reinterpret_cast<const uint32_t *>(p))
	#define WRITE_LE32(p, v) *reinterpret_cast<uint32_t *>(p) = swap32((uint32_t)(v))
	#define WRITE_LE64(p, v) *reinterpret_cast<uint64_t *>(p) = swap64((uint64_t)(v))
	
	#define READ_BE32(p) *reinterpret_cast<const uint32_t *>(p)
#else
	#define READ_LE16(p) (*reinterpret_cast<const uint16_t *>(p))
	#define READ_LE32(p) (*reinterpret_cast<const uint32_t *>(p))
	#define WRITE_LE32(p, v) *reinterpret_cast<uint32_t *>(p) = (uint32_t)(v)
	#define WRITE_LE64(p, v) *reinterpret_cast<uint64_t *>(p) = (uint64_t)(v)

	#define READ_BE32(p) swap32(*reinterpret_cast<const uint32_t *>(p))
#endif
				
	const uint32_t FPNG_CRC32_INIT = 0;
	static inline uint32_t fpng_crc32(uint32_t prev_crc32, const void* pData, size_t size)
	{
		// Call into Crc32.cpp. Feel free to replace this with something faster.
		return crc32_fast(pData, size, prev_crc32);
	}
		
	// Vanilla adler32 function. Feel free to replace this with something faster.
	static uint32_t fpng_adler32(uint32_t adler, const uint8_t* ptr, size_t buf_len)
	{
		uint32_t i, s1 = (uint32_t)(adler & 0xffff), s2 = (uint32_t)(adler >> 16); uint32_t block_len = (uint32_t)(buf_len % 5552);
		if (!ptr) return FPNG_ADLER32_INIT;
		while (buf_len) {
			for (i = 0; i + 7 < block_len; i += 8, ptr += 8) {
				s1 += ptr[0], s2 += s1; s1 += ptr[1], s2 += s1; s1 += ptr[2], s2 += s1; s1 += ptr[3], s2 += s1;
				s1 += ptr[4], s2 += s1; s1 += ptr[5], s2 += s1; s1 += ptr[6], s2 += s1; s1 += ptr[7], s2 += s1;
			}
			for (; i < block_len; ++i) s1 += *ptr++, s2 += s1;
			s1 %= 65521U, s2 %= 65521U; buf_len -= block_len; block_len = 5552;
		}
		return (s2 << 16) + s1;
	}
		
	static const uint16_t g_defl_len_sym[256] = {
	  257,258,259,260,261,262,263,264,265,265,266,266,267,267,268,268,269,269,269,269,270,270,270,270,271,271,271,271,272,272,272,272,
	  273,273,273,273,273,273,273,273,274,274,274,274,274,274,274,274,275,275,275,275,275,275,275,275,276,276,276,276,276,276,276,276,
	  277,277,277,277,277,277,277,277,277,277,277,277,277,277,277,277,278,278,278,278,278,278,278,278,278,278,278,278,278,278,278,278,
	  279,279,279,279,279,279,279,279,279,279,279,279,279,279,279,279,280,280,280,280,280,280,280,280,280,280,280,280,280,280,280,280,
	  281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,281,
	  282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,282,
	  283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,283,
	  284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,284,285 };

	static const uint8_t g_defl_len_extra[256] = {
	  0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,
	  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
	  5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
	  5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,0 };

	static const uint8_t g_defl_small_dist_sym[512] = {
	  0,1,2,3,4,4,5,5,6,6,6,6,7,7,7,7,8,8,8,8,8,8,8,8,9,9,9,9,9,9,9,9,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,10,11,11,11,11,11,11,
	  11,11,11,11,11,11,11,11,11,11,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,13,
	  13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,14,14,14,14,14,14,14,14,14,14,14,14,
	  14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,14,
	  14,14,14,14,14,14,14,14,14,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
	  15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,16,16,16,16,16,16,16,16,16,16,16,16,16,
	  16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
	  16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,
	  16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,16,17,17,17,17,17,17,17,17,17,17,17,17,17,17,
	  17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,
	  17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,
	  17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17,17 };
		
	static const uint32_t g_bitmasks[17] = { 0x0000, 0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF };

	static const uint8_t g_dyn_huff_3[] = { 120, 1, 229, 194, 3, 176, 37, 75, 148, 5, 208, 189, 79, 102, 86, 213, 197, 99, 187, 231, 143, 109, 219, 182, 109, 219, 182, 109, 219, 182, 109, 219,
		198, 31, 207, 159, 118, 63, 94, 84, 85, 102, 158, 61, 21, 241, 34, 58, 38, 198, 102, 196 };
	const uint32_t DYN_HUFF_3_BITBUF = 0x2, DYN_HUFF_3_BITBUF_SIZE = 3;
		
	static const struct { uint8_t m_code_size; uint16_t m_code; } g_dyn_huff_3_codes[288] =
	{
		{3,0x0},{3,0x4},{4,0x6},{5,0x1},{5,0x11},{5,0x9},{6,0xD},{6,0x2D},{6,0x1D},{7,0x33},{7,0x73},{7,0xB},{7,0x4B},{8,0x3B},{8,0xBB},{8,0x7B},
		{8,0xFB},{8,0x7},{8,0x87},{9,0x97},{9,0x197},{9,0x57},{9,0x157},{9,0xD7},{9,0x1D7},{9,0x37},{9,0x137},{12,0x24F},{10,0x18F},{12,0xA4F},{12,0x64F},{12,0xE4F},
		{12,0x14F},{12,0x94F},{12,0x54F},{12,0xD4F},{12,0x34F},{12,0xB4F},{12,0x74F},{12,0xF4F},{12,0xCF},{12,0x8CF},{12,0x4CF},{12,0xCCF},{12,0x2CF},{12,0xACF},{12,0x6CF},{12,0xECF},
		{12,0x1CF},{12,0x9CF},{12,0x5CF},{12,0xDCF},{12,0x3CF},{12,0xBCF},{12,0x7CF},{12,0xFCF},{12,0x2F},{12,0x82F},{12,0x42F},{12,0xC2F},{12,0x22F},{12,0xA2F},{12,0x62F},{12,0xE2F},
		{12,0x12F},{12,0x92F},{12,0x52F},{12,0xD2F},{12,0x32F},{12,0xB2F},{12,0x72F},{12,0xF2F},{12,0xAF},{12,0x8AF},{12,0x4AF},{12,0xCAF},{12,0x2AF},{12,0xAAF},{12,0x6AF},{12,0xEAF},
		{12,0x1AF},{12,0x9AF},{12,0x5AF},{12,0xDAF},{12,0x3AF},{12,0xBAF},{12,0x7AF},{12,0xFAF},{12,0x6F},{12,0x86F},{12,0x46F},{12,0xC6F},{12,0x26F},{12,0xA6F},{12,0x66F},{12,0xE6F},
		{12,0x16F},{12,0x96F},{12,0x56F},{12,0xD6F},{12,0x36F},{12,0xB6F},{12,0x76F},{12,0xF6F},{12,0xEF},{12,0x8EF},{12,0x4EF},{12,0xCEF},{12,0x2EF},{12,0xAEF},{12,0x6EF},{12,0xEEF},
		{12,0x1EF},{12,0x9EF},{12,0x5EF},{12,0xDEF},{12,0x3EF},{12,0xBEF},{12,0x7EF},{12,0xFEF},{12,0x1F},{12,0x81F},{12,0x41F},{12,0xC1F},{12,0x21F},{12,0xA1F},{12,0x61F},{12,0xE1F},
		{12,0x11F},{12,0x91F},{12,0x51F},{12,0xD1F},{12,0x31F},{12,0xB1F},{12,0x71F},{12,0xF1F},{12,0x9F},{12,0x89F},{12,0x49F},{12,0xC9F},{12,0x29F},{12,0xA9F},{12,0x69F},{12,0xE9F},
		{12,0x19F},{12,0x99F},{12,0x59F},{12,0xD9F},{12,0x39F},{12,0xB9F},{12,0x79F},{12,0xF9F},{12,0x5F},{12,0x85F},{12,0x45F},{12,0xC5F},{12,0x25F},{12,0xA5F},{12,0x65F},{12,0xE5F},
		{12,0x15F},{12,0x95F},{12,0x55F},{12,0xD5F},{12,0x35F},{12,0xB5F},{12,0x75F},{12,0xF5F},{12,0xDF},{12,0x8DF},{12,0x4DF},{12,0xCDF},{12,0x2DF},{12,0xADF},{12,0x6DF},{12,0xEDF},
		{12,0x1DF},{12,0x9DF},{12,0x5DF},{12,0xDDF},{12,0x3DF},{12,0xBDF},{12,0x7DF},{12,0xFDF},{12,0x3F},{12,0x83F},{12,0x43F},{12,0xC3F},{12,0x23F},{12,0xA3F},{12,0x63F},{12,0xE3F},
		{12,0x13F},{12,0x93F},{12,0x53F},{12,0xD3F},{12,0x33F},{12,0xB3F},{12,0x73F},{12,0xF3F},{12,0xBF},{12,0x8BF},{12,0x4BF},{12,0xCBF},{12,0x2BF},{12,0xABF},{12,0x6BF},{12,0xEBF},
		{12,0x1BF},{12,0x9BF},{12,0x5BF},{12,0xDBF},{12,0x3BF},{12,0xBBF},{12,0x7BF},{12,0xFBF},{12,0x7F},{12,0x87F},{12,0x47F},{10,0x38F},{12,0xC7F},{12,0x27F},{12,0xA7F},{12,0x67F},
		{12,0xE7F},{12,0x17F},{12,0x97F},{12,0x57F},{10,0x4F},{12,0xD7F},{9,0xB7},{9,0x1B7},{9,0x77},{9,0x177},{9,0xF7},{9,0x1F7},{9,0xF},{9,0x10F},{8,0x47},{8,0xC7},
		{8,0x27},{8,0xA7},{8,0x67},{8,0xE7},{7,0x2B},{7,0x6B},{7,0x1B},{7,0x5B},{6,0x3D},{6,0x3},{6,0x23},{5,0x19},{5,0x5},{5,0x15},{4,0xE},{3,0x2},
		{12,0x37F},{6,0x13},{0,0x0},{0,0x0},{8,0x17},{0,0x0},{0,0x0},{9,0x8F},{0,0x0},{12,0xB7F},{0,0x0},{12,0x77F},{12,0xF7F},{12,0xFF},{12,0x8FF},{12,0x4FF},
		{12,0xCFF},{12,0x2FF},{12,0xAFF},{12,0x6FF},{12,0xEFF},{12,0x1FF},{12,0x9FF},{12,0x5FF},{12,0xDFF},{12,0x3FF},{12,0xBFF},{12,0x7FF},{12,0xFFF},{0,0x0},{0,0x0},{0,0x0}
	};

	static const uint8_t g_dyn_huff_4[] = { 120,1,229,195,83,144,37,219,182,0,208,49,87,230,70,177,171,121,204,171,103,219,182,109,219,182,109,219,182,109,219,214,
		197,177,154,213,197,141,204,53,95,228,71,69,116,156,56,207,126,251,99 };
	const uint32_t DYN_HUFF_4_BITBUF = 0x0, DYN_HUFF_4_BITBUF_SIZE = 2;

	static const struct { uint8_t m_code_size; uint16_t m_code; } g_dyn_huff_4_codes[288] =
	{
		{1,0x0},{4,0x1},{5,0x5},{6,0xD},{6,0x2D},{7,0x23},{7,0x63},{7,0x13},{7,0x53},{8,0x6B},{8,0xEB},{8,0x1B},{8,0x9B},{8,0x5B},{8,0xDB},{9,0xA7},
		{8,0x3B},{9,0x1A7},{9,0x67},{9,0x167},{9,0xE7},{9,0x1E7},{9,0x17},{10,0x137},{10,0x337},{10,0xB7},{10,0x2B7},{10,0x1B7},{10,0x3B7},{10,0x77},{10,0x277},{10,0x177},
		{10,0x377},{10,0xF7},{10,0x2F7},{11,0x34F},{11,0x74F},{11,0xCF},{11,0x4CF},{11,0x2CF},{12,0x7CF},{12,0xFCF},{12,0x2F},{12,0x82F},{12,0x42F},{12,0xC2F},{12,0x22F},{12,0xA2F},
		{12,0x62F},{12,0xE2F},{12,0x12F},{12,0x92F},{12,0x52F},{12,0xD2F},{12,0x32F},{12,0xB2F},{12,0x72F},{12,0xF2F},{12,0xAF},{12,0x8AF},{12,0x4AF},{12,0xCAF},{12,0x2AF},{12,0xAAF},
		{12,0x6AF},{12,0xEAF},{12,0x1AF},{12,0x9AF},{12,0x5AF},{12,0xDAF},{12,0x3AF},{12,0xBAF},{12,0x7AF},{12,0xFAF},{12,0x6F},{12,0x86F},{12,0x46F},{12,0xC6F},{12,0x26F},{12,0xA6F},
		{12,0x66F},{12,0xE6F},{12,0x16F},{12,0x96F},{12,0x56F},{12,0xD6F},{12,0x36F},{12,0xB6F},{12,0x76F},{12,0xF6F},{12,0xEF},{12,0x8EF},{12,0x4EF},{12,0xCEF},{12,0x2EF},{12,0xAEF},
		{12,0x6EF},{12,0xEEF},{12,0x1EF},{12,0x9EF},{12,0x5EF},{12,0xDEF},{12,0x3EF},{12,0xBEF},{12,0x7EF},{12,0xFEF},{12,0x1F},{12,0x81F},{12,0x41F},{12,0xC1F},{12,0x21F},{12,0xA1F},
		{12,0x61F},{12,0xE1F},{12,0x11F},{12,0x91F},{12,0x51F},{12,0xD1F},{12,0x31F},{12,0xB1F},{12,0x71F},{12,0xF1F},{12,0x9F},{12,0x89F},{12,0x49F},{12,0xC9F},{12,0x29F},{12,0xA9F},
		{12,0x69F},{12,0xE9F},{12,0x19F},{12,0x99F},{12,0x59F},{12,0xD9F},{12,0x39F},{12,0xB9F},{12,0x79F},{12,0xF9F},{12,0x5F},{12,0x85F},{12,0x45F},{12,0xC5F},{12,0x25F},{12,0xA5F},
		{12,0x65F},{12,0xE5F},{12,0x15F},{12,0x95F},{12,0x55F},{12,0xD5F},{12,0x35F},{12,0xB5F},{12,0x75F},{12,0xF5F},{12,0xDF},{12,0x8DF},{12,0x4DF},{12,0xCDF},{12,0x2DF},{12,0xADF},
		{12,0x6DF},{12,0xEDF},{12,0x1DF},{12,0x9DF},{12,0x5DF},{12,0xDDF},{12,0x3DF},{12,0xBDF},{12,0x7DF},{12,0xFDF},{12,0x3F},{12,0x83F},{12,0x43F},{12,0xC3F},{12,0x23F},{12,0xA3F},
		{12,0x63F},{12,0xE3F},{12,0x13F},{12,0x93F},{12,0x53F},{12,0xD3F},{12,0x33F},{12,0xB3F},{12,0x73F},{12,0xF3F},{12,0xBF},{12,0x8BF},{12,0x4BF},{12,0xCBF},{12,0x2BF},{12,0xABF},
		{12,0x6BF},{12,0xEBF},{12,0x1BF},{12,0x9BF},{12,0x5BF},{12,0xDBF},{12,0x3BF},{12,0xBBF},{12,0x7BF},{12,0xFBF},{12,0x7F},{12,0x87F},{12,0x47F},{12,0xC7F},{12,0x27F},{12,0xA7F},
		{12,0x67F},{12,0xE7F},{12,0x17F},{12,0x97F},{12,0x57F},{12,0xD7F},{12,0x37F},{12,0xB7F},{12,0x77F},{12,0xF7F},{12,0xFF},{11,0x6CF},{11,0x1CF},{11,0x5CF},{11,0x3CF},{10,0x1F7},
		{10,0x3F7},{10,0xF},{10,0x20F},{10,0x10F},{10,0x30F},{10,0x8F},{10,0x28F},{10,0x18F},{10,0x38F},{10,0x4F},{9,0x117},{9,0x97},{9,0x197},{9,0x57},{9,0x157},{9,0xD7},
		{8,0xBB},{9,0x1D7},{8,0x7B},{8,0xFB},{8,0x7},{8,0x87},{8,0x47},{8,0xC7},{7,0x33},{7,0x73},{7,0xB},{7,0x4B},{6,0x1D},{6,0x3D},{5,0x15},{4,0x9},
		{12,0x8FF},{0,0x0},{6,0x3},{0,0x0},{0,0x0},{0,0x0},{8,0x27},{0,0x0},{0,0x0},{9,0x37},{0,0x0},{10,0x24F},{0,0x0},{10,0x14F},{12,0x4FF},{12,0xCFF},
		{12,0x2FF},{12,0xAFF},{12,0x6FF},{12,0xEFF},{12,0x1FF},{12,0x9FF},{12,0x5FF},{12,0xDFF},{12,0x3FF},{12,0xBFF},{12,0x7FF},{12,0xFFF},{7,0x2B},{0,0x0},{0,0x0},{0,0x0},
	};

#define PUT_BITS(bb, ll) do { uint32_t b = bb, l = ll; assert((l) >= 0 && (l) <= 16); assert((b) < (1ULL << (l))); bit_buf |= (((uint64_t)(b)) << bit_buf_size); bit_buf_size += (l); assert(bit_buf_size <= 64); } while(0)
#define PUT_BITS_CZ(bb, ll) do { uint32_t b = bb, l = ll; assert((l) >= 1 && (l) <= 16); assert((b) < (1ULL << (l))); bit_buf |= (((uint64_t)(b)) << bit_buf_size); bit_buf_size += (l); assert(bit_buf_size <= 64); } while(0)

#define PUT_BITS_FLUSH do { \
	if ((dst_ofs + 8) > dst_buf_size) \
		return 0; \
	WRITE_LE64(pDst + dst_ofs, bit_buf); \
	uint32_t bits_to_shift = bit_buf_size & ~7; \
	dst_ofs += (bits_to_shift >> 3); \
	assert(bits_to_shift < 64); \
	bit_buf = bit_buf >> bits_to_shift; \
	bit_buf_size -= bits_to_shift; \
} while(0)

#define PUT_BITS_FORCE_FLUSH do { \
	while (bit_buf_size > 0) \
	{ \
		if ((dst_ofs + 1) > dst_buf_size) \
			return 0; \
		*(uint8_t*)(pDst + dst_ofs) = (uint8_t)bit_buf; \
		dst_ofs++; \
		bit_buf >>= 8; \
		bit_buf_size -= 8; \
	} \
} while(0)

	enum
	{
		DEFL_MAX_HUFF_TABLES = 3,
		DEFL_MAX_HUFF_SYMBOLS = 288,
		DEFL_MAX_HUFF_SYMBOLS_0 = 288,
		DEFL_MAX_HUFF_SYMBOLS_1 = 32,
		DEFL_MAX_HUFF_SYMBOLS_2 = 19,
		DEFL_LZ_DICT_SIZE = 32768,
		DEFL_LZ_DICT_SIZE_MASK = DEFL_LZ_DICT_SIZE - 1,
		DEFL_MIN_MATCH_LEN = 3,
		DEFL_MAX_MATCH_LEN = 258
	};

	struct defl_huff
	{
		uint16_t m_huff_count[DEFL_MAX_HUFF_TABLES][DEFL_MAX_HUFF_SYMBOLS];
		uint16_t m_huff_codes[DEFL_MAX_HUFF_TABLES][DEFL_MAX_HUFF_SYMBOLS];
		uint8_t m_huff_code_sizes[DEFL_MAX_HUFF_TABLES][DEFL_MAX_HUFF_SYMBOLS];
	};

	struct defl_sym_freq
	{
		uint16_t m_key;
		uint16_t m_sym_index;
	};

#define DEFL_CLEAR_OBJ(obj) memset(&(obj), 0, sizeof(obj))

	static defl_sym_freq* defl_radix_sort_syms(uint32_t num_syms, defl_sym_freq* pSyms0, defl_sym_freq* pSyms1)
	{
		uint32_t total_passes = 2, pass_shift, pass, i, hist[256 * 2]; defl_sym_freq* pCur_syms = pSyms0, * pNew_syms = pSyms1; DEFL_CLEAR_OBJ(hist);
		for (i = 0; i < num_syms; i++) { uint32_t freq = pSyms0[i].m_key; hist[freq & 0xFF]++; hist[256 + ((freq >> 8) & 0xFF)]++; }
		while ((total_passes > 1) && (num_syms == hist[(total_passes - 1) * 256])) total_passes--;
		for (pass_shift = 0, pass = 0; pass < total_passes; pass++, pass_shift += 8)
		{
			const uint32_t* pHist = &hist[pass << 8];
			uint32_t offsets[256], cur_ofs = 0;
			for (i = 0; i < 256; i++) { offsets[i] = cur_ofs; cur_ofs += pHist[i]; }
			for (i = 0; i < num_syms; i++) pNew_syms[offsets[(pCur_syms[i].m_key >> pass_shift) & 0xFF]++] = pCur_syms[i];
			{ defl_sym_freq* t = pCur_syms; pCur_syms = pNew_syms; pNew_syms = t; }
		}
		return pCur_syms;
	}

	// defl_calculate_minimum_redundancy() originally written by: Alistair Moffat, alistair@cs.mu.oz.au, Jyrki Katajainen, jyrki@diku.dk, November 1996.
	static void defl_calculate_minimum_redundancy(defl_sym_freq* A, int n)
	{
		int root, leaf, next, avbl, used, dpth;
		if (n == 0) return; else if (n == 1) { A[0].m_key = 1; return; }
		A[0].m_key += A[1].m_key; root = 0; leaf = 2;
		for (next = 1; next < n - 1; next++)
		{
			if (leaf >= n || A[root].m_key < A[leaf].m_key) { A[next].m_key = A[root].m_key; A[root++].m_key = (uint16_t)next; }
			else A[next].m_key = A[leaf++].m_key;
			if (leaf >= n || (root < next && A[root].m_key < A[leaf].m_key)) { A[next].m_key = (uint16_t)(A[next].m_key + A[root].m_key); A[root++].m_key = (uint16_t)next; }
			else A[next].m_key = (uint16_t)(A[next].m_key + A[leaf++].m_key);
		}
		A[n - 2].m_key = 0; for (next = n - 3; next >= 0; next--) A[next].m_key = A[A[next].m_key].m_key + 1;
		avbl = 1; used = dpth = 0; root = n - 2; next = n - 1;
		while (avbl > 0)
		{
			while (root >= 0 && (int)A[root].m_key == dpth) { used++; root--; }
			while (avbl > used) { A[next--].m_key = (uint16_t)(dpth); avbl--; }
			avbl = 2 * used; dpth++; used = 0;
		}
	}

	// Limits canonical Huffman code table's max code size.
	enum { DEFL_MAX_SUPPORTED_HUFF_CODESIZE = 32 };
	static void defl_huffman_enforce_max_code_size(int* pNum_codes, int code_list_len, int max_code_size)
	{
		int i; uint32_t total = 0; if (code_list_len <= 1) return;
		for (i = max_code_size + 1; i <= DEFL_MAX_SUPPORTED_HUFF_CODESIZE; i++) pNum_codes[max_code_size] += pNum_codes[i];
		for (i = max_code_size; i > 0; i--) total += (((uint32_t)pNum_codes[i]) << (max_code_size - i));
		while (total != (1UL << max_code_size))
		{
			pNum_codes[max_code_size]--;
			for (i = max_code_size - 1; i > 0; i--) if (pNum_codes[i]) { pNum_codes[i]--; pNum_codes[i + 1] += 2; break; }
			total--;
		}
	}

	static void defl_optimize_huffman_table(defl_huff* d, int table_num, int table_len, int code_size_limit, int static_table)
	{
		int i, j, l, num_codes[1 + DEFL_MAX_SUPPORTED_HUFF_CODESIZE]; uint32_t next_code[DEFL_MAX_SUPPORTED_HUFF_CODESIZE + 1]; DEFL_CLEAR_OBJ(num_codes);
		if (static_table)
		{
			for (i = 0; i < table_len; i++) num_codes[d->m_huff_code_sizes[table_num][i]]++;
		}
		else
		{
			defl_sym_freq syms0[DEFL_MAX_HUFF_SYMBOLS], syms1[DEFL_MAX_HUFF_SYMBOLS], * pSyms;
			int num_used_syms = 0;
			const uint16_t* pSym_count = &d->m_huff_count[table_num][0];
			for (i = 0; i < table_len; i++) if (pSym_count[i]) { syms0[num_used_syms].m_key = (uint16_t)pSym_count[i]; syms0[num_used_syms++].m_sym_index = (uint16_t)i; }

			pSyms = defl_radix_sort_syms(num_used_syms, syms0, syms1); defl_calculate_minimum_redundancy(pSyms, num_used_syms);

			for (i = 0; i < num_used_syms; i++) num_codes[pSyms[i].m_key]++;

			defl_huffman_enforce_max_code_size(num_codes, num_used_syms, code_size_limit);

			DEFL_CLEAR_OBJ(d->m_huff_code_sizes[table_num]); DEFL_CLEAR_OBJ(d->m_huff_codes[table_num]);
			for (i = 1, j = num_used_syms; i <= code_size_limit; i++)
				for (l = num_codes[i]; l > 0; l--) d->m_huff_code_sizes[table_num][pSyms[--j].m_sym_index] = (uint8_t)(i);
		}

		next_code[1] = 0; for (j = 0, i = 2; i <= code_size_limit; i++) next_code[i] = j = ((j + num_codes[i - 1]) << 1);

		for (i = 0; i < table_len; i++)
		{
			uint32_t rev_code = 0, code, code_size; if ((code_size = d->m_huff_code_sizes[table_num][i]) == 0) continue;
			code = next_code[code_size]++; for (l = code_size; l > 0; l--, code >>= 1) rev_code = (rev_code << 1) | (code & 1);
			d->m_huff_codes[table_num][i] = (uint16_t)rev_code;
		}
	}

#define DEFL_RLE_PREV_CODE_SIZE() { if (rle_repeat_count) { \
  if (rle_repeat_count < 3) { \
    d->m_huff_count[2][prev_code_size] = (uint16_t)(d->m_huff_count[2][prev_code_size] + rle_repeat_count); \
    while (rle_repeat_count--) packed_code_sizes[num_packed_code_sizes++] = prev_code_size; \
  } else { \
    d->m_huff_count[2][16] = (uint16_t)(d->m_huff_count[2][16] + 1); packed_code_sizes[num_packed_code_sizes++] = 16; packed_code_sizes[num_packed_code_sizes++] = (uint8_t)(rle_repeat_count - 3); \
} rle_repeat_count = 0; } }

#define DEFL_RLE_ZERO_CODE_SIZE() { if (rle_z_count) { \
  if (rle_z_count < 3) { \
    d->m_huff_count[2][0] = (uint16_t)(d->m_huff_count[2][0] + rle_z_count); while (rle_z_count--) packed_code_sizes[num_packed_code_sizes++] = 0; \
  } else if (rle_z_count <= 10) { \
    d->m_huff_count[2][17] = (uint16_t)(d->m_huff_count[2][17] + 1); packed_code_sizes[num_packed_code_sizes++] = 17; packed_code_sizes[num_packed_code_sizes++] = (uint8_t)(rle_z_count - 3); \
  } else { \
    d->m_huff_count[2][18] = (uint16_t)(d->m_huff_count[2][18] + 1); packed_code_sizes[num_packed_code_sizes++] = 18; packed_code_sizes[num_packed_code_sizes++] = (uint8_t)(rle_z_count - 11); \
} rle_z_count = 0; } }

	static uint8_t g_defl_packed_code_size_syms_swizzle[] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

#define DEFL_DYN_PUT_BITS(bb, ll) \
do { \
	uint32_t b = (bb), l = (ll); \
	assert((l) >= 1 && (l) <= 16); assert((b) < (1ULL << (l))); \
	bit_buf |= (((uint64_t)(b)) << bit_buf_size); bit_buf_size += (l); assert(bit_buf_size <= 64); \
	while (bit_buf_size >= 8) \
	{ \
		if ((dst_ofs + 1) > dst_buf_size) \
			return false; \
		*(uint8_t*)(pDst + dst_ofs) = (uint8_t)bit_buf; \
		dst_ofs++; \
		bit_buf >>= 8; \
		bit_buf_size -= 8; \
	} \
} while(0)

	static bool defl_start_dynamic_block(defl_huff* d, uint8_t* pDst, uint32_t& dst_ofs, uint32_t dst_buf_size, uint64_t& bit_buf, int& bit_buf_size)
	{
		int num_lit_codes, num_dist_codes, num_bit_lengths; uint32_t i, total_code_sizes_to_pack, num_packed_code_sizes, rle_z_count, rle_repeat_count, packed_code_sizes_index;
		uint8_t code_sizes_to_pack[DEFL_MAX_HUFF_SYMBOLS_0 + DEFL_MAX_HUFF_SYMBOLS_1], packed_code_sizes[DEFL_MAX_HUFF_SYMBOLS_0 + DEFL_MAX_HUFF_SYMBOLS_1], prev_code_size = 0xFF;

		d->m_huff_count[0][256] = 1;

		defl_optimize_huffman_table(d, 0, DEFL_MAX_HUFF_SYMBOLS_0, 12, FPNG_FALSE);
		defl_optimize_huffman_table(d, 1, DEFL_MAX_HUFF_SYMBOLS_1, 12, FPNG_FALSE);

		for (num_lit_codes = 286; num_lit_codes > 257; num_lit_codes--) if (d->m_huff_code_sizes[0][num_lit_codes - 1]) break;
		for (num_dist_codes = 30; num_dist_codes > 1; num_dist_codes--) if (d->m_huff_code_sizes[1][num_dist_codes - 1]) break;

		memcpy(code_sizes_to_pack, &d->m_huff_code_sizes[0][0], num_lit_codes);
		memcpy(code_sizes_to_pack + num_lit_codes, &d->m_huff_code_sizes[1][0], num_dist_codes);
		total_code_sizes_to_pack = num_lit_codes + num_dist_codes; num_packed_code_sizes = 0; rle_z_count = 0; rle_repeat_count = 0;

		memset(&d->m_huff_count[2][0], 0, sizeof(d->m_huff_count[2][0]) * DEFL_MAX_HUFF_SYMBOLS_2);
		for (i = 0; i < total_code_sizes_to_pack; i++)
		{
			uint8_t code_size = code_sizes_to_pack[i];
			if (!code_size)
			{
				DEFL_RLE_PREV_CODE_SIZE();
				if (++rle_z_count == 138) { DEFL_RLE_ZERO_CODE_SIZE(); }
			}
			else
			{
				DEFL_RLE_ZERO_CODE_SIZE();
				if (code_size != prev_code_size)
				{
					DEFL_RLE_PREV_CODE_SIZE();
					d->m_huff_count[2][code_size] = (uint16_t)(d->m_huff_count[2][code_size] + 1); packed_code_sizes[num_packed_code_sizes++] = code_size;
				}
				else if (++rle_repeat_count == 6)
				{
					DEFL_RLE_PREV_CODE_SIZE();
				}
			}
			prev_code_size = code_size;
		}
		if (rle_repeat_count) { DEFL_RLE_PREV_CODE_SIZE(); }
		else { DEFL_RLE_ZERO_CODE_SIZE(); }

		defl_optimize_huffman_table(d, 2, DEFL_MAX_HUFF_SYMBOLS_2, 7, FPNG_FALSE);

		// max of 2+5+5+4+18*3+(288+32)*7=2310 bits
		DEFL_DYN_PUT_BITS(2, 2);

		DEFL_DYN_PUT_BITS(num_lit_codes - 257, 5);
		DEFL_DYN_PUT_BITS(num_dist_codes - 1, 5);

		for (num_bit_lengths = 18; num_bit_lengths >= 0; num_bit_lengths--) if (d->m_huff_code_sizes[2][g_defl_packed_code_size_syms_swizzle[num_bit_lengths]]) break;
		num_bit_lengths = maximum<int>(4, (num_bit_lengths + 1)); DEFL_DYN_PUT_BITS(num_bit_lengths - 4, 4);
		for (i = 0; (int)i < num_bit_lengths; i++) DEFL_DYN_PUT_BITS(d->m_huff_code_sizes[2][g_defl_packed_code_size_syms_swizzle[i]], 3);

		for (packed_code_sizes_index = 0; packed_code_sizes_index < num_packed_code_sizes; )
		{
			uint32_t code = packed_code_sizes[packed_code_sizes_index++]; assert(code < DEFL_MAX_HUFF_SYMBOLS_2);
			DEFL_DYN_PUT_BITS(d->m_huff_codes[2][code], d->m_huff_code_sizes[2][code]);
			if (code >= 16) DEFL_DYN_PUT_BITS(packed_code_sizes[packed_code_sizes_index++], "\02\03\07"[code - 16]);
		}

		return true;
	}

	static uint32_t write_raw_block(const uint8_t* pSrc, uint32_t src_len, uint8_t* pDst, uint32_t dst_buf_size)
	{
		if (dst_buf_size < 2)
			return 0;

		pDst[0] = 0x78;
		pDst[1] = 0x01;

		uint32_t dst_ofs = 2;

		uint32_t src_ofs = 0;
		while (src_ofs < src_len)
		{
			const uint32_t src_remaining = src_len - src_ofs;
			const uint32_t block_size = minimum<uint32_t>(UINT16_MAX, src_remaining);
			const bool final_block = (block_size == src_remaining);

			if ((dst_ofs + 5 + block_size) > dst_buf_size)
				return 0;

			pDst[dst_ofs + 0] = final_block ? 1 : 0;

			pDst[dst_ofs + 1] = block_size & 0xFF;
			pDst[dst_ofs + 2] = (block_size >> 8) & 0xFF;

			pDst[dst_ofs + 3] = (~block_size) & 0xFF;
			pDst[dst_ofs + 4] = ((~block_size) >> 8) & 0xFF;

			memcpy(pDst + dst_ofs + 5, pSrc + src_ofs, block_size);

			src_ofs += block_size;
			dst_ofs += 5 + block_size;
		}

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pSrc, src_len);

		for (uint32_t i = 0; i < 4; i++)
		{
			if (dst_ofs + 1 > dst_buf_size)
				return 0;

			pDst[dst_ofs] = (uint8_t)(src_adler32 >> 24);
			dst_ofs++;

			src_adler32 <<= 8;
		}

		return dst_ofs;
	}

	static void adjust_freq32(uint32_t num_freq, uint32_t* pFreq, uint16_t* pFreq16)
	{
		uint32_t total_freq = 0;
		for (uint32_t i = 0; i < num_freq; i++)
			total_freq += pFreq[i];

		if (!total_freq)
		{
			memset(pFreq16, 0, num_freq * sizeof(uint16_t));
			return;
		}

		uint32_t total_freq16 = 0;
		for (uint32_t i = 0; i < num_freq; i++)
		{
			uint64_t f = pFreq[i];
			if (!f)
			{
				pFreq16[i] = 0;
				continue;
			}

			pFreq16[i] = (uint16_t)maximum<uint32_t>(1, (uint32_t)((f * UINT16_MAX) / total_freq));

			total_freq16 += pFreq16[i];
		}

		while (total_freq16 > UINT16_MAX)
		{
			total_freq16 = 0;
			for (uint32_t i = 0; i < num_freq; i++)
			{
				if (pFreq[i])
				{
					pFreq[i] = maximum<uint32_t>(1, pFreq[i] >> 1);
					total_freq16 += pFreq[i];
				}
			}
		}
	}

	static uint32_t pixel_deflate_dyn_3_rle(
		const uint8_t* pImg, uint32_t w, uint32_t h,
		uint8_t* pDst, uint32_t dst_buf_size)
	{
		const uint32_t bpl = 1 + w * 3;

		uint64_t bit_buf = 0;
		int bit_buf_size = 0;

		uint32_t dst_ofs = 0;

		// zlib header
		PUT_BITS(0x78, 8);
		PUT_BITS(0x01, 8);

		// write BFINAL bit
		PUT_BITS(1, 1);

		std::vector<uint32_t> codes((w + 1) * h);
		uint32_t* pDst_codes = codes.data();

		uint32_t lit_freq[DEFL_MAX_HUFF_SYMBOLS_0];
		memset(lit_freq, 0, sizeof(lit_freq));
		
		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		const uint32_t dist_sym = g_defl_small_dist_sym[3 - 1];
				
		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			*pDst_codes++ = 1 | (filter_lit << 8);
			lit_freq[filter_lit]++;

			uint32_t prev_lits;

			{
				uint32_t lits = READ_LE32(pSrc + src_ofs) & 0xFFFFFF;

				*pDst_codes++ = lits << 8;

				lit_freq[lits & 0xFF]++;
				lit_freq[(lits >> 8) & 0xFF]++;
				lit_freq[lits >> 16]++;

				src_ofs += 3;

				prev_lits = lits;
			}

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs) & 0xFFFFFF;

				if (lits == prev_lits)
				{
					uint32_t match_len = 3;
					uint32_t max_match_len = minimum<int>(255, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						if ((READ_LE32(pSrc + src_ofs + match_len) & 0xFFFFFF) != lits)
							break;
						match_len += 3;
					}
										
					*pDst_codes++ = match_len - 1;

					uint32_t adj_match_len = match_len - 3;

					lit_freq[g_defl_len_sym[adj_match_len]]++;
					
					src_ofs += match_len;
				}
				else
				{
					*pDst_codes++ = lits << 8;

					lit_freq[lits & 0xFF]++;
					lit_freq[(lits >> 8) & 0xFF]++;
					lit_freq[lits >> 16]++;

					prev_lits = lits;

					src_ofs += 3;
				}

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);
		const uint32_t total_codes = (uint32_t)(pDst_codes - codes.data());
		assert(total_codes <= codes.size());
								
		defl_huff dh;
		
		lit_freq[256] = 1;

		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_0, lit_freq, &dh.m_huff_count[0][0]);

		memset(&dh.m_huff_count[1][0], 0, sizeof(dh.m_huff_count[1][0]) * DEFL_MAX_HUFF_SYMBOLS_1);
		dh.m_huff_count[1][dist_sym] = 1;

		if (!defl_start_dynamic_block(&dh, pDst, dst_ofs, dst_buf_size, bit_buf, bit_buf_size))
			return 0;

		assert(bit_buf_size <= 7);
		assert(dh.m_huff_codes[1][dist_sym] == 0 && dh.m_huff_code_sizes[1][dist_sym] == 1);
				
		for (uint32_t i = 0; i < total_codes; i++)
		{
			uint32_t c = codes[i];

			uint32_t c_type = c & 0xFF;
			if (c_type == 0)
			{
				uint32_t lits = c >> 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][lits & 0xFF], dh.m_huff_code_sizes[0][lits & 0xFF]);
				lits >>= 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][lits & 0xFF], dh.m_huff_code_sizes[0][lits & 0xFF]);
				lits >>= 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][lits], dh.m_huff_code_sizes[0][lits]);
			}
			else if (c_type == 1)
			{
				uint32_t lit = c >> 8;
				PUT_BITS_CZ(dh.m_huff_codes[0][lit], dh.m_huff_code_sizes[0][lit]);
			}
			else
			{
				uint32_t match_len = c_type + 1;

				uint32_t adj_match_len = match_len - 3;
				
				PUT_BITS_CZ(dh.m_huff_codes[0][g_defl_len_sym[adj_match_len]], dh.m_huff_code_sizes[0][g_defl_len_sym[adj_match_len]]);
				PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], g_defl_len_extra[adj_match_len] + 1); // up to 6 bits, +1 for the match distance Huff code which is always 0

				// no need to write the distance code, it's always 0
				//PUT_BITS_CZ(dh.m_huff_codes[1][dist_sym], dh.m_huff_code_sizes[1][dist_sym]);
			}

			// up to 55 bits
			PUT_BITS_FLUSH;
		}

		PUT_BITS_CZ(dh.m_huff_codes[0][256], dh.m_huff_code_sizes[0][256]);

		PUT_BITS_FORCE_FLUSH;

		// Write zlib adler32
		for (uint32_t i = 0; i < 4; i++)
		{
			if ((dst_ofs + 1) > dst_buf_size)
				return 0;
			*(uint8_t*)(pDst + dst_ofs) = (uint8_t)(src_adler32 >> 24);
			dst_ofs++;

			src_adler32 <<= 8;
		}

		return dst_ofs;
	}

	static uint32_t pixel_deflate_dyn_3_rle_one_pass(
		const uint8_t* pImg, uint32_t w, uint32_t h,
		uint8_t* pDst, uint32_t dst_buf_size)
	{
		const uint32_t bpl = 1 + w * 3;

		if (dst_buf_size < sizeof(g_dyn_huff_3))
			return false;
		memcpy(pDst, g_dyn_huff_3, sizeof(g_dyn_huff_3));
		uint32_t dst_ofs = sizeof(g_dyn_huff_3);

		uint64_t bit_buf = DYN_HUFF_3_BITBUF;
		int bit_buf_size = DYN_HUFF_3_BITBUF_SIZE;

		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			PUT_BITS_CZ(g_dyn_huff_3_codes[filter_lit].m_code, g_dyn_huff_3_codes[filter_lit].m_code_size);

			uint32_t prev_lits;

			{
				uint32_t lits = READ_LE32(pSrc + src_ofs) & 0xFFFFFF;

				PUT_BITS_CZ(g_dyn_huff_3_codes[lits & 0xFF].m_code, g_dyn_huff_3_codes[lits & 0xFF].m_code_size);
				PUT_BITS_CZ(g_dyn_huff_3_codes[(lits >> 8) & 0xFF].m_code, g_dyn_huff_3_codes[(lits >> 8) & 0xFF].m_code_size);
				PUT_BITS_CZ(g_dyn_huff_3_codes[(lits >> 16)].m_code, g_dyn_huff_3_codes[(lits >> 16)].m_code_size);

				src_ofs += 3;
			
				prev_lits = lits;
			}

			PUT_BITS_FLUSH;

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs) & 0xFFFFFF;

				if (lits == prev_lits)
				{
					uint32_t match_len = 3;
					uint32_t max_match_len = minimum<int>(255, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						if ((READ_LE32(pSrc + src_ofs + match_len) & 0xFFFFFF) != lits)
							break;
						match_len += 3;
					}
										
					uint32_t adj_match_len = match_len - 3;

					PUT_BITS_CZ(g_dyn_huff_3_codes[g_defl_len_sym[adj_match_len]].m_code, g_dyn_huff_3_codes[g_defl_len_sym[adj_match_len]].m_code_size);
					PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], g_defl_len_extra[adj_match_len] + 1); // up to 6 bits, +1 for the match distance Huff code which is always 0

					src_ofs += match_len;
				}
				else
				{
					PUT_BITS_CZ(g_dyn_huff_3_codes[lits & 0xFF].m_code, g_dyn_huff_3_codes[lits & 0xFF].m_code_size);
					PUT_BITS_CZ(g_dyn_huff_3_codes[(lits >> 8) & 0xFF].m_code, g_dyn_huff_3_codes[(lits >> 8) & 0xFF].m_code_size);
					PUT_BITS_CZ(g_dyn_huff_3_codes[(lits >> 16)].m_code, g_dyn_huff_3_codes[(lits >> 16)].m_code_size);
					
					prev_lits = lits;

					src_ofs += 3;
				}

				PUT_BITS_FLUSH;

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);
		
		assert(bit_buf_size <= 7);

		PUT_BITS_CZ(g_dyn_huff_3_codes[256].m_code, g_dyn_huff_3_codes[256].m_code_size);

		PUT_BITS_FORCE_FLUSH;

		// Write zlib adler32
		for (uint32_t i = 0; i < 4; i++)
		{
			if ((dst_ofs + 1) > dst_buf_size)
				return 0;
			*(uint8_t*)(pDst + dst_ofs) = (uint8_t)(src_adler32 >> 24);
			dst_ofs++;

			src_adler32 <<= 8;
		}

		return dst_ofs;
	}

	static uint32_t pixel_deflate_dyn_4_rle(
		const uint8_t* pImg, uint32_t w, uint32_t h,
		uint8_t* pDst, uint32_t dst_buf_size)
	{
		const uint32_t bpl = 1 + w * 4;

		uint64_t bit_buf = 0;
		int bit_buf_size = 0;

		uint32_t dst_ofs = 0;

		// zlib header
		PUT_BITS(0x78, 8);
		PUT_BITS(0x01, 8);

		// write BFINAL bit
		PUT_BITS(1, 1);

		std::vector<uint64_t> codes;
		codes.resize((w + 1) * h);
		uint64_t* pDst_codes = codes.data();

		uint32_t lit_freq[DEFL_MAX_HUFF_SYMBOLS_0];
		memset(lit_freq, 0, sizeof(lit_freq));

		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		const uint32_t dist_sym = g_defl_small_dist_sym[4 - 1];

		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			*pDst_codes++ = 1 | (filter_lit << 8);
			lit_freq[filter_lit]++;

			uint32_t prev_lits;
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs);

				*pDst_codes++ = (uint64_t)lits << 8;

				lit_freq[lits & 0xFF]++;
				lit_freq[(lits >> 8) & 0xFF]++;
				lit_freq[(lits >> 16) & 0xFF]++;
				lit_freq[lits >> 24]++;

				src_ofs += 4;
				
				prev_lits = lits;
			}

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs);

				if (lits == prev_lits)
				{
					uint32_t match_len = 4;
					uint32_t max_match_len = minimum<int>(252, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						if (READ_LE32(pSrc + src_ofs + match_len) != lits)
							break;
						match_len += 4;
					}
										
					*pDst_codes++ = match_len - 1;

					uint32_t adj_match_len = match_len - 3;

					lit_freq[g_defl_len_sym[adj_match_len]]++;
					
					src_ofs += match_len;
				}
				else
				{
					*pDst_codes++ = (uint64_t)lits << 8;

					lit_freq[lits & 0xFF]++;
					lit_freq[(lits >> 8) & 0xFF]++;
					lit_freq[(lits >> 16) & 0xFF]++;
					lit_freq[lits >> 24]++;
					
					prev_lits = lits;

					src_ofs += 4;
				}

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);
		const uint32_t total_codes = (uint32_t)(pDst_codes - codes.data());
		assert(total_codes <= codes.size());
						
		defl_huff dh;
		
		lit_freq[256] = 1;

		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_0, lit_freq, &dh.m_huff_count[0][0]);
		
		memset(&dh.m_huff_count[1][0], 0, sizeof(dh.m_huff_count[1][0]) * DEFL_MAX_HUFF_SYMBOLS_1);
		dh.m_huff_count[1][dist_sym] = 1;

		if (!defl_start_dynamic_block(&dh, pDst, dst_ofs, dst_buf_size, bit_buf, bit_buf_size))
			return 0;

		assert(bit_buf_size <= 7);
		assert(dh.m_huff_codes[1][dist_sym] == 0 && dh.m_huff_code_sizes[1][dist_sym] == 1);

		for (uint32_t i = 0; i < total_codes; i++)
		{
			uint64_t c = codes[i];

			uint32_t c_type = (uint32_t)(c & 0xFF);
			if (c_type == 0)
			{
				uint32_t lits = (uint32_t)(c >> 8);

				PUT_BITS_CZ(dh.m_huff_codes[0][lits & 0xFF], dh.m_huff_code_sizes[0][lits & 0xFF]);
				lits >>= 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][lits & 0xFF], dh.m_huff_code_sizes[0][lits & 0xFF]);
				lits >>= 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][lits & 0xFF], dh.m_huff_code_sizes[0][lits & 0xFF]);
				lits >>= 8;

				if (bit_buf_size >= 49)
				{
					PUT_BITS_FLUSH;
				}

				PUT_BITS_CZ(dh.m_huff_codes[0][lits], dh.m_huff_code_sizes[0][lits]);
			}
			else if (c_type == 1)
			{
				uint32_t lit = (uint32_t)(c >> 8);
				PUT_BITS_CZ(dh.m_huff_codes[0][lit], dh.m_huff_code_sizes[0][lit]);
			}
			else
			{
				uint32_t match_len = c_type + 1;

				uint32_t adj_match_len = match_len - 3;
				
				PUT_BITS_CZ(dh.m_huff_codes[0][g_defl_len_sym[adj_match_len]], dh.m_huff_code_sizes[0][g_defl_len_sym[adj_match_len]]);
				PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], g_defl_len_extra[adj_match_len] + 1); // up to 6 bits, +1 for the match distance Huff code which is always 0

				// no need to write the distance code, it's always 0
			}

			// up to 55 bits
			PUT_BITS_FLUSH;
		}

		PUT_BITS_CZ(dh.m_huff_codes[0][256], dh.m_huff_code_sizes[0][256]);

		PUT_BITS_FORCE_FLUSH;

		// Write zlib adler32
		for (uint32_t i = 0; i < 4; i++)
		{
			if ((dst_ofs + 1) > dst_buf_size)
				return 0;
			*(uint8_t*)(pDst + dst_ofs) = (uint8_t)(src_adler32 >> 24);
			dst_ofs++;

			src_adler32 <<= 8;
		}

		return dst_ofs;
	}

	static uint32_t pixel_deflate_dyn_4_rle_one_pass(
		const uint8_t* pImg, uint32_t w, uint32_t h,
		uint8_t* pDst, uint32_t dst_buf_size)
	{
		const uint32_t bpl = 1 + w * 4;

		if (dst_buf_size < sizeof(g_dyn_huff_4))
			return false;
		memcpy(pDst, g_dyn_huff_4, sizeof(g_dyn_huff_4));
		uint32_t dst_ofs = sizeof(g_dyn_huff_4);

		uint64_t bit_buf = DYN_HUFF_4_BITBUF;
		int bit_buf_size = DYN_HUFF_4_BITBUF_SIZE;

		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			PUT_BITS_CZ(g_dyn_huff_4_codes[filter_lit].m_code, g_dyn_huff_4_codes[filter_lit].m_code_size);

			PUT_BITS_FLUSH;

			uint32_t prev_lits;
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs);

				PUT_BITS_CZ(g_dyn_huff_4_codes[lits & 0xFF].m_code, g_dyn_huff_4_codes[lits & 0xFF].m_code_size);
				PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 8) & 0xFF].m_code, g_dyn_huff_4_codes[(lits >> 8) & 0xFF].m_code_size);
				PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 16) & 0xFF].m_code, g_dyn_huff_4_codes[(lits >> 16) & 0xFF].m_code_size);

				if (bit_buf_size >= 49)
				{
					PUT_BITS_FLUSH;
				}
				
				PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 24)].m_code, g_dyn_huff_4_codes[(lits >> 24)].m_code_size);

				src_ofs += 4;
				
				prev_lits = lits;
			}

			PUT_BITS_FLUSH;

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs);
								
				if (lits == prev_lits)
				{
					uint32_t match_len = 4;
					uint32_t max_match_len = minimum<int>(252, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						if (READ_LE32(pSrc + src_ofs + match_len) != lits)
							break;
						match_len += 4;
					}

					uint32_t adj_match_len = match_len - 3;

					const uint32_t match_code_bits = g_dyn_huff_4_codes[g_defl_len_sym[adj_match_len]].m_code_size;
					const uint32_t len_extra_bits = g_defl_len_extra[adj_match_len];

					if (match_len == 4)
					{
						// This check is optional - see if just encoding 4 literals would be cheaper than using a short match.
						uint32_t lit_bits = g_dyn_huff_4_codes[lits & 0xFF].m_code_size + g_dyn_huff_4_codes[(lits >> 8) & 0xFF].m_code_size + 
							g_dyn_huff_4_codes[(lits >> 16) & 0xFF].m_code_size + g_dyn_huff_4_codes[(lits >> 24)].m_code_size;
						
						if ((match_code_bits + len_extra_bits + 1) > lit_bits)
							goto do_literals;
					}

					PUT_BITS_CZ(g_dyn_huff_4_codes[g_defl_len_sym[adj_match_len]].m_code, match_code_bits);
					PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], len_extra_bits + 1); // up to 6 bits, +1 for the match distance Huff code which is always 0

					src_ofs += match_len;
				}
				else
				{
do_literals:
					PUT_BITS_CZ(g_dyn_huff_4_codes[lits & 0xFF].m_code, g_dyn_huff_4_codes[lits & 0xFF].m_code_size);
					PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 8) & 0xFF].m_code, g_dyn_huff_4_codes[(lits >> 8) & 0xFF].m_code_size);
					PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 16) & 0xFF].m_code, g_dyn_huff_4_codes[(lits >> 16) & 0xFF].m_code_size);

					if (bit_buf_size >= 49)
					{
						PUT_BITS_FLUSH;
					}

					PUT_BITS_CZ(g_dyn_huff_4_codes[(lits >> 24)].m_code, g_dyn_huff_4_codes[(lits >> 24)].m_code_size);

					src_ofs += 4;
					
					prev_lits = lits;
				}

				PUT_BITS_FLUSH;

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);

		assert(bit_buf_size <= 7);

		PUT_BITS_CZ(g_dyn_huff_4_codes[256].m_code, g_dyn_huff_4_codes[256].m_code_size);

		PUT_BITS_FORCE_FLUSH;

		// Write zlib adler32
		for (uint32_t i = 0; i < 4; i++)
		{
			if ((dst_ofs + 1) > dst_buf_size)
				return 0;
			*(uint8_t*)(pDst + dst_ofs) = (uint8_t)(src_adler32 >> 24);
			dst_ofs++;

			src_adler32 <<= 8;
		}

		return dst_ofs;
	}

	static void vector_append(std::vector<uint8_t>& buf, const void* pData, size_t len)
	{
		if (len)
		{
			size_t l = buf.size();
			buf.resize(l + len);
			memcpy(buf.data() + l, pData, len);
		}
	}
		
	static void apply_filter(uint32_t filter, int w, int h, uint32_t num_chans, uint32_t bpl, const uint8_t* pSrc, const uint8_t* pPrev_src, uint8_t* pDst)
	{
		(void)h;

		switch (filter)
		{
		case 0:
		{
			*pDst++ = 0;

			memcpy(pDst, pSrc, bpl);
			break;
		}
		case 2:
		{
			assert(pPrev_src);

			// Previous scanline
			*pDst++ = 2;

			if (num_chans == 3)
			{
				for (uint32_t x = 0; x < (uint32_t)w; x++)
				{
					pDst[0] = (uint8_t)(pSrc[0] - pPrev_src[0]);
					pDst[1] = (uint8_t)(pSrc[1] - pPrev_src[1]);
					pDst[2] = (uint8_t)(pSrc[2] - pPrev_src[2]);

					pSrc += 3;
					pPrev_src += 3;
					pDst += 3;
				}
			}
			else
			{
				for (uint32_t x = 0; x < (uint32_t)w; x++)
				{
					pDst[0] = (uint8_t)(pSrc[0] - pPrev_src[0]);
					pDst[1] = (uint8_t)(pSrc[1] - pPrev_src[1]);
					pDst[2] = (uint8_t)(pSrc[2] - pPrev_src[2]);
					pDst[3] = (uint8_t)(pSrc[3] - pPrev_src[3]);

					pSrc += 4;
					pPrev_src += 4;
					pDst += 4;
				}
			}
			break;
		}
		default:
			assert(0);
			break;
		}
	}

	bool fpng_encode_image_to_memory(const void* pImage, uint32_t w, uint32_t h, uint32_t num_chans, std::vector<uint8_t>& out_buf, uint32_t flags)
	{
		if ((w < 1) || (h < 1) || (w * h > UINT32_MAX) || (w > FPNG_MAX_SUPPORTED_DIM) || (h > FPNG_MAX_SUPPORTED_DIM))
		{
			assert(0);
			return false;
		}

		if ((num_chans != 3) && (num_chans != 4))
		{
			assert(0);
			return false;
		}

		int i, bpl = w * num_chans;
		uint32_t y;

		std::vector<uint8_t> temp_buf;
		temp_buf.resize(((bpl + 1) * h + 7) & ~7);
		uint32_t temp_buf_ofs = 0;

		for (y = 0; y < h; ++y)
		{
			const uint8_t* pSrc = (uint8_t*)pImage + y * bpl;
			const uint8_t* pPrev_src = y ? ((uint8_t*)pImage + (y - 1) * bpl) : nullptr;

			uint8_t* pDst = &temp_buf[temp_buf_ofs];

			apply_filter(y ? 2 : 0, w, h, num_chans, bpl, pSrc, pPrev_src, pDst);

			temp_buf_ofs += 1 + bpl;
		}

		const uint32_t PNG_HEADER_SIZE = 58;
				
		uint32_t out_ofs = PNG_HEADER_SIZE;
				
		out_buf.resize((out_ofs + (bpl + 1) * h + 7) & ~7);

		uint32_t defl_size = 0;
		if ((flags & FPNG_FORCE_UNCOMPRESSED) == 0)
		{
			if (num_chans == 3)
			{
				if (flags & FPNG_ENCODE_SLOWER)
					defl_size = pixel_deflate_dyn_3_rle(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);
				else
					defl_size = pixel_deflate_dyn_3_rle_one_pass(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);
			}
			else
			{
				if (flags & FPNG_ENCODE_SLOWER)
					defl_size = pixel_deflate_dyn_4_rle(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);
				else
					defl_size = pixel_deflate_dyn_4_rle_one_pass(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);
			}
		}

		uint32_t zlib_size = defl_size;
		
		if (!defl_size)
		{
			// Dynamic block failed to compress - fall back to uncompressed blocks, filter 0.

			temp_buf_ofs = 0;

			for (y = 0; y < h; ++y)
			{
				const uint8_t* pSrc = (uint8_t*)pImage + y * bpl;

				uint8_t* pDst = &temp_buf[temp_buf_ofs];

				apply_filter(0, w, h, num_chans, bpl, pSrc, nullptr, pDst);

				temp_buf_ofs += 1 + bpl;
			}

			assert(temp_buf_ofs <= temp_buf.size());
						
			out_buf.resize(out_ofs + 6 + temp_buf_ofs + ((temp_buf_ofs + 65534) / 65535) * 5);

			uint32_t raw_size = write_raw_block(temp_buf.data(), (uint32_t)temp_buf_ofs, out_buf.data() + out_ofs, (uint32_t)out_buf.size() - out_ofs);
			if (!raw_size)
			{
				// Somehow we miscomputed the size of the output buffer.
				assert(0);
				return false;
			}

			zlib_size = raw_size;
		}
		
		assert((out_ofs + zlib_size) <= out_buf.size());

		out_buf.resize(out_ofs + zlib_size);

		const uint32_t idat_len = (uint32_t)out_buf.size() - PNG_HEADER_SIZE;

		// Write real PNG header, fdEC chunk, and the beginning of the IDAT chunk
		{
			static const uint8_t s_color_type[] = { 0x00, 0x00, 0x04, 0x02, 0x06 };

			uint8_t pnghdr[58] = { 
				0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,   // PNG sig
				0x00,0x00,0x00,0x0d, 'I','H','D','R',  // IHDR chunk len, type
			    0,0,(uint8_t)(w >> 8),(uint8_t)w, // width
				0,0,(uint8_t)(h >> 8),(uint8_t)h, // height
				8,   //bit_depth
				s_color_type[num_chans], // color_type
				0, // compression
				0, // filter
				0, // interlace
				0, 0, 0, 0, // IHDR crc32
				0, 0, 0, 5, 'f', 'd', 'E', 'C', 82, 36, 147, 227, FPNG_FDEC_VERSION,   0xE5, 0xAB, 0x62, 0x99, // our custom private, ancillary, do not copy, fdEC chunk
			  (uint8_t)(idat_len >> 24),(uint8_t)(idat_len >> 16),(uint8_t)(idat_len >> 8),(uint8_t)idat_len, 'I','D','A','T' // IDATA chunk len, type
			}; 

			// Compute IHDR CRC32
			uint32_t c = (uint32_t)fpng_crc32(FPNG_CRC32_INIT, pnghdr + 12, 17);
			for (i = 0; i < 4; ++i, c <<= 8)
				((uint8_t*)(pnghdr + 29))[i] = (uint8_t)(c >> 24);

			memcpy(out_buf.data(), pnghdr, PNG_HEADER_SIZE);
		}

		// Write IDAT chunk's CRC32 and a 0 length IEND chunk
		vector_append(out_buf, "\0\0\0\0\0\0\0\0\x49\x45\x4e\x44\xae\x42\x60\x82", 16); // IDAT CRC32, followed by the IEND chunk

		// Compute IDAT crc32
		uint32_t c = (uint32_t)fpng_crc32(FPNG_CRC32_INIT, out_buf.data() + PNG_HEADER_SIZE - 4, idat_len + 4);
		//uint32_t c = 0;

		for (i = 0; i < 4; ++i, c <<= 8)
			(out_buf.data() + out_buf.size() - 16)[i] = (uint8_t)(c >> 24);
				
		return true;
	}

#ifndef FPNG_NO_STDIO
	bool fpng_encode_image_to_file(const char* pFilename, const void* pImage, uint32_t w, uint32_t h, uint32_t num_chans, uint32_t flags)
	{
		std::vector<uint8_t> out_buf;
		if (!fpng_encode_image_to_memory(pImage, w, h, num_chans, out_buf, flags))
			return false;

		FILE* pFile = nullptr;
#ifdef _MSC_VER
		fopen_s(&pFile, pFilename, "wb");
#else
		pFile = fopen(pFilename, "wb");
#endif
		if (!pFile)
			return false;

		if (fwrite(out_buf.data(), 1, out_buf.size(), pFile) != out_buf.size())
		{
			fclose(pFile);
			return false;
		}

		return (fclose(pFile) != EOF);
	}
#endif

	// Decompression

	const uint32_t FPNG_DECODER_TABLE_BITS = 12;
	const uint32_t FPNG_DECODER_TABLE_SIZE = 1 << FPNG_DECODER_TABLE_BITS;

	static bool build_decoder_table(uint32_t num_syms, uint8_t* pCode_sizes, uint32_t* pTable)
	{
		uint32_t num_codes[16];

		memset(num_codes, 0, sizeof(num_codes));
		for (uint32_t i = 0; i < num_syms; i++)
		{
			assert(pCode_sizes[i] <= FPNG_DECODER_TABLE_SIZE);
			num_codes[pCode_sizes[i]]++;
		}

		uint32_t next_code[17];
		next_code[0] = next_code[1] = 0;
		uint32_t total = 0;
		for (uint32_t i = 1; i <= 15; i++)
			next_code[i + 1] = (uint32_t)(total = ((total + ((uint32_t)num_codes[i])) << 1));

		if (total != 0x10000)
		{
			uint32_t j = 0;

			for (uint32_t i = 15; i != 0; i--)
				if ((j += num_codes[i]) > 1)
					return false;
			
			if (j != 1)
				return false;
		}

		uint32_t rev_codes[DEFL_MAX_HUFF_SYMBOLS];

		for (uint32_t i = 0; i < num_syms; i++)
			rev_codes[i] = next_code[pCode_sizes[i]]++;

		memset(pTable, 0, sizeof(uint32_t) * FPNG_DECODER_TABLE_SIZE);

		for (uint32_t i = 0; i < num_syms; i++)
		{
			const uint32_t code_size = pCode_sizes[i];
			if (!code_size)
				continue;

			uint32_t old_code = rev_codes[i], new_code = 0;
			for (uint32_t j = code_size; j != 0; j--)
			{
				new_code = (new_code << 1) | (old_code & 1);
				old_code >>= 1;
			}

			uint32_t j = 1 << code_size;

			while (new_code < FPNG_DECODER_TABLE_SIZE)
			{
				pTable[new_code] = i | (code_size << 9);
				new_code += j;
			}
		}

		return true;
	}

	static const uint8_t g_match_len_valid_3[259] = 
	{
		0,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,
		0,0,1,0,0,1
	};

	static const uint16_t g_run_len3_to_4[259] = 
	{
		0,
		0, 0, 4, 0, 0, 8, 0, 0, 12, 0, 0, 16, 0, 0, 20, 0, 0, 24, 0, 0, 28, 0, 0,
		32, 0, 0, 36, 0, 0, 40, 0, 0, 44, 0, 0, 48, 0, 0, 52, 0, 0, 56, 0, 0,
		60, 0, 0, 64, 0, 0, 68, 0, 0, 72, 0, 0, 76, 0, 0, 80, 0, 0, 84, 0, 0,
		88, 0, 0, 92, 0, 0, 96, 0, 0, 100, 0, 0, 104, 0, 0, 108, 0, 0, 112, 0, 0,
		116, 0, 0, 120, 0, 0, 124, 0, 0, 128, 0, 0, 132, 0, 0, 136, 0, 0, 140, 0, 0,
		144, 0, 0, 148, 0, 0, 152, 0, 0, 156, 0, 0, 160, 0, 0, 164, 0, 0, 168, 0, 0,
		172, 0, 0, 176, 0, 0, 180, 0, 0, 184, 0, 0, 188, 0, 0, 192, 0, 0, 196, 0, 0,
		200, 0, 0, 204, 0, 0, 208, 0, 0, 212, 0, 0, 216, 0, 0, 220, 0, 0, 224, 0, 0,
		228, 0, 0, 232, 0, 0, 236, 0, 0, 240, 0, 0, 244, 0, 0, 248, 0, 0, 252, 0, 0,
		256, 0, 0, 260, 0, 0, 264, 0, 0, 268, 0, 0, 272, 0, 0, 276, 0, 0, 280, 0, 0,
		284, 0, 0, 288, 0, 0, 292, 0, 0, 296, 0, 0, 300, 0, 0, 304, 0, 0, 308, 0, 0,
		312, 0, 0, 316, 0, 0, 320, 0, 0, 324, 0, 0, 328, 0, 0, 332, 0, 0, 336, 0, 0,
		340, 0, 0, 
		344,
	};

	static const int s_length_extra[] = { 0,0,0,0, 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3, 4,4,4,4, 5,5,5,5, 0,    0,0 };
	static const int s_length_range[] = { 3,4,5,6, 7,8,9,10, 11,13,15,17, 19,23,27,31, 35,43,51,59, 67,83,99,115, 131,163,195,227, 258,    0,0 };

#define ENSURE_32BITS() do { \
	if (bit_buf_size < 32) { \
		if ((src_ofs + 4) > src_len) return false; \
		bit_buf |= ((uint64_t)READ_LE32(pSrc + src_ofs)) << bit_buf_size; \
		src_ofs += 4; bit_buf_size += 32; } \
	} while(0)

#define GET_BITS(b, ll) do { \
	uint32_t l = ll; assert(l && (l <= 32)); \
	b = (uint32_t)(bit_buf & g_bitmasks[l]); \
	bit_buf >>= l; \
	bit_buf_size -= l; \
	ENSURE_32BITS(); \
	} while(0)

#define SKIP_BITS(ll) do { \
	uint32_t l = ll; assert(l <= 32); \
	bit_buf >>= l; \
	bit_buf_size -= l; \
	ENSURE_32BITS(); \
	} while(0)

#define GET_BITS_NE(b, ll) do { \
	uint32_t l = ll; assert(l && (l <= 32) && (bit_buf_size >= l)); \
	b = (uint32_t)(bit_buf & g_bitmasks[l]); \
	bit_buf >>= l; \
	bit_buf_size -= l; \
	} while(0)

#define SKIP_BITS_NE(ll) do { \
	uint32_t l = ll; assert(l <= 32 && (bit_buf_size >= l)); \
	bit_buf >>= l; \
	bit_buf_size -= l; \
	} while(0)

	static bool prepare_dynamic_block(
		const uint8_t* pSrc, uint32_t src_len, uint32_t& src_ofs,
		uint32_t& bit_buf_size, uint64_t& bit_buf,
		uint32_t* pLit_table, uint32_t num_chans)
	{
		static const uint8_t s_bit_length_order[] = { 16, 17, 18, 0, 8,  7,  9, 6, 10,  5, 11, 4, 12,  3, 13, 2, 14,  1, 15 };

		uint32_t num_lit_codes, num_dist_codes, num_clen_codes;

		GET_BITS(num_lit_codes, 5);
		num_lit_codes += 257;

		GET_BITS(num_dist_codes, 5);
		num_dist_codes += 1;
		if (num_dist_codes != num_chans)
			return false;

		uint32_t total_codes = num_lit_codes + num_dist_codes;
		if (total_codes > (DEFL_MAX_HUFF_SYMBOLS_0 + DEFL_MAX_HUFF_SYMBOLS_1))
			return false;

		uint8_t code_sizes[DEFL_MAX_HUFF_SYMBOLS_0 + DEFL_MAX_HUFF_SYMBOLS_1];
		memset(code_sizes, 0, sizeof(code_sizes));

		GET_BITS(num_clen_codes, 4);
		num_clen_codes += 4;

		uint8_t clen_codesizes[DEFL_MAX_HUFF_SYMBOLS_2];
		memset(clen_codesizes, 0, sizeof(clen_codesizes));

		for (uint32_t i = 0; i < num_clen_codes; i++)
		{
			uint32_t len = 0;
			GET_BITS(len, 3);
			clen_codesizes[s_bit_length_order[i]] = (uint8_t)len;
		}

		uint32_t clen_table[FPNG_DECODER_TABLE_SIZE];
		if (!build_decoder_table(DEFL_MAX_HUFF_SYMBOLS_2, clen_codesizes, clen_table))
			return false;

		uint32_t min_code_size = 15;

		for (uint32_t cur_code = 0; cur_code < total_codes; )
		{
			uint32_t sym = clen_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
			uint32_t sym_len = sym >> 9;
			if (!sym_len)
				return false;
			SKIP_BITS(sym_len);
			sym &= 511;
						
			if (sym <= 15)
			{
				// Can't be a fpng Huffman table
				if (sym > FPNG_DECODER_TABLE_BITS)
					return false;

				if (sym)
					min_code_size = minimum(min_code_size, sym);

				code_sizes[cur_code++] = (uint8_t)sym;
				continue;
			}

			uint32_t rep_len = 0, rep_code_size = 0;

			switch (sym)
			{
			case 16:
			{
				GET_BITS(rep_len, 2);
				rep_len += 3;
				if (!cur_code)
					return false;
				rep_code_size = code_sizes[cur_code - 1];
				break;
			}
			case 17:
			{
				GET_BITS(rep_len, 3);
				rep_len += 3;
				rep_code_size = 0;
				break;
			}
			case 18:
			{
				GET_BITS(rep_len, 7);
				rep_len += 11;
				rep_code_size = 0;
				break;
			}
			}

			if ((cur_code + rep_len) > total_codes)
				return false;

			for (; rep_len; rep_len--)
				code_sizes[cur_code++] = (uint8_t)rep_code_size;
		}

		uint8_t lit_codesizes[DEFL_MAX_HUFF_SYMBOLS_0];

		memcpy(lit_codesizes, code_sizes, num_lit_codes);
		memset(lit_codesizes + num_lit_codes, 0, DEFL_MAX_HUFF_SYMBOLS_0 - num_lit_codes);

		uint32_t total_valid_distcodes = 0;
		for (uint32_t i = 0; i < num_dist_codes; i++)
			total_valid_distcodes += code_sizes[num_lit_codes + i];
		if (total_valid_distcodes != 1)
			return false;

		if (code_sizes[num_lit_codes + (num_chans - 1)] != 1)
			return false;
						
		if (!build_decoder_table(num_lit_codes, lit_codesizes, pLit_table))
			return false;

		// Add next symbol to decoder table, when it fits
		for (uint32_t i = 0; i < FPNG_DECODER_TABLE_SIZE; i++)
		{
			uint32_t sym = pLit_table[i] & 511;
			if (sym >= 256)
				continue;

			uint32_t sym_bits = (pLit_table[i] >> 9) & 15;
			if (!sym_bits)
				continue;
			assert(sym_bits <= FPNG_DECODER_TABLE_BITS);

			uint32_t bits_left = FPNG_DECODER_TABLE_BITS - sym_bits;
			if (bits_left < min_code_size)
				continue;

			uint32_t next_bits = i >> sym_bits;
			uint32_t next_sym = pLit_table[next_bits] & 511;
			uint32_t next_sym_bits = (pLit_table[next_bits] >> 9) & 15;
			if ((!next_sym_bits) || (bits_left < next_sym_bits))
				continue;

			pLit_table[i] |= (next_sym << 16) | (next_sym_bits << (16 + 9));
		}

		return true;
	}
		
	static bool fpng_pixel_zlib_raw_decompress(
		const uint8_t* pSrc, uint32_t src_len, uint32_t zlib_len,
		uint8_t* pDst, uint32_t w, uint32_t h,
		uint32_t src_chans, uint32_t dst_chans)
	{
		assert((src_chans == 3) || (src_chans == 4));
		assert((dst_chans == 3) || (dst_chans == 4));
		
		const uint32_t src_bpl = w * src_chans;
		const uint32_t dst_bpl = w * dst_chans;
		const uint32_t dst_len = dst_bpl * h;

		uint32_t src_ofs = 2;
		uint32_t dst_ofs = 0;
		uint32_t raster_ofs = 0;
		uint32_t comp_ofs = 0;

		for (; ; )
		{
			if ((src_ofs + 1) > src_len)
				return false;

			const bool bfinal = (pSrc[src_ofs] & 1) != 0;
			const uint32_t btype = (pSrc[src_ofs] >> 1) & 3;
			if (btype != 0)
				return false;

			src_ofs++;

			if ((src_ofs + 4) > src_len)
				return false;
			uint32_t len = pSrc[src_ofs + 0] | (pSrc[src_ofs + 1] << 8);
			uint32_t nlen = pSrc[src_ofs + 2] | (pSrc[src_ofs + 3] << 8);
			src_ofs += 4;

			if (len != (~nlen & 0xFFFF))
				return false;

			if ((src_ofs + len) > src_len)
				return false;

			// Raw blocks are a relatively uncommon case so this isn't well optimized.
			// Supports 3->4 and 4->3 byte/pixel conversion.
			for (uint32_t i = 0; i < len; i++)
			{
				uint32_t c = pSrc[src_ofs + i];

				if (!raster_ofs)
				{
					// Check filter type
					if (c != 0)
						return false;
					
					assert(!comp_ofs);
				}
				else
				{
					if (comp_ofs < dst_chans)
					{
						if (dst_ofs == dst_len)
							return false;

						pDst[dst_ofs++] = (uint8_t)c;
					}
					
					if (++comp_ofs == src_chans)
					{
						if (dst_chans > src_chans)
						{
							if (dst_ofs == dst_len)
								return false;

							pDst[dst_ofs++] = (uint8_t)0xFF;
						}

						comp_ofs = 0;
					}
				}

				if (++raster_ofs == (src_bpl + 1))
				{
					assert(!comp_ofs);
					raster_ofs = 0;
				}
			}

			src_ofs += len;

			if (bfinal)
				break;
		}

		if (comp_ofs != 0)
			return false;

		// Check for zlib adler32
		if ((src_ofs + 4) != zlib_len)
			return false;

		return (dst_ofs == dst_len);
	}
	
	template<uint32_t dst_comps>
	static bool fpng_pixel_zlib_decompress_3(
		const uint8_t* pSrc, uint32_t src_len, uint32_t zlib_len,
		uint8_t* pDst, uint32_t w, uint32_t h)
	{
		assert(src_len >= (zlib_len + 4));

		const uint32_t dst_bpl = w * dst_comps;
		//const uint32_t dst_len = dst_bpl * h;

		if (zlib_len < 7)
			return false;

		// check zlib header
		if ((pSrc[0] != 0x78) || (pSrc[1] != 0x01))
			return false;

		uint32_t src_ofs = 2;
		
		if ((pSrc[src_ofs] & 6) == 0)
			return fpng_pixel_zlib_raw_decompress(pSrc, src_len, zlib_len, pDst, w, h, 3, dst_comps);
		
		if ((src_ofs + 4) > src_len)
			return false;
		uint64_t bit_buf = READ_LE32(pSrc + src_ofs);
		src_ofs += 4;

		uint32_t bit_buf_size = 32;

		uint32_t bfinal, btype;
		GET_BITS(bfinal, 1);
		GET_BITS(btype, 2);

		// Must be the final block or it's not valid, and type=1 (dynamic)
		if ((bfinal != 1) || (btype != 2))
			return false;
		
		uint32_t lit_table[FPNG_DECODER_TABLE_SIZE];
		if (!prepare_dynamic_block(pSrc, src_len, src_ofs, bit_buf_size, bit_buf, lit_table, 3))
			return false;

		const uint8_t* pPrev_scanline = nullptr;
		uint8_t* pCur_scanline = pDst;

		for (uint32_t y = 0; y < h; y++)
		{
			// At start of PNG scanline, so read the filter literal
			assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
			uint32_t filter = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
			uint32_t filter_len = (filter >> 9) & 15;
			if (!filter_len)
				return false;
			SKIP_BITS(filter_len);
			filter &= 511;

			uint32_t expected_filter = (y ? 2 : 0);
			if (filter != expected_filter)
				return false;

			uint32_t x_ofs = 0;
			uint8_t prev_delta_r = 0, prev_delta_g = 0, prev_delta_b = 0;
			do
			{
				assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
				uint32_t lit0_tab = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
				
				uint32_t lit0 = lit0_tab;
				uint32_t lit0_len = (lit0_tab >> 9) & 15;
				if (!lit0_len)
					return false;
				SKIP_BITS(lit0_len);

				if (lit0 & 256)
				{
					lit0 &= 511;

					// Can't be EOB - we still have more pixels to decompress.
					if (lit0 == 256)
						return false;

					// Must be an RLE match against the previous pixel.
					uint32_t run_len = s_length_range[lit0 - 257];
					if (lit0 >= 265)
					{
						uint32_t e;
						GET_BITS_NE(e, s_length_extra[lit0 - 257]);

						run_len += e;
					}
					
					// Skip match distance - it's always the same (3)
					SKIP_BITS_NE(1);

					// Matches must always be a multiple of 3/4 bytes
					assert((run_len % 3) == 0);
																				
					if (dst_comps == 4)
					{
						const uint32_t x_ofs_end = x_ofs + g_run_len3_to_4[run_len];
						
						// Check for valid run lengths
						if (x_ofs == x_ofs_end)
							return false;

						// Matches cannot cross scanlines.
						if (x_ofs_end > dst_bpl)
							return false;

						if (pPrev_scanline)
						{
							if ((prev_delta_r | prev_delta_g | prev_delta_b) == 0)
							{
								memcpy(pCur_scanline + x_ofs, pPrev_scanline + x_ofs, x_ofs_end - x_ofs);
								x_ofs = x_ofs_end;
							}
							else
							{
								do
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + prev_delta_r);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + prev_delta_g);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + prev_delta_b);
									pCur_scanline[x_ofs + 3] = 0xFF;
									x_ofs += 4;
								} while (x_ofs < x_ofs_end);
							}
						}
						else
						{
							do
							{
								pCur_scanline[x_ofs] = prev_delta_r;
								pCur_scanline[x_ofs + 1] = prev_delta_g;
								pCur_scanline[x_ofs + 2] = prev_delta_b;
								pCur_scanline[x_ofs + 3] = 0xFF;
								x_ofs += 4;
							} while (x_ofs < x_ofs_end);
						}
					}
					else
					{
						// Check for valid run lengths
						if (!g_match_len_valid_3[run_len])
							return false;

						const uint32_t x_ofs_end = x_ofs + run_len;

						// Matches cannot cross scanlines.
						if (x_ofs_end > dst_bpl)
							return false;

						if (pPrev_scanline)
						{
							if ((prev_delta_r | prev_delta_g | prev_delta_b) == 0)
							{
								memcpy(pCur_scanline + x_ofs, pPrev_scanline + x_ofs, run_len);
								x_ofs = x_ofs_end;
							}
							else
							{
								do
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + prev_delta_r);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + prev_delta_g);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + prev_delta_b);
									x_ofs += 3;
								} while (x_ofs < x_ofs_end);
							}
						}
						else
						{
							do
							{
								pCur_scanline[x_ofs] = prev_delta_r;
								pCur_scanline[x_ofs + 1] = prev_delta_g;
								pCur_scanline[x_ofs + 2] = prev_delta_b;
								x_ofs += 3;
							} while (x_ofs < x_ofs_end);
						}
					}
				}
				else
				{
					uint32_t lit1, lit2;

					uint32_t lit1_spec_len = (lit0_tab >> (16 + 9));
					uint32_t lit2_len;
					if (lit1_spec_len)
					{
						lit1 = (lit0_tab >> 16) & 511;
						SKIP_BITS_NE(lit1_spec_len);

						assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
						lit2 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
						lit2_len = (lit2 >> 9) & 15;
						if (!lit2_len)
							return false;
					}
					else
					{
						assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
						lit1 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
						uint32_t lit1_len = (lit1 >> 9) & 15;
						if (!lit1_len)
							return false;
						SKIP_BITS_NE(lit1_len);

						lit2_len = (lit1 >> (16 + 9));
						if (lit2_len)
							lit2 = lit1 >> 16;
						else
						{
							assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
							lit2 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
							lit2_len = (lit2 >> 9) & 15;
							if (!lit2_len)
								return false;
						}
					}

					SKIP_BITS(lit2_len);
					
					// Check for matches
					if ((lit1 | lit2) & 256)
						return false;

					if (dst_comps == 4)
					{
						if (pPrev_scanline)
						{
							pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
							pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
							pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
							pCur_scanline[x_ofs + 3] = 0xFF;
						}
						else
						{
							pCur_scanline[x_ofs] = (uint8_t)lit0;
							pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
							pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
							pCur_scanline[x_ofs + 3] = 0xFF;
						}
						x_ofs += 4;
					}
					else
					{
						if (pPrev_scanline)
						{
							pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
							pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
							pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
						}
						else
						{
							pCur_scanline[x_ofs] = (uint8_t)lit0;
							pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
							pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
						}
						x_ofs += 3;
					}

					prev_delta_r = (uint8_t)lit0;
					prev_delta_g = (uint8_t)lit1;
					prev_delta_b = (uint8_t)lit2;
										
					// See if we can decode one more pixel.
					uint32_t spec_next_len0_len = lit2 >> (16 + 9);
					if ((spec_next_len0_len) && (x_ofs < dst_bpl))
					{
						lit0 = (lit2 >> 16) & 511;
						if (lit0 < 256)
						{
							SKIP_BITS_NE(spec_next_len0_len);

							assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
							lit1 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
							uint32_t lit1_len = (lit1 >> 9) & 15;
							if (!lit1_len)
								return false;
							SKIP_BITS(lit1_len);

							lit2_len = (lit1 >> (16 + 9));
							if (lit2_len)
								lit2 = lit1 >> 16;
							else
							{
								assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
								lit2 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
								lit2_len = (lit2 >> 9) & 15;
								if (!lit2_len)
									return false;
							}

							SKIP_BITS_NE(lit2_len);

							// Check for matches
							if ((lit1 | lit2) & 256)
								return false;
					
							if (dst_comps == 4)
							{
								if (pPrev_scanline)
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
									pCur_scanline[x_ofs + 3] = 0xFF;
								}
								else
								{
									pCur_scanline[x_ofs] = (uint8_t)lit0;
									pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
									pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
									pCur_scanline[x_ofs + 3] = 0xFF;
								}
								x_ofs += 4;
							}
							else
							{
								if (pPrev_scanline)
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
								}
								else
								{
									pCur_scanline[x_ofs] = (uint8_t)lit0;
									pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
									pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
								}
								x_ofs += 3;
							}

							prev_delta_r = (uint8_t)lit0;
							prev_delta_g = (uint8_t)lit1;
							prev_delta_b = (uint8_t)lit2;
																				
						} // if (lit0 < 256)

					} // if ((spec_next_len0_len) && (x_ofs < bpl))
				}

			} while (x_ofs < dst_bpl);

			pPrev_scanline = pCur_scanline;
			pCur_scanline += dst_bpl;

		} // y

		// The last symbol should be EOB
		assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
		uint32_t lit0 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
		uint32_t lit0_len = (lit0 >> 9) & 15;
		if (!lit0_len)
			return false;
		lit0 &= 511;
		if (lit0 != 256)
			return false;

		bit_buf_size -= lit0_len;
		bit_buf >>= lit0_len;

		uint32_t align_bits = bit_buf_size & 7;
		bit_buf_size -= align_bits;
		bit_buf >>= align_bits;

		if (src_ofs < (bit_buf_size >> 3))
			return false;
		src_ofs -= (bit_buf_size >> 3);

		// We should be at the very end, because the bit buf reads ahead 32-bits (which contains the zlib adler32).
		if ((src_ofs + 4) != zlib_len)
			return false;

		return true;
	}

	template<uint32_t dst_comps>
	static bool fpng_pixel_zlib_decompress_4(
		const uint8_t* pSrc, uint32_t src_len, uint32_t zlib_len,
		uint8_t* pDst, uint32_t w, uint32_t h)
	{
		assert(src_len >= (zlib_len + 4));

		const uint32_t dst_bpl = w * dst_comps;
		//const uint32_t dst_len = dst_bpl * h;

		if (zlib_len < 7)
			return false;

		// check zlib header
		if ((pSrc[0] != 0x78) || (pSrc[1] != 0x01))
			return false;

		uint32_t src_ofs = 2;

		if ((pSrc[src_ofs] & 6) == 0)
			return fpng_pixel_zlib_raw_decompress(pSrc, src_len, zlib_len, pDst, w, h, 4, dst_comps);

		if ((src_ofs + 4) > src_len)
			return false;
		uint64_t bit_buf = READ_LE32(pSrc + src_ofs);
		src_ofs += 4;

		uint32_t bit_buf_size = 32;

		uint32_t bfinal, btype;
		GET_BITS(bfinal, 1);
		GET_BITS(btype, 2);

		// Must be the final block or it's not valid, and type=1 (dynamic)
		if ((bfinal != 1) || (btype != 2))
			return false;

		uint32_t lit_table[FPNG_DECODER_TABLE_SIZE];
		if (!prepare_dynamic_block(pSrc, src_len, src_ofs, bit_buf_size, bit_buf, lit_table, 4))
			return false;

		const uint8_t* pPrev_scanline = nullptr;
		uint8_t* pCur_scanline = pDst;

		for (uint32_t y = 0; y < h; y++)
		{
			// At start of PNG scanline, so read the filter literal
			assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
			uint32_t filter = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
			uint32_t filter_len = (filter >> 9) & 15;
			if (!filter_len)
				return false;
			SKIP_BITS(filter_len);
			filter &= 511;

			uint32_t expected_filter = (y ? 2 : 0);
			if (filter != expected_filter)
				return false;

			uint32_t x_ofs = 0;
			uint8_t prev_delta_r = 0, prev_delta_g = 0, prev_delta_b = 0, prev_delta_a = 0;
			do
			{
				assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
				uint32_t lit0_tab = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];

				uint32_t lit0 = lit0_tab;
				uint32_t lit0_len = (lit0_tab >> 9) & 15;
				if (!lit0_len)
					return false;
				SKIP_BITS(lit0_len);

				if (lit0 & 256)
				{
					lit0 &= 511;

					// Can't be EOB - we still have more pixels to decompress.
					if (lit0 == 256)
						return false;

					// Must be an RLE match against the previous pixel.
					uint32_t run_len = s_length_range[lit0 - 257];
					if (lit0 >= 265)
					{
						uint32_t e;
						GET_BITS_NE(e, s_length_extra[lit0 - 257]);

						run_len += e;
					}

					// Skip match distance - it's always the same (4)
					SKIP_BITS_NE(1);

					// Matches must always be a multiple of 3/4 bytes
					if (run_len & 3)
						return false;
										
					if (dst_comps == 3)
					{
						const uint32_t run_len3 = (run_len >> 2) * 3;
						const uint32_t x_ofs_end = x_ofs + run_len3;

						// Matches cannot cross scanlines.
						if (x_ofs_end > dst_bpl)
							return false;

						if (pPrev_scanline)
						{
							if ((prev_delta_r | prev_delta_g | prev_delta_b | prev_delta_a) == 0)
							{
								memcpy(pCur_scanline + x_ofs, pPrev_scanline + x_ofs, run_len3);
								x_ofs = x_ofs_end;
							}
							else
							{
								do
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + prev_delta_r);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + prev_delta_g);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + prev_delta_b);
									x_ofs += 3;
								} while (x_ofs < x_ofs_end);
							}
						}
						else
						{
							do
							{
								pCur_scanline[x_ofs] = prev_delta_r;
								pCur_scanline[x_ofs + 1] = prev_delta_g;
								pCur_scanline[x_ofs + 2] = prev_delta_b;
								x_ofs += 3;
							} while (x_ofs < x_ofs_end);
						}
					}
					else
					{
						const uint32_t x_ofs_end = x_ofs + run_len;

						// Matches cannot cross scanlines.
						if (x_ofs_end > dst_bpl)
							return false;

						if (pPrev_scanline)
						{
							if ((prev_delta_r | prev_delta_g | prev_delta_b | prev_delta_a) == 0)
							{
								memcpy(pCur_scanline + x_ofs, pPrev_scanline + x_ofs, run_len);
								x_ofs = x_ofs_end;
							}
							else
							{
								do
								{
									pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + prev_delta_r);
									pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + prev_delta_g);
									pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + prev_delta_b);
									pCur_scanline[x_ofs + 3] = (uint8_t)(pPrev_scanline[x_ofs + 3] + prev_delta_a);
									x_ofs += 4;
								} while (x_ofs < x_ofs_end);
							}
						}
						else
						{
							do
							{
								pCur_scanline[x_ofs] = prev_delta_r;
								pCur_scanline[x_ofs + 1] = prev_delta_g;
								pCur_scanline[x_ofs + 2] = prev_delta_b;
								pCur_scanline[x_ofs + 3] = prev_delta_a;
								x_ofs += 4;
							} while (x_ofs < x_ofs_end);
						}
					}
				}
				else
				{
					uint32_t lit1, lit2;

					uint32_t lit1_spec_len = (lit0_tab >> (16 + 9));
					uint32_t lit2_len;
					if (lit1_spec_len)
					{
						lit1 = (lit0_tab >> 16) & 511;
						SKIP_BITS_NE(lit1_spec_len);

						assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
						lit2 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
						lit2_len = (lit2 >> 9) & 15;
						if (!lit2_len)
							return false;
					}
					else
					{
						assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
						lit1 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
						uint32_t lit1_len = (lit1 >> 9) & 15;
						if (!lit1_len)
							return false;
						SKIP_BITS_NE(lit1_len);

						lit2_len = (lit1 >> (16 + 9));
						if (lit2_len)
							lit2 = lit1 >> 16;
						else
						{
							assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
							lit2 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
							lit2_len = (lit2 >> 9) & 15;
							if (!lit2_len)
								return false;
						}
					}

					uint32_t lit3;
					uint32_t lit3_len = lit2 >> (16 + 9);
					
					if (lit3_len)
					{
						lit3 = (lit2 >> 16);
						SKIP_BITS(lit2_len + lit3_len);
					}
					else
					{
						SKIP_BITS(lit2_len);

						assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
						lit3 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
						lit3_len = (lit3 >> 9) & 15;
						if (!lit3_len)
							return false;

						SKIP_BITS_NE(lit3_len);
					}
										
					// Check for matches
					if ((lit1 | lit2 | lit3) & 256)
						return false;

					if (dst_comps == 3)
					{
						if (pPrev_scanline)
						{
							pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
							pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
							pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
						}
						else
						{
							pCur_scanline[x_ofs] = (uint8_t)lit0;
							pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
							pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
						}

						x_ofs += 3;
					}
					else
					{
						if (pPrev_scanline)
						{
							pCur_scanline[x_ofs] = (uint8_t)(pPrev_scanline[x_ofs] + lit0);
							pCur_scanline[x_ofs + 1] = (uint8_t)(pPrev_scanline[x_ofs + 1] + lit1);
							pCur_scanline[x_ofs + 2] = (uint8_t)(pPrev_scanline[x_ofs + 2] + lit2);
							pCur_scanline[x_ofs + 3] = (uint8_t)(pPrev_scanline[x_ofs + 3] + lit3);
						}
						else
						{
							pCur_scanline[x_ofs] = (uint8_t)lit0;
							pCur_scanline[x_ofs + 1] = (uint8_t)lit1;
							pCur_scanline[x_ofs + 2] = (uint8_t)lit2;
							pCur_scanline[x_ofs + 3] = (uint8_t)lit3;
						}
						
						x_ofs += 4;
					}

					prev_delta_r = (uint8_t)lit0;
					prev_delta_g = (uint8_t)lit1;
					prev_delta_b = (uint8_t)lit2;
					prev_delta_a = (uint8_t)lit3;
				}

			} while (x_ofs < dst_bpl);

			pPrev_scanline = pCur_scanline;
			pCur_scanline += dst_bpl;
		} // y

		// The last symbol should be EOB
		assert(bit_buf_size >= FPNG_DECODER_TABLE_BITS);
		uint32_t lit0 = lit_table[bit_buf & (FPNG_DECODER_TABLE_SIZE - 1)];
		uint32_t lit0_len = (lit0 >> 9) & 15;
		if (!lit0_len)
			return false;
		lit0 &= 511;
		if (lit0 != 256)
			return false;

		bit_buf_size -= lit0_len;
		bit_buf >>= lit0_len;

		uint32_t align_bits = bit_buf_size & 7;
		bit_buf_size -= align_bits;
		bit_buf >>= align_bits;

		if (src_ofs < (bit_buf_size >> 3))
			return false;
		src_ofs -= (bit_buf_size >> 3);

		// We should be at the very end, because the bit buf reads ahead 32-bits (which contains the zlib adler32).
		if ((src_ofs + 4) != zlib_len)
			return false;

		return true;
	}

#pragma pack(push)
#pragma pack(1)
	struct png_chunk_prefix
	{
		uint32_t m_length;
		uint8_t m_type[4];
	};
	struct png_ihdr
	{
		png_chunk_prefix m_prefix;
		uint32_t m_width;
		uint32_t m_height;
		uint8_t m_bitdepth;
		uint8_t m_color_type;
		uint8_t m_comp_method;
		uint8_t m_filter_method;
		uint8_t m_interlace_method;
		uint32_t m_crc32;
	};
	const uint32_t IHDR_EXPECTED_LENGTH = 13;
	struct png_iend
	{
		png_chunk_prefix m_prefix;
		uint32_t m_crc32;
	};
#pragma pack(pop)

	static int fpng_get_info_internal(const void* pImage, uint32_t image_size, uint32_t& width, uint32_t& height, uint32_t& channels_in_file, uint32_t &idat_ofs, uint32_t &idat_len)
	{
		static const uint8_t s_png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

		width = 0;
		height = 0;
		channels_in_file = 0;
		idat_ofs = 0, idat_len = 0;
				
		// Ensure the file has at least a minimum possible size
		if (image_size < (sizeof(s_png_sig) + sizeof(png_ihdr) + sizeof(png_chunk_prefix) + 1 + sizeof(uint32_t) + sizeof(png_iend)))
			return FPNG_DECODE_FAILED_NOT_PNG;

		if (memcmp(pImage, s_png_sig, 8) != 0)
			return FPNG_DECODE_FAILED_NOT_PNG;

		const uint8_t* pImage_u8 = static_cast<const uint8_t*>(pImage) + 8;

		const png_ihdr& ihdr = *reinterpret_cast<const png_ihdr*>(pImage_u8);
		pImage_u8 += sizeof(png_ihdr);

		if (READ_BE32(&ihdr.m_prefix.m_length) != IHDR_EXPECTED_LENGTH)
			return FPNG_DECODE_FAILED_NOT_PNG;

		if (fpng_crc32(FPNG_CRC32_INIT, ihdr.m_prefix.m_type, 4 + IHDR_EXPECTED_LENGTH) != READ_BE32(&ihdr.m_crc32))
			return FPNG_DECODE_FAILED_HEADER_CRC32;

		width = READ_BE32(&ihdr.m_width);
		height = READ_BE32(&ihdr.m_height);
				
		if (!width || !height || (width > FPNG_MAX_SUPPORTED_DIM) || (height > FPNG_MAX_SUPPORTED_DIM))
			return FPNG_DECODE_FAILED_INVALID_DIMENSIONS;

		uint64_t total_pixels = (uint64_t)width * height;
		if (total_pixels > (1 << 30))
			return FPNG_DECODE_FAILED_INVALID_DIMENSIONS;

		if ((ihdr.m_comp_method) || (ihdr.m_filter_method) || (ihdr.m_interlace_method) || (ihdr.m_bitdepth != 8))
			return FPNG_DECODE_NOT_FPNG;

		if (ihdr.m_color_type == 2)
			channels_in_file = 3;
		else if (ihdr.m_color_type == 6)
			channels_in_file = 4;

		if (!channels_in_file)
			return FPNG_DECODE_NOT_FPNG;

		// Scan all the chunks. Look for one IDAT, IEND, and our custom fdEC chunk that indicates the file was compressed by us. Skip any ancillary chunks.
		bool found_fdec_chunk = false;
		
		for (; ; )
		{
			const size_t src_ofs = pImage_u8 - static_cast<const uint8_t*>(pImage);
			if (src_ofs >= image_size)
				return FPNG_DECODE_FAILED_CHUNK_PARSING;

			const uint32_t bytes_remaining = image_size - (uint32_t)src_ofs;
			if (bytes_remaining < sizeof(uint32_t) * 3)
				return FPNG_DECODE_FAILED_CHUNK_PARSING;

			const png_chunk_prefix* pChunk = reinterpret_cast<const png_chunk_prefix*>(pImage_u8);

			const uint32_t chunk_len = READ_BE32(&pChunk->m_length);
			if ((src_ofs + sizeof(uint32_t) + chunk_len + sizeof(uint32_t)) > image_size)
				return FPNG_DECODE_FAILED_CHUNK_PARSING;

			for (uint32_t i = 0; i < 4; i++)
			{
				const uint8_t c = pChunk->m_type[i];
				const bool is_upper = (c >= 65) && (c <= 90), is_lower = (c >= 97) && (c <= 122);
				if ((!is_upper) && (!is_lower))
					return FPNG_DECODE_FAILED_CHUNK_PARSING;
			}

			const uint32_t expected_crc32 = READ_BE32(pImage_u8 + sizeof(uint32_t) * 2 + chunk_len);

			char chunk_type[5] = { (char)pChunk->m_type[0], (char)pChunk->m_type[1], (char)pChunk->m_type[2], (char)pChunk->m_type[3], 0 };
			const bool is_idat = strcmp(chunk_type, "IDAT") == 0;

#if !FPNG_DISABLE_DECODE_CRC32_CHECKS
			if (!is_idat)
			{
				uint32_t actual_crc32 = fpng_crc32(FPNG_CRC32_INIT, pImage_u8 + sizeof(uint32_t), sizeof(uint32_t) + chunk_len);
				if (actual_crc32 != expected_crc32)
					return FPNG_DECODE_FAILED_HEADER_CRC32;
			}
#endif

			const uint8_t* pChunk_data = pImage_u8 + sizeof(uint32_t) * 2;

			if (strcmp(chunk_type, "IEND") == 0)
				break;
			else if (is_idat)
			{
				// If there were multiple IDAT's, or we didn't find the fdEC chunk, then it's not FPNG.
				if ((idat_ofs) || (!found_fdec_chunk))
					return FPNG_DECODE_NOT_FPNG;

				idat_ofs = (uint32_t)src_ofs;
				idat_len = chunk_len;

				// Sanity check the IDAT chunk length
				if (idat_len < 7)
					return FPNG_DECODE_FAILED_INVALID_IDAT;
			}
			else if (strcmp(chunk_type, "fdEC") == 0)
			{
				if (found_fdec_chunk)
					return FPNG_DECODE_NOT_FPNG;

				// We've got our fdEC chunk. Now make sure it's big enough and check its contents.
				if (chunk_len != 5)
					return FPNG_DECODE_NOT_FPNG;

				// Check fdEC chunk sig
				if ((pChunk_data[0] != 82) || (pChunk_data[1] != 36) || (pChunk_data[2] != 147) || (pChunk_data[3] != 227))
					return FPNG_DECODE_NOT_FPNG;

				// Check fdEC version
				if (pChunk_data[4] != FPNG_FDEC_VERSION)
					return FPNG_DECODE_NOT_FPNG;

				found_fdec_chunk = true;
			}
			else
			{
				// Bail if it's a critical chunk - can't be FPNG
				if ((chunk_type[0] & 32) == 0)
					return FPNG_DECODE_NOT_FPNG;

				// ancillary chunk - skip it
			}

			pImage_u8 += sizeof(png_chunk_prefix) + chunk_len + sizeof(uint32_t);
		}

		if ((!found_fdec_chunk) || (!idat_ofs))
			return FPNG_DECODE_NOT_FPNG;
		
		return FPNG_DECODE_SUCCESS;
	}

	int fpng_get_info(const void* pImage, uint32_t image_size, uint32_t& width, uint32_t& height, uint32_t& channels_in_file)
	{
		uint32_t idat_ofs = 0, idat_len = 0;
		return fpng_get_info_internal(pImage, image_size, width, height, channels_in_file, idat_ofs, idat_len);
	}

	int fpng_decode_memory(const void *pImage, uint32_t image_size, std::vector<uint8_t> &out, uint32_t& width, uint32_t& height, uint32_t &channels_in_file, uint32_t desired_channels)
	{
		out.resize(0);
		width = 0;
		height = 0;
		channels_in_file = 0;

		if ((!pImage) || (!image_size) || ((desired_channels != 3) && (desired_channels != 4)))
		{
			assert(0);
			return FPNG_DECODE_INVALID_ARG;
		}

		uint32_t idat_ofs = 0, idat_len = 0;
		int status = fpng_get_info_internal(pImage, image_size, width, height, channels_in_file, idat_ofs, idat_len);
		if (status)
			return status;
				
		const uint64_t mem_needed = (uint64_t)width * height * desired_channels;
		if (mem_needed > UINT32_MAX)
			return FPNG_DECODE_FAILED_DIMENSIONS_TOO_LARGE;

		// On 32-bit systems do a quick sanity check before we try to resize the output buffer.
		if ((sizeof(size_t) == sizeof(uint32_t)) && (mem_needed >= 0x80000000))
			return FPNG_DECODE_FAILED_DIMENSIONS_TOO_LARGE;

		out.resize(mem_needed);
		
		const uint8_t* pIDAT_data = static_cast<const uint8_t*>(pImage) + idat_ofs + sizeof(uint32_t) * 2;
		const uint32_t src_len = image_size - (idat_ofs + sizeof(uint32_t) * 2);

		bool decomp_status;
		if (desired_channels == 3)
		{
			if (channels_in_file == 3)
				decomp_status = fpng_pixel_zlib_decompress_3<3>(pIDAT_data, src_len, idat_len, out.data(), width, height);
			else
				decomp_status = fpng_pixel_zlib_decompress_4<3>(pIDAT_data, src_len, idat_len, out.data(), width, height);
		}
		else
		{
			if (channels_in_file == 3)
				decomp_status = fpng_pixel_zlib_decompress_3<4>(pIDAT_data, src_len, idat_len, out.data(), width, height);
			else
				decomp_status = fpng_pixel_zlib_decompress_4<4>(pIDAT_data, src_len, idat_len, out.data(), width, height);
		}
		if (!decomp_status)
		{
			// Something went wrong. Either the file data was corrupted, or it doesn't conform to one of our zlib/Deflate constraints.
			// The conservative thing to do is indicate it wasn't written by us, and let the general purpose PNG decoder handle it.
			return FPNG_DECODE_NOT_FPNG;
		}

		return FPNG_DECODE_SUCCESS;
	}

#ifndef FPNG_NO_STDIO
	int fpng_decode_file(const char* pFilename, std::vector<uint8_t>& out, uint32_t& width, uint32_t& height, uint32_t& channels_in_file, uint32_t desired_channels)
	{
		FILE* pFile = nullptr;

#ifdef _MSC_VER
		fopen_s(&pFile, pFilename, "rb");
#else
		pFile = fopen(pFilename, "rb");
#endif

		if (!pFile)
			return FPNG_DECODE_FILE_OPEN_FAILED;

		if (fseek(pFile, 0, SEEK_END) != 0)
		{
			fclose(pFile);
			return FPNG_DECODE_FILE_SEEK_FAILED;
		}

#ifdef _WIN32
		int64_t filesize = _ftelli64(pFile);
#else
		int64_t filesize = ftello(pFile);
#endif

		if (fseek(pFile, 0, SEEK_SET) != 0)
		{
			fclose(pFile);
			return FPNG_DECODE_FILE_SEEK_FAILED;
		}

		if ( (filesize < 0) || (filesize > UINT32_MAX) || ( (sizeof(size_t) == sizeof(uint32_t)) && (filesize > 0x70000000) ) )
		{
			fclose(pFile);
			return FPNG_DECODE_FILE_TOO_LARGE;
		}

		std::vector<uint8_t> buf((size_t)filesize);
		if (fread(buf.data(), 1, buf.size(), pFile) != buf.size())
		{
			fclose(pFile);
			return FPNG_DECODE_FILE_READ_FAILED;
		}

		fclose(pFile);

		return fpng_decode_memory(buf.data(), (uint32_t)buf.size(), out, width, height, channels_in_file, desired_channels);
	}
#endif

} // namespace fpng

/*
	Copyright (c) 2021 Richard Geldreich, Jr.

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

	http ://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissionsand
	limitations under the License.
*/
