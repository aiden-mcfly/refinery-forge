/**
 * Refinery entropy tank contract.
 *
 * MUST be byte-identical between refinery-core and refinery-forge.
 * The tank is producer-side evidence storage, never a refinery-core runtime
 * input. This header defines the public v2 tank file surface; forge stores
 * additional private refuter bookkeeping between the entries region and the
 * canon text blob.
 */
#ifndef REFINERY_TANK_INTERFACE_H
#define REFINERY_TANK_INTERFACE_H

#include <stdint.h>

#include "refinery/substrate_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REFINERY_TANK_MAGIC_LEN 8u
#define REFINERY_TANK_MAGIC_STR_V1 "SELAHTNK"
#define REFINERY_TANK_MAGIC_STR "SELAHTN2"
REFINERY_STATIC_ASSERT(sizeof(REFINERY_TANK_MAGIC_STR_V1) == REFINERY_TANK_MAGIC_LEN + 1u,
                       "REFINERY_TANK_MAGIC_STR_V1 must be exactly 8 bytes plus null terminator");
REFINERY_STATIC_ASSERT(sizeof(REFINERY_TANK_MAGIC_STR) == REFINERY_TANK_MAGIC_LEN + 1u,
                       "REFINERY_TANK_MAGIC_STR must be exactly 8 bytes plus null terminator");

#define REFINERY_TANK_FORMAT_VERSION 2u
#define REFINERY_TANK_MAX_REFUTERS 16u
#define REFINERY_TANK_FILE_MAX_BYTES (2u * 1024u * 1024u * 1024u)

typedef enum TankEntryStatus {
  TANK_STATUS_PENDING = 0,
  TANK_STATUS_UN_REFUTED = 1,
  TANK_STATUS_REFUTED = 2,
} TankEntryStatus;

typedef struct TankFileHeader {
  uint8_t magic[REFINERY_TANK_MAGIC_LEN];
  uint32_t format_version;
  uint32_t entry_count;
  uint64_t entries_offset;
  uint64_t entries_bytes;
  uint64_t canon_offset;
  uint64_t canon_size;
  uint8_t reserved[16];
} TankFileHeader;

REFINERY_STATIC_ASSERT(sizeof(TankFileHeader) == 64, "TankFileHeader must be 64 bytes");

/*
 * Field order is constrained by natural 8-byte alignment of Hash128.
 * Hash128 fields (canonical_hash, latest_event_id) are placed at 8-byte-
 * aligned offsets, and an explicit trailing pad takes the struct to a
 * multiple of 8 so the compiler inserts NO additional padding. Total
 * on-disk size per entry: 64 bytes.
 */
typedef struct TankEntry {
  uint8_t magic[REFINERY_TANK_MAGIC_LEN];   /*  0..8  */
  Hash128 canonical_hash;                    /*  8..24 */
  Hash128 latest_event_id;                   /* 24..40 */
  uint32_t refutations_attempted;            /* 40..44 */
  uint32_t distinct_refuters;                /* 44..48 */
  uint32_t status;                           /* 48..52 */
  uint32_t canon_offset;                     /* 52..56  byte offset into the canon blob */
  uint16_t canon_size;                       /* 56..58  signal text length in bytes */
  uint16_t pad;                              /* 58..60  zero in v2 writers */
  uint32_t trailing_pad;                     /* 60..64  zero in v2 writers */
} TankEntry;

REFINERY_STATIC_ASSERT(sizeof(TankEntry) == 64, "TankEntry must be 64 bytes (Hash128 forces 8-byte struct alignment)");

#define REFINERY_MIN_REFUTATIONS_ATTEMPTED_DEFAULT 3u
#define REFINERY_MIN_DISTINCT_REFUTERS_DEFAULT 2u

#ifdef __cplusplus
}
#endif

#endif /* REFINERY_TANK_INTERFACE_H */
