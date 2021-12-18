// fpng.cpp - R. Geldreich, Jr. - public domain or zlib license (see end for Public Domain waiver/optional license)
// Fast 24/32bpp standard .PNG image writer. 
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

// This module relies on the fast CRC-32 function in Crc32.cpp/.h.
#include "Crc32.h"

#ifndef FPNG_NO_STDIO
#include <stdio.h>
#endif

#ifndef __LITTLE_ENDIAN
#define __LITTLE_ENDIAN 1234
#endif
#ifndef __BIG_ENDIAN
#define __BIG_ENDIAN 4321
#endif

#if defined(_MSC_VER) || defined(__MINGW32__)
	// Assume little endian on Windows.
	#define __BYTE_ORDER __LITTLE_ENDIAN
#else
	// for __BYTE_ORDER (__LITTLE_ENDIAN or __BIG_ENDIAN)
	#include <sys/param.h>

	#if !defined(__BYTE_ORDER)
	#error __BYTE_ORDER undefined. Compile with -D__BYTE_ORDER=1234 for little endian or -D__BYTE_ORDER=4321 for big endian.
	#endif
#endif

namespace fpng
{
	template <typename S> static inline S maximum(S a, S b) { return (a > b) ? a : b; }
	template <typename S> static inline S minimum(S a, S b) { return (a < b) ? a : b; }
		
	static const int FPNG_FALSE = 0, FPNG_TRUE = 1, FPNG_ADLER32_INIT = 1;

	static inline uint32_t simple_swap32(uint32_t x) { return (x >> 24) | ((x >> 8) & 0x0000FF00) | ((x << 8) & 0x00FF0000) | (x << 24); }
	static inline uint64_t simple_swap64(uint64_t x) { return (((uint64_t)simple_swap32((uint32_t)x)) << 32U) | simple_swap32((uint32_t)(x >> 32U)); }

#if __BYTE_ORDER == __BIG_ENDIAN
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

	#define READ_LE32(p) swap32(*reinterpret_cast<const uint32_t *>(p))
	#define WRITE_LE32(p, v) *reinterpret_cast<uint32_t *>(p) = swap32((uint32_t)(v))
	#define WRITE_LE64(p, v) *reinterpret_cast<uint64_t *>(p) = swap64((uint64_t)(v))
#else
	#define READ_LE32(p) (*reinterpret_cast<const uint32_t *>(p))
	#define WRITE_LE32(p, v) *reinterpret_cast<uint32_t *>(p) = (uint32_t)(v)
	#define WRITE_LE64(p, v) *reinterpret_cast<uint64_t *>(p) = (uint64_t)(v)
#endif
		
	const uint32_t FPNG_CRC32_INIT = 0;
	static inline uint32_t fpng_crc32(uint32_t prev_crc32, const void* pData, size_t size)
	{
		return crc32_fast(pData, size, prev_crc32);
	}
		
