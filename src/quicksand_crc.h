/* Private fused copy/CRC-32C helper. No runtime dispatch or initialization. */
#ifndef QUICKSAND_CRC_H
#define QUICKSAND_CRC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_MSC_VER)
#define QS_RESTRICT __restrict
#else
#define QS_RESTRICT restrict
#endif

#if !defined(QUICKSAND_CRC_SOFTWARE) && defined(__x86_64__) && defined(__SSE4_2__) && defined(__PCLMUL__)
#include <immintrin.h>
#define QUICKSAND_CRC_SIMD 1
#define QUICKSAND_CRC_BACKEND "x86 PCLMUL/CRC"
static inline uint32_t crc_word(uint32_t c, uint64_t w) { return (uint32_t) _mm_crc32_u64(c, w); }
static inline uint32_t crc_byte(uint32_t c, uint8_t b) { return _mm_crc32_u8(c, b); }
static inline uint32_t crc_word32(uint32_t c, uint32_t w) { return _mm_crc32_u32(c, w); }
static inline uint32_t crc_word16(uint32_t c, uint16_t w) { return _mm_crc32_u16(c, w); }
#elif !defined(QUICKSAND_CRC_SOFTWARE) && defined(__aarch64__) && defined(__ARM_NEON) && defined(__ARM_FEATURE_AES) && defined(__ARM_FEATURE_CRC32) && !defined(__AARCH64EB__)
#include <arm_acle.h>
#include <arm_neon.h>
#define QUICKSAND_CRC_SIMD 1
#define QUICKSAND_CRC_BACKEND "ARM NEON/PMULL/CRC"
static inline uint32_t crc_word(uint32_t c, uint64_t w) { return __crc32cd(c, w); }
static inline uint32_t crc_byte(uint32_t c, uint8_t b) { return __crc32cb(c, b); }
static inline uint32_t crc_word32(uint32_t c, uint32_t w) { return __crc32cw(c, w); }
static inline uint32_t crc_word16(uint32_t c, uint16_t w) { return __crc32ch(c, w); }
#else
#define QUICKSAND_CRC_SIMD 0
#define QUICKSAND_CRC_BACKEND "software table"
/* Reflected Castagnoli polynomial 0x82f63b78 (normal form 0x1edc6f41).
 * Entry i is i advanced by eight reflected polynomial steps. */
