/**
 * Refinery substrate binary contract (frozen format v1).
 *
 * MUST be byte-identical between refinery-core and refinery-forge.
 * Single source of truth for: on-disk layout, magic bytes, size constants,
 * word-boundary classification, and the window hash algorithm spec.
 *
 * Alignment with OPP-51 (deterministic identity) + OPP-49 (canon corpus).
 */
#ifndef REFINERY_SUBSTRATE_INTERFACE_H
#define REFINERY_SUBSTRATE_INTERFACE_H

#include <stdint.h>

/* Portable file-scope static assertion: C uses _Static_assert (C11),
 * C++ uses static_assert (C++11). */
#if defined(__cplusplus)
#define REFINERY_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define REFINERY_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Endianness contract: integer header/marker fields are stored in host byte
 * order under the assertion that the host IS little-endian. The magic seal
 * is stored as an explicit byte array so its on-wire bytes are unambiguous
 * on any host. On a big-endian host this header fails to compile — by design.
 * A v2 contract that supports big-endian hosts must be a deliberate spec
 * change, not a silent assumption.
 */
#if defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__)
REFINERY_STATIC_ASSERT(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
                       "Refinery substrate v1 requires a little-endian host");
#endif

/** 8-byte ASCII magic "SELAHRF1" — substrate integrity seal + format gen 1.
 *  Stored as bytes-on-wire (not a typed integer) so the seal is portable
 *  across host byte orders even though the rest of the v1 contract is LE-only. */
#define REFINERY_MAGIC_LEN 8u
#define REFINERY_MAGIC_STR "SELAHRF1"
REFINERY_STATIC_ASSERT(sizeof(REFINERY_MAGIC_STR) == REFINERY_MAGIC_LEN + 1u,
                       "REFINERY_MAGIC_STR must be exactly 8 bytes plus null terminator");

/** Format version baked into binaries (OPP-51 track: deterministic refinery v1) */
#define REFINERY_FORMAT_VERSION_OPP51_1 1u

/** Physical mesh ceiling for sliding-window spectroscopy under current v1 contract */
#define REFINERY_MAX_MARKERS 1200000u

/** Sliding-window size for spectroscopy. Frozen at v1. */
#define REFINERY_WINDOW_WORDS 7u

/** Maximum byte length of a single tokenized word. Shared cap.
 *  Forge MUST silence (refuse .bin emission) on violation.
 *  Kernel MUST return REFINERY_SPECTROSCOPY_ERR_WORD_TOO_LONG on violation. */
#define REFINERY_MAX_WORD_BYTES 128u

/** Single source of truth for word-boundary classification.
 *  Kernel and forge MUST tokenize identically; both call this predicate.
 *  Word class = ASCII [0-9A-Za-z]. Case is preserved (no folding in v1). */
static inline int refinery_word_byte(unsigned char ch) {
  return (ch >= '0' && ch <= '9') ||
         (ch >= 'a' && ch <= 'z') ||
         (ch >= 'A' && ch <= 'Z');
}

/* ============================================================================
 * Window hash algorithm spec (kernel + forge MUST agree byte-for-byte):
 *
 *   1. Tokenize input into maximal runs of bytes for which
 *      refinery_word_byte() returns true. Case is preserved (no folding).
 *      Any single token >= REFINERY_MAX_WORD_BYTES collapses to SILENCE.
 *   2. For every position i in [0, word_count - REFINERY_WINDOW_WORDS]:
 *      a. Construct a flattened buffer:
 *           token[i] ++ 0x20 ++ token[i+1] ++ 0x20 ++ ... ++ token[i+6]
 *         (exactly 6 separator bytes between 7 tokens; no leading/trailing 0x20)
 *      b. Hash that buffer with XXH3_128bits. Store digest as Hash128
 *         (low64, high64) per the xxHash C API.
 *      c. canon_offset = byte offset of token[i] start in original canon.
 *         span_bytes   = end_of_token[i+6] - start_of_token[i]
 *                        (in original canon bytes; includes any non-word bytes
 *                         lying between the 7 tokens).
 *
 * Any divergence between kernel and forge on this construction is a v1
 * contract violation and produces drift between forge .bin output and
 * kernel spectroscopy lookups. Parity is enforced by the shared constants
 * and refinery_word_byte() above; downstream code must not redefine them.
 * ============================================================================ */

/**
 * Portable 128-bit identity (xxHash128 digest or logical duplicate).
 * Lexicographic sort order: high64 first, then low64 (canonical total order).
 */
typedef struct Hash128 {
  uint64_t low64;
  uint64_t high64;
} Hash128;

/**
 * On-disk header — fixed 64 bytes for SIMD-friendly alignment.
 * Multi-byte integer fields are little-endian on wire (LE host enforced above).
 * The magic field is bytes-on-wire (host-order-independent integrity seal).
 */
typedef struct SubstrateHeader {
  /** "SELAHRF1" bytes-on-wire — host-byte-order-independent integrity seal */
  uint8_t magic[REFINERY_MAGIC_LEN];
  uint32_t format_version;
  uint32_t marker_count;
  /** XXH64(canon_bytes, canon_size, seed 0) — classic XXH64, not XXH3 */
  uint64_t canon_xxh64;
  /** Byte offset from file start to start of Marker array */
  uint64_t markers_offset;
  /** Byte length of Marker array = marker_count * sizeof(Marker) */
  uint64_t markers_bytes;
  /** Byte offset from file start to canon UTF-8 blob */
  uint64_t canon_offset;
  /** Length in bytes of canon blob */
  uint64_t canon_size;
  /** Reserved — must be zero in v1 writers */
  uint8_t reserved[8];
} SubstrateHeader;

REFINERY_STATIC_ASSERT(sizeof(SubstrateHeader) == 64, "SubstrateHeader must be 64 bytes");

/**
 * One sliding-window marker: hash identity + span location in canon blob.
 */
typedef struct Marker {
  Hash128 hash_id;
  /** Byte offset into canon blob where span begins */
  uint32_t canon_offset;
  /** Span length in bytes (UTF-8) */
  uint16_t span_bytes;
  uint16_t pad;
} Marker;

REFINERY_STATIC_ASSERT(sizeof(Marker) == 24, "Marker must be 24 bytes");

#ifdef __cplusplus
}
#endif

#endif /* REFINERY_SUBSTRATE_INTERFACE_H */