	static uint32_t fpng_adler32(uint32_t adler, const uint8_t* ptr, size_t buf_len)
	{
		uint32_t i, s1 = (uint32_t)(adler & 0xffff), s2 = (uint32_t)(adler >> 16); size_t block_len = buf_len % 5552;
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

	static const uint8_t g_defl_small_dist_extra[512] = {
	  0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,
	  5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
	  6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	  7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
	  7,7,7,7,7,7,7,7 };

	static const uint8_t g_defl_large_dist_sym[128] = {
	  0,0,18,19,20,20,21,21,22,22,22,22,23,23,23,23,24,24,24,24,24,24,24,24,25,25,25,25,25,25,25,25,26,26,26,26,26,26,26,26,26,26,26,26,
	  26,26,26,26,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,27,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,28,
	  28,28,28,28,28,28,28,28,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29,29 };

	static const uint8_t g_defl_large_dist_extra[128] = {
	  0,0,8,8,9,9,9,9,10,10,10,10,10,10,10,10,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,11,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,
	  12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,12,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,
	  13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13,13 };

	static const uint32_t g_bitmasks[17] = { 0x0000, 0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F, 0x00FF, 0x01FF, 0x03FF, 0x07FF, 0x0FFF, 0x1FFF, 0x3FFF, 0x7FFF, 0xFFFF };

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

		defl_optimize_huffman_table(d, 0, DEFL_MAX_HUFF_SYMBOLS_0, 15, FPNG_FALSE);
		defl_optimize_huffman_table(d, 1, DEFL_MAX_HUFF_SYMBOLS_1, 15, FPNG_FALSE);

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

	static uint32_t pixel_deflate_dyn_3(
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

		std::vector<uint32_t> codes;
		codes.reserve(w * h);

		uint32_t lit_freq[DEFL_MAX_HUFF_SYMBOLS_0], dist_freq[DEFL_MAX_HUFF_SYMBOLS_1];
		memset(lit_freq, 0, sizeof(lit_freq));
		memset(dist_freq, 0, sizeof(dist_freq));

		const uint32_t HASH_TABLE_SIZE = 4096;
		uint32_t hash_tab[HASH_TABLE_SIZE];
		memset(hash_tab, 0xFF, sizeof(hash_tab));

		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			codes.push_back(1 | (filter_lit << 8));
			lit_freq[filter_lit]++;

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs) & 0xFFFFFF;

				uint32_t hash = (lits ^ (lits >> 17)) & (HASH_TABLE_SIZE - 1);

				uint32_t match_ofs = hash_tab[hash];
				hash_tab[hash] = src_ofs;

				uint32_t match_dist = src_ofs - match_ofs;

				if ((match_ofs != UINT32_MAX) && (lits == (READ_LE32(pSrc + match_ofs) & 0xFFFFFF)) && (match_dist < 0x8000))
				{
					uint32_t match_len = 3;
					uint32_t max_match_len = minimum<int>(255, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						// could be optimized for big endian CPU's
						if ((READ_LE32(pSrc + src_ofs + match_len) & 0xFFFFFF) != (READ_LE32(pSrc + match_ofs + match_len) & 0xFFFFFF))
							break;
						match_len += 3;
					}

					if ((match_len == 3) && (match_dist >= 16384))
					{
						codes.push_back(lits << 8);

						lit_freq[lits & 0xFF]++;
						lit_freq[(lits >> 8) & 0xFF]++;
						lit_freq[(lits >> 16) & 0xFF]++;

						src_ofs += 3;
					}
					else
					{
						uint32_t adj_match_dist = match_dist - 1;
						codes.push_back((match_len - 1) | (adj_match_dist << 8));

						uint32_t adj_match_len = match_len - 3;

						lit_freq[g_defl_len_sym[adj_match_len]]++;

						uint32_t s0 = g_defl_small_dist_sym[adj_match_dist & 511];
						uint32_t s1 = g_defl_large_dist_sym[adj_match_dist >> 8];
						uint32_t sym = (adj_match_dist < 512) ? s0 : s1;

						dist_freq[sym]++;

						src_ofs += match_len;
					}
				}
				else
				{
					codes.push_back(lits << 8);

					lit_freq[lits & 0xFF]++;
					lit_freq[(lits >> 8) & 0xFF]++;
					lit_freq[lits >> 16]++;

					src_ofs += 3;
				}

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);

		lit_freq[256] = 1;

