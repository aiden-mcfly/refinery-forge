/**
 * Refinery entropy tank contract.
 *
 * MUST be byte-identical between refinery-core and refinery-forge.
 * The tank is producer-side evidence storage, never a refinery-core runtime
 * input. This header defines only the public per-subject state surface.
 * Forge stores additional private refuter bookkeeping on disk.
 */
#ifndef REFINERY_TANK_INTERFACE_H
#define REFINERY_TANK_INTERFACE_H

#include <stdint.h>

#include "refinery/substrate_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REFINERY_TANK_MAGIC_LEN 8u
#define REFINERY_TANK_MAGIC_STR "SELAHTNK"
REFINERY_STATIC_ASSERT(sizeof(REFINERY_TANK_MAGIC_STR) == REFINERY_TANK_MAGIC_LEN + 1u,
                       "REFINERY_TANK_MAGIC_STR must be exactly 8 bytes plus null terminator");

typedef enum TankEntryStatus {
  TANK_STATUS_PENDING = 0,
  TANK_STATUS_UN_REFUTED = 1,
  TANK_STATUS_REFUTED = 2,
} TankEntryStatus;

typedef struct TankEntry {
  uint8_t magic[REFINERY_TANK_MAGIC_LEN];
  Hash128 canonical_hash;
  uint32_t refutations_attempted;
  /* Forge tracks at most 16 distinct refuters per subject and refuses the
   * 17th distinct refuter rather than silently over-counting. */
  uint32_t distinct_refuters;
  uint32_t status;
  uint32_t pad;
  Hash128 latest_event_id;
} TankEntry;

REFINERY_STATIC_ASSERT(sizeof(TankEntry) == 56, "TankEntry must be 56 bytes");

#define REFINERY_MIN_REFUTATIONS_ATTEMPTED_DEFAULT 3u
#define REFINERY_MIN_DISTINCT_REFUTERS_DEFAULT 2u

#ifdef __cplusplus
}
#endif

#endif /* REFINERY_TANK_INTERFACE_H */
