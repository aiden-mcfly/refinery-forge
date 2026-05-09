/**
 * Refinery substrate binary contract (frozen format v1).
 * Duplicated in refinery-forge — keep byte-for-byte identical.
 *
 * Alignment with OPP-51 (deterministic identity) + OPP-49 (canon corpus).
 */
#ifndef REFINERY_SUBSTRATE_INTERFACE_H
#define REFINERY_SUBSTRATE_INTERFACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 8-byte LE magic: "SELAHRF1" — substrate integrity seal + format generation 1 */
#define REFINERY_MAGIC UINT64_C(0x31465248414C4553)

/** Format version baked into binaries (OPP-51 track: deterministic refinery v1) */
#define REFINERY_FORMAT_VERSION_OPP51_1 1u

/** Physical mesh ceiling for sliding-window spectroscopy under current v1 contract */
#define REFINERY_MAX_MARKERS 1200000u

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
 * All integers little-endian on wire.
 */
typedef struct SubstrateHeader {
  uint64_t magic;
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

#if defined(__cplusplus)
static_assert(sizeof(SubstrateHeader) == 64, "SubstrateHeader must be 64 bytes");
#else
_Static_assert(sizeof(SubstrateHeader) == 64, "SubstrateHeader must be 64 bytes");
#endif

/**
 * One sliding-window marker: hash identity + span location in canon blob.
 */
typedef struct Marker {
  Hash128 hash_id;
  /** Byte offset into canon blob where span begins */
  uint64_t canon_offset;
  /** Span length in bytes (UTF-8) */
  uint32_t span_bytes;
  uint8_t pad[4];
} Marker;

_Static_assert(sizeof(Marker) == 32, "Marker must be 32 bytes");

#ifdef __cplusplus
}
#endif

#endif /* REFINERY_SUBSTRATE_INTERFACE_H */