		defl_huff dh;
		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_0, lit_freq, &dh.m_huff_count[0][0]);
		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_1, dist_freq, &dh.m_huff_count[1][0]);

		if (!defl_start_dynamic_block(&dh, pDst, dst_ofs, dst_buf_size, bit_buf, bit_buf_size))
			return 0;

		assert(bit_buf_size <= 7);

		for (uint32_t i = 0; i < codes.size(); i++)
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
				uint32_t adj_match_dist = c >> 8;

				PUT_BITS_CZ(dh.m_huff_codes[0][g_defl_len_sym[adj_match_len]], dh.m_huff_code_sizes[0][g_defl_len_sym[adj_match_len]]);
				PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], g_defl_len_extra[adj_match_len]); // up to 5 bits

				uint32_t s0 = g_defl_small_dist_sym[adj_match_dist & 511];
				uint32_t n0 = g_defl_small_dist_extra[adj_match_dist & 511];
				uint32_t s1 = g_defl_large_dist_sym[adj_match_dist >> 8];
				uint32_t n1 = g_defl_large_dist_extra[adj_match_dist >> 8];
				uint32_t sym = (adj_match_dist < 512) ? s0 : s1;
				uint32_t num_extra_bits = (adj_match_dist < 512) ? n0 : n1;

				PUT_BITS_CZ(dh.m_huff_codes[1][sym], dh.m_huff_code_sizes[1][sym]);
				PUT_BITS(adj_match_dist & g_bitmasks[num_extra_bits], num_extra_bits); // up to 13 bits
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

	static uint32_t pixel_deflate_dyn_4(
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
		codes.reserve(w * h);

		uint32_t lit_freq[DEFL_MAX_HUFF_SYMBOLS_0], dist_freq[DEFL_MAX_HUFF_SYMBOLS_1];
		memset(lit_freq, 0, sizeof(lit_freq));
		memset(dist_freq, 0, sizeof(dist_freq));

		const uint32_t HASH_TABLE_SIZE = 4096;
		uint32_t hash_tab[HASH_TABLE_SIZE];
		memset(hash_tab, 0xFF, sizeof(hash_tab));

		const uint8_t* pSrc = pImg;
		uint32_t src_ofs = 0;

		uint32_t src_adler32 = fpng_adler32(FPNG_ADLER32_INIT, pImg, bpl * h);

		for (uint32_t y = 0; y < h; y++)
		{
			const uint32_t end_src_ofs = src_ofs + bpl;

			const uint32_t filter_lit = pSrc[src_ofs++];
			codes.push_back(1 | (filter_lit << 8));
			lit_freq[filter_lit]++;

			while (src_ofs < end_src_ofs)
			{
				uint32_t lits = READ_LE32(pSrc + src_ofs);

				uint32_t hash = (lits ^ (lits >> 12) ^ (lits >> 20)) & (HASH_TABLE_SIZE - 1);

				uint32_t match_ofs = hash_tab[hash];
				hash_tab[hash] = src_ofs;

				uint32_t match_dist = src_ofs - match_ofs;

				if ((match_ofs != UINT32_MAX) && (lits == READ_LE32(pSrc + match_ofs)) && (match_dist < 0x8000))
				{
					uint32_t match_len = 4;
					uint32_t max_match_len = minimum<int>(252, (int)(end_src_ofs - src_ofs));

					while (match_len < max_match_len)
					{
						// unaligned reads
						if (*(const uint32_t*)(pSrc + src_ofs + match_len) != *(const uint32_t*)(pSrc + match_ofs + match_len))
							break;
						match_len += 4;
					}

					uint32_t adj_match_dist = match_dist - 1;
					codes.push_back((match_len - 1) | (adj_match_dist << 8));

					uint32_t adj_match_len = match_len - 3;

					lit_freq[g_defl_len_sym[adj_match_len]]++;

					uint32_t s0 = g_defl_small_dist_sym[adj_match_dist & 511];
					uint32_t s1 = g_defl_large_dist_sym[adj_match_dist >> 8];
					uint32_t sym = (adj_match_dist < 512) ? s0 : s1;

					dist_freq[sym]++;

					src_ofs += match_len;
				}
				else
				{
					codes.push_back((uint64_t)lits << 8);

					lit_freq[lits & 0xFF]++;
					lit_freq[(lits >> 8) & 0xFF]++;
					lit_freq[(lits >> 16) & 0xFF]++;
					lit_freq[lits >> 24]++;

					src_ofs += 4;
				}

			} // while (src_ofs < end_src_ofs)

		} // y

		assert(src_ofs == h * bpl);

		lit_freq[256] = 1;

		defl_huff dh;
		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_0, lit_freq, &dh.m_huff_count[0][0]);
		adjust_freq32(DEFL_MAX_HUFF_SYMBOLS_1, dist_freq, &dh.m_huff_count[1][0]);

		if (!defl_start_dynamic_block(&dh, pDst, dst_ofs, dst_buf_size, bit_buf, bit_buf_size))
			return 0;

		assert(bit_buf_size <= 7);

		for (uint32_t i = 0; i < codes.size(); i++)
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
				uint32_t adj_match_dist = (uint32_t)(c >> 8);

				PUT_BITS_CZ(dh.m_huff_codes[0][g_defl_len_sym[adj_match_len]], dh.m_huff_code_sizes[0][g_defl_len_sym[adj_match_len]]);
				PUT_BITS(adj_match_len & g_bitmasks[g_defl_len_extra[adj_match_len]], g_defl_len_extra[adj_match_len]); // up to 5 bits

				uint32_t s0 = g_defl_small_dist_sym[adj_match_dist & 511];
				uint32_t n0 = g_defl_small_dist_extra[adj_match_dist & 511];
				uint32_t s1 = g_defl_large_dist_sym[adj_match_dist >> 8];
				uint32_t n1 = g_defl_large_dist_extra[adj_match_dist >> 8];
				uint32_t sym = (adj_match_dist < 512) ? s0 : s1;
				uint32_t num_extra_bits = (adj_match_dist < 512) ? n0 : n1;

				PUT_BITS_CZ(dh.m_huff_codes[1][sym], dh.m_huff_code_sizes[1][sym]);
				PUT_BITS(adj_match_dist & g_bitmasks[num_extra_bits], num_extra_bits); // up to 13 bits
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

	static void vector_append(std::vector<uint8_t>& buf, const void* pData, size_t len)
	{
		if (len)
		{
			size_t l = buf.size();
			buf.resize(l + len);
			memcpy(buf.data() + l, pData, len);
		}
	}

	bool fpng_encode_image_to_memory(const void* pImage, int w, int h, int num_chans, bool flip, std::vector<uint8_t>& out_buf)
	{
		if ((num_chans != 3) && (num_chans != 4))
			return false;

		int i, bpl = w * num_chans, y;

		std::vector<uint8_t> temp_buf;
		temp_buf.resize(((bpl + 1) * h + 7) & ~7);
		uint32_t temp_buf_ofs = 0;

		for (y = 0; y < h; ++y)
		{
			const uint8_t* pSrc = (uint8_t*)pImage + (flip ? (h - 1 - y) : y) * bpl;

			if (!y)
			{
				temp_buf[temp_buf_ofs++] = 0;

				memcpy(&temp_buf[temp_buf_ofs], pSrc, bpl);
			}
			else
			{
				const uint8_t* pPrev_src = (uint8_t*)pImage + (flip ? (h - 1 - (y - 1)) : (y - 1)) * bpl;

				// Always use filter 2 (previous scanline)
				temp_buf[temp_buf_ofs++] = 2;

				uint8_t* pDst = &temp_buf[temp_buf_ofs];

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
			}

			temp_buf_ofs += bpl;
		}

		const uint32_t PNG_HEADER_SIZE = 41;

		out_buf.resize((PNG_HEADER_SIZE + (bpl + 1) * h + 7) & ~7);

		uint32_t out_ofs = PNG_HEADER_SIZE;

		uint32_t defl_size;
		if (num_chans == 3)
			defl_size = pixel_deflate_dyn_3(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);
		else
			defl_size = pixel_deflate_dyn_4(temp_buf.data(), w, h, &out_buf[out_ofs], (uint32_t)out_buf.size() - out_ofs);

		if (!defl_size)
		{
			// Dynamic block failed to compress - fall back to uncompressed blocks.
			out_buf.resize(PNG_HEADER_SIZE + 6 + temp_buf_ofs + ((temp_buf_ofs + 65534) / 65535) * 5);

			uint32_t raw_size = write_raw_block(temp_buf.data(), (uint32_t)temp_buf_ofs, out_buf.data() + PNG_HEADER_SIZE, (uint32_t)out_buf.size() - PNG_HEADER_SIZE);
			if (!raw_size)
				return false;

			out_buf.resize(PNG_HEADER_SIZE + raw_size);
		}
		else
		{
			out_buf.resize(PNG_HEADER_SIZE + defl_size);
		}

		const uint32_t idat_len = (uint32_t)out_buf.size() - PNG_HEADER_SIZE;

		// Write real PNG header
		{
			static const uint8_t chans[] = { 0x00, 0x00, 0x04, 0x02, 0x06 };

			uint8_t pnghdr[41] = { 0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
			  0,0,(uint8_t)(w >> 8),(uint8_t)w,0,0,(uint8_t)(h >> 8),(uint8_t)h,8,chans[num_chans],0,0,0,0,0,0,0,
			  (uint8_t)(idat_len >> 24),(uint8_t)(idat_len >> 16),(uint8_t)(idat_len >> 8),(uint8_t)idat_len,0x49,0x44,0x41,0x54 };

			uint32_t c = (uint32_t)fpng_crc32(FPNG_CRC32_INIT, pnghdr + 12, 17);
			for (i = 0; i < 4; ++i, c <<= 8)
				((uint8_t*)(pnghdr + 29))[i] = (uint8_t)(c >> 24);

			memcpy(out_buf.data(), pnghdr, PNG_HEADER_SIZE);
		}

		vector_append(out_buf, "\0\0\0\0\0\0\0\0\x49\x45\x4e\x44\xae\x42\x60\x82", 16);

		// Compute IDAT crc32
		uint32_t c = (uint32_t)fpng_crc32(FPNG_CRC32_INIT, out_buf.data() + PNG_HEADER_SIZE - 4, idat_len + 4);

		for (i = 0; i < 4; ++i, c <<= 8)
			(out_buf.data() + out_buf.size() - 16)[i] = (uint8_t)(c >> 24);

		return true;
	}

#ifndef FPNG_NO_STDIO
	bool fpng_encode_image_to_file(const char* pFilename, const void* pImage, int w, int h, int num_chans, bool flip)
	{
		std::vector<uint8_t> out_buf;
		if (!fpng_encode_image_to_memory(pImage, w, h, num_chans, flip, out_buf))
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

} // namespace fpng

/*
  This software has been explictly and purposely placed into the Public Domain 
  and is not Intellectual Property.
  
  However you may also choose the zlib license, which can be useful in legal 
  jurisdictions which don't recognize the Public Domain:

  Copyright (C) 2011-2021 Richard Geldreich, Jr.

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the authors be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
	 claim that you wrote the original software. If you use this software
	 in a product, an acknowledgment in the product documentation would be
	 appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
	 misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Richard Geldreich, Jr. 12/17/2021
  richgel99@gmail.com
*/
