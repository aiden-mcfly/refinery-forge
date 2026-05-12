#ifndef REFINERY_FORGE_TANK_H
#define REFINERY_FORGE_TANK_H

#include "ledger.h"
#include "refinery/tank_interface.h"

#include <cstddef>
#include <string>

namespace refinery_forge {

struct TankOpResult {
  Hash128 subject_hash;
  Hash128 event_id;
  uint32_t refutations_attempted = 0;
  uint32_t distinct_refuters = 0;
  uint32_t status = TANK_STATUS_PENDING;
};

std::string refinery_resolve_tank_path();
const char* refinery_tank_status_name(uint32_t status);

int refinery_tank_witness(const std::string& tank_path,
                          const std::string& ledger_path,
                          const std::string& text,
                          TankOpResult* result,
                          std::string* error);

int refinery_tank_reject(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         TankOpResult* result,
                         std::string* error);

int refinery_tank_refute(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         const std::string& evidence,
                         const std::string& refuter_id,
                         bool succeeds,
                         TankOpResult* result,
                         std::string* error);

int refinery_tank_evict(const std::string& tank_path,
                        const std::string& ledger_path,
                        const Hash128& subject_hash,
                        const Hash128& refutation_event_id,
                        TankOpResult* result,
                        std::string* error);

int refinery_tank_promote_to_substrate(const std::string& tank_path,
                                       const std::string& ledger_path,
                                       const std::string& substrate_path,
                                       const char* argv0,
                                       std::string* error);

}  // namespace refinery_forge

#endif /* REFINERY_FORGE_TANK_H */