// clang-format off
static const uint32_t crc_table[256] = {
	0x00000000u, 0xf26b8303u, 0xe13b70f7u, 0x1350f3f4u,
	0xc79a971fu, 0x35f1141cu, 0x26a1e7e8u, 0xd4ca64ebu,
	0x8ad958cfu, 0x78b2dbccu, 0x6be22838u, 0x9989ab3bu,
	0x4d43cfd0u, 0xbf284cd3u, 0xac78bf27u, 0x5e133c24u,
	0x105ec76fu, 0xe235446cu, 0xf165b798u, 0x030e349bu,
	0xd7c45070u, 0x25afd373u, 0x36ff2087u, 0xc494a384u,
	0x9a879fa0u, 0x68ec1ca3u, 0x7bbcef57u, 0x89d76c54u,
	0x5d1d08bfu, 0xaf768bbcu, 0xbc267848u, 0x4e4dfb4bu,
	0x20bd8edeu, 0xd2d60dddu, 0xc186fe29u, 0x33ed7d2au,
	0xe72719c1u, 0x154c9ac2u, 0x061c6936u, 0xf477ea35u,
	0xaa64d611u, 0x580f5512u, 0x4b5fa6e6u, 0xb93425e5u,
	0x6dfe410eu, 0x9f95c20du, 0x8cc531f9u, 0x7eaeb2fau,
	0x30e349b1u, 0xc288cab2u, 0xd1d83946u, 0x23b3ba45u,
	0xf779deaeu, 0x05125dadu, 0x1642ae59u, 0xe4292d5au,
	0xba3a117eu, 0x4851927du, 0x5b016189u, 0xa96ae28au,
	0x7da08661u, 0x8fcb0562u, 0x9c9bf696u, 0x6ef07595u,
	0x417b1dbcu, 0xb3109ebfu, 0xa0406d4bu, 0x522bee48u,
	0x86e18aa3u, 0x748a09a0u, 0x67dafa54u, 0x95b17957u,
	0xcba24573u, 0x39c9c670u, 0x2a993584u, 0xd8f2b687u,
	0x0c38d26cu, 0xfe53516fu, 0xed03a29bu, 0x1f682198u,
	0x5125dad3u, 0xa34e59d0u, 0xb01eaa24u, 0x42752927u,
	0x96bf4dccu, 0x64d4cecfu, 0x77843d3bu, 0x85efbe38u,
	0xdbfc821cu, 0x2997011fu, 0x3ac7f2ebu, 0xc8ac71e8u,
	0x1c661503u, 0xee0d9600u, 0xfd5d65f4u, 0x0f36e6f7u,
	0x61c69362u, 0x93ad1061u, 0x80fde395u, 0x72966096u,
	0xa65c047du, 0x5437877eu, 0x4767748au, 0xb50cf789u,
	0xeb1fcbadu, 0x197448aeu, 0x0a24bb5au, 0xf84f3859u,
	0x2c855cb2u, 0xdeeedfb1u, 0xcdbe2c45u, 0x3fd5af46u,
	0x7198540du, 0x83f3d70eu, 0x90a324fau, 0x62c8a7f9u,
	0xb602c312u, 0x44694011u, 0x5739b3e5u, 0xa55230e6u,
	0xfb410cc2u, 0x092a8fc1u, 0x1a7a7c35u, 0xe811ff36u,
	0x3cdb9bddu, 0xceb018deu, 0xdde0eb2au, 0x2f8b6829u,
	0x82f63b78u, 0x709db87bu, 0x63cd4b8fu, 0x91a6c88cu,
	0x456cac67u, 0xb7072f64u, 0xa457dc90u, 0x563c5f93u,
	0x082f63b7u, 0xfa44e0b4u, 0xe9141340u, 0x1b7f9043u,
	0xcfb5f4a8u, 0x3dde77abu, 0x2e8e845fu, 0xdce5075cu,
	0x92a8fc17u, 0x60c37f14u, 0x73938ce0u, 0x81f80fe3u,
	0x55326b08u, 0xa759e80bu, 0xb4091bffu, 0x466298fcu,
	0x1871a4d8u, 0xea1a27dbu, 0xf94ad42fu, 0x0b21572cu,
	0xdfeb33c7u, 0x2d80b0c4u, 0x3ed04330u, 0xccbbc033u,
	0xa24bb5a6u, 0x502036a5u, 0x4370c551u, 0xb11b4652u,
	0x65d122b9u, 0x97baa1bau, 0x84ea524eu, 0x7681d14du,
	0x2892ed69u, 0xdaf96e6au, 0xc9a99d9eu, 0x3bc21e9du,
	0xef087a76u, 0x1d63f975u, 0x0e330a81u, 0xfc588982u,
	0xb21572c9u, 0x407ef1cau, 0x532e023eu, 0xa145813du,
	0x758fe5d6u, 0x87e466d5u, 0x94b49521u, 0x66df1622u,
	0x38cc2a06u, 0xcaa7a905u, 0xd9f75af1u, 0x2b9cd9f2u,
	0xff56bd19u, 0x0d3d3e1au, 0x1e6dcdeeu, 0xec064eedu,
	0xc38d26c4u, 0x31e6a5c7u, 0x22b65633u, 0xd0ddd530u,
	0x0417b1dbu, 0xf67c32d8u, 0xe52cc12cu, 0x1747422fu,
	0x49547e0bu, 0xbb3ffd08u, 0xa86f0efcu, 0x5a048dffu,
	0x8ecee914u, 0x7ca56a17u, 0x6ff599e3u, 0x9d9e1ae0u,
	0xd3d3e1abu, 0x21b862a8u, 0x32e8915cu, 0xc083125fu,
	0x144976b4u, 0xe622f5b7u, 0xf5720643u, 0x07198540u,
	0x590ab964u, 0xab613a67u, 0xb831c993u, 0x4a5a4a90u,
	0x9e902e7bu, 0x6cfbad78u, 0x7fab5e8cu, 0x8dc0dd8fu,
	0xe330a81au, 0x115b2b19u, 0x020bd8edu, 0xf0605beeu,
	0x24aa3f05u, 0xd6c1bc06u, 0xc5914ff2u, 0x37faccf1u,
	0x69e9f0d5u, 0x9b8273d6u, 0x88d28022u, 0x7ab90321u,
	0xae7367cau, 0x5c18e4c9u, 0x4f48173du, 0xbd23943eu,
	0xf36e6f75u, 0x0105ec76u, 0x12551f82u, 0xe03e9c81u,
	0x34f4f86au, 0xc69f7b69u, 0xd5cf889du, 0x27a40b9eu,
	0x79b737bau, 0x8bdcb4b9u, 0x988c474du, 0x6ae7c44eu,
	0xbe2da0a5u, 0x4c4623a6u, 0x5f16d052u, 0xad7d5351u,
};
// clang-format on
static inline uint32_t crc_byte(uint32_t c, uint8_t b) {
	return (c >> 8) ^ crc_table[(c ^ b) & 255];
}
#endif

