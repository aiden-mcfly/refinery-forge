/**
 * Refinery evidence ledger contract.
 *
 * MUST be byte-identical between refinery-core and refinery-forge.
 * The ledger is a forge-owned evidence artifact; refinery-core does not read it
 * on the kernel hot path.
 */
#ifndef REFINERY_LEDGER_INTERFACE_H
#define REFINERY_LEDGER_INTERFACE_H

#include <stdint.h>

#include "refinery/substrate_interface.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REFINERY_LEDGER_MAGIC_LEN 8u
#define REFINERY_LEDGER_MAGIC_STR "SELAHLDG"
REFINERY_STATIC_ASSERT(sizeof(REFINERY_LEDGER_MAGIC_STR) == REFINERY_LEDGER_MAGIC_LEN + 1u,
                       "REFINERY_LEDGER_MAGIC_STR must be exactly 8 bytes plus null terminator");

typedef enum LedgerEventType {
  LEDGER_EVENT_TESTIMONY = 1,
  LEDGER_EVENT_REJECTION = 2,
  LEDGER_EVENT_REFUTATION_ATTEMPTED = 3,
  LEDGER_EVENT_REFUTATION_SUCCEEDED = 4,
  LEDGER_EVENT_EVICTION = 5,
  LEDGER_EVENT_PROMOTION = 6,
} LedgerEventType;

typedef struct LedgerEntry {
  uint8_t magic[REFINERY_LEDGER_MAGIC_LEN];
  uint32_t event_type;
  uint32_t pad;
  Hash128 entry_id;
  Hash128 subject_hash;
  Hash128 evidence_hash;
  Hash128 prev_entry_id;
  uint64_t sequence;
} LedgerEntry;

REFINERY_STATIC_ASSERT(sizeof(LedgerEntry) == 88, "LedgerEntry must be 88 bytes");

#ifdef __cplusplus
}
#endif

#endif /* REFINERY_LEDGER_INTERFACE_H */
