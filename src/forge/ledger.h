#ifndef REFINERY_FORGE_LEDGER_H
#define REFINERY_FORGE_LEDGER_H

#include "refinery/ledger_interface.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace refinery_forge {

Hash128 refinery_hash_bytes(const void* data, size_t len);
Hash128 refinery_hash_string(std::string_view value);
Hash128 refinery_zero_hash();
bool refinery_hash_equal(const Hash128& a, const Hash128& b);
bool refinery_hash_is_zero(const Hash128& value);
std::string refinery_hash_to_hex(const Hash128& value);
bool refinery_parse_hash(std::string_view text, Hash128* out);

std::string refinery_resolve_ledger_path();

int refinery_ledger_open_locked(const std::string& ledger_path, int* out_fd, std::string* error);
void refinery_ledger_close_locked(int fd);
int refinery_ledger_prepare_entry_locked(int fd, LedgerEventType event_type,
                                         const Hash128& subject_hash,
                                         const Hash128& evidence_hash,
                                         LedgerEntry* out_entry,
                                         std::string* error);
int refinery_ledger_append_entry_locked(int fd, const LedgerEntry& entry, std::string* error);

int refinery_emit_ledger_entry(const std::string& ledger_path,
                               LedgerEventType event_type,
                               const Hash128& subject_hash,
                               const Hash128& evidence_hash,
                               Hash128* out_entry_id,
                               std::string* error);

bool refinery_ledger_has_event(const std::string& ledger_path,
                               const Hash128& entry_id,
                               LedgerEventType event_type,
                               const Hash128& subject_hash);

}  // namespace refinery_forge

#endif /* REFINERY_FORGE_LEDGER_H */