#if QUICKSAND_CRC_SIMD
#if defined(__x86_64__)
typedef __m128i crc_vec;
static inline crc_vec crc_loadv(const uint8_t *p) {
	return _mm_loadu_si128((const __m128i *) p);
}
static inline void crc_storev(uint8_t *p, crc_vec x) {
	_mm_storeu_si128((__m128i *) p, x);
}
static inline crc_vec crc_vxor(crc_vec a, crc_vec b) { return _mm_xor_si128(a, b); }
static inline crc_vec crc_pair(uint64_t lo, uint64_t hi) {
	return _mm_set_epi64x(hi, lo);
}
static inline crc_vec crc_fold(crc_vec a, crc_vec b, crc_vec k) {
	return crc_vxor(
			b, crc_vxor(_mm_clmulepi64_si128(a, k, 0), _mm_clmulepi64_si128(a, k, 0x11)));
}
static inline uint32_t crc_reduce(crc_vec x) {
	uint32_t c = crc_word(0, (uint64_t) _mm_cvtsi128_si64(x));
	return crc_word(c, (uint64_t) _mm_cvtsi128_si64(_mm_srli_si128(x, 8)));
}
#else
typedef uint64x2_t crc_vec;
static inline crc_vec crc_loadv(const uint8_t *p) {
	return vreinterpretq_u64_u8(vld1q_u8(p));
}
static inline void crc_storev(uint8_t *p, crc_vec x) {
	vst1q_u8(p, vreinterpretq_u8_u64(x));
}
static inline crc_vec crc_vxor(crc_vec a, crc_vec b) { return veorq_u64(a, b); }
static inline crc_vec crc_pair(uint64_t lo, uint64_t hi) {
	uint64_t v[2] = {lo, hi};
	return vld1q_u64(v);
}
static inline crc_vec crc_fold(crc_vec a, crc_vec b, crc_vec k) {
	crc_vec lo = vreinterpretq_u64_p128(
			vmull_p64(vgetq_lane_u64(a, 0), vgetq_lane_u64(k, 0)));
	crc_vec hi = vreinterpretq_u64_p128(
			vmull_p64(vgetq_lane_u64(a, 1), vgetq_lane_u64(k, 1)));
	return crc_vxor(b, crc_vxor(lo, hi));
}
static inline uint32_t crc_reduce(crc_vec x) {
	return crc_word(crc_word(0, vgetq_lane_u64(x, 0)), vgetq_lane_u64(x, 1));
}
#endif
#endif

/* Copies exactly n bytes between nonoverlapping, arbitrarily aligned buffers.
 * CRC-32C uses initial/final XOR 0xffffffff: pass zero for the first buffer,
 * then the returned CRC to extend it. Empty input returns previous_crc.
 * SIMD is limited to little-endian targets; the bytewise fallback is portable.
 */
static inline uint32_t fast_memcpy_crc32c(unsigned char *QS_RESTRICT d,
					  const unsigned char *QS_RESTRICT s,
					  size_t n, uint32_t previous_crc) {
	uint32_t crc = ~previous_crc;
#if QUICKSAND_CRC_SIMD
	if(n >= 64) {
		/* Four 128-bit streams spaced 64 bytes apart. For a gap g, the low
		 * and high folding constants are raw CRC state 1 advanced through
		 * g and g-8 zero bytes, respectively (no initial/final XOR).
		 * k64 folds each stream; k16 combines them in message order.
		 * Two hardware CRC operations reduce the final 128-bit polynomial. */
		crc_vec k64 = crc_pair(0x740eef02, 0x9e4addf8), k16 = crc_pair(0xf20c0dfe, 0x493c7d27);
		crc_vec a = crc_loadv(s), b = crc_loadv(s + 16), c = crc_loadv(s + 32), e = crc_loadv(s + 48);
		crc_storev(d, a);
		crc_storev(d + 16, b);
		crc_storev(d + 32, c);
		crc_storev(d + 48, e);
		a = crc_vxor(a, crc_pair(crc, 0));
		d += 64;
		s += 64;
		n -= 64;
		while(n >= 64) {
			crc_vec w0 = crc_loadv(s), w1 = crc_loadv(s + 16), w2 = crc_loadv(s + 32),
				w3 = crc_loadv(s + 48);
			crc_storev(d, w0);
			crc_storev(d + 16, w1);
			crc_storev(d + 32, w2);
			crc_storev(d + 48, w3);
			a = crc_fold(a, w0, k64);
			b = crc_fold(b, w1, k64);
			c = crc_fold(c, w2, k64);
			e = crc_fold(e, w3, k64);
			s += 64;
			d += 64;
			n -= 64;
		}
		a = crc_fold(a, b, k16);
		a = crc_fold(a, c, k16);
		a = crc_fold(a, e, k16);
		crc = crc_reduce(a);
	}
	/* Bounded scalar remainder, also used for messages shorter than 64 bytes. */
	while(n >= 8) {
		uint64_t w;
		memcpy(&w, s, sizeof(w));
		memcpy(d, &w, sizeof(w));
		crc = crc_word(crc, w);
		d += 8;
		s += 8;
		n -= 8;
	}
	if(n >= 4) {
		uint32_t w;
		memcpy(&w, s, sizeof(w));
		memcpy(d, &w, sizeof(w));
		crc = crc_word32(crc, w);
		d += 4;
		s += 4;
		n -= 4;
	}
	if(n >= 2) {
		uint16_t w;
		memcpy(&w, s, sizeof(w));
		memcpy(d, &w, sizeof(w));
		crc = crc_word16(crc, w);
		d += 2;
		s += 2;
		n -= 2;
	}
#endif
	for(size_t i = 0; i < n; ++i) {
		uint8_t b = s[i];
		d[i] = b;
		crc = crc_byte(crc, b);
	}
	return ~crc;
}

#undef QS_RESTRICT
#endif
