#include "tank.h"

#include <array>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace refinery_forge {
namespace {

constexpr size_t kMaxRefutersPerSubject = 16u;

struct TankDiskRecord {
  TankEntry entry;
  std::array<Hash128, kMaxRefutersPerSubject> refuters;
};

std::vector<TankDiskRecord> load_records(const std::string& tank_path) {
  std::vector<TankDiskRecord> records;
  std::ifstream in(tank_path, std::ios::binary);
  if (!in) return records;
  TankDiskRecord record{};
  while (in.read(reinterpret_cast<char*>(&record), static_cast<std::streamsize>(sizeof(record)))) {
    if (std::memcmp(record.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN) != 0) {
      throw std::runtime_error("L-F-03: malformed entropy tank");
    }
    records.push_back(record);
  }
  if (!in.eof()) {
    throw std::runtime_error("L-F-03: malformed entropy tank");
  }
  return records;
}

bool save_records(const std::string& tank_path, const std::vector<TankDiskRecord>& records) {
  std::ofstream out(tank_path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  for (const auto& record : records) {
    out.write(reinterpret_cast<const char*>(&record), static_cast<std::streamsize>(sizeof(record)));
  }
  out.flush();
  return static_cast<bool>(out);
}

TankDiskRecord make_record(const Hash128& subject_hash) {
  TankDiskRecord record{};
  std::memcpy(record.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN);
  record.entry.canonical_hash = subject_hash;
  record.entry.status = TANK_STATUS_PENDING;
  return record;
}

TankDiskRecord* find_record(std::vector<TankDiskRecord>* records, const Hash128& subject_hash) {
  for (auto& record : *records) {
    if (refinery_hash_equal(record.entry.canonical_hash, subject_hash)) return &record;
  }
  return nullptr;
}

bool has_refuter(const TankDiskRecord& record, const Hash128& refuter_hash) {
  for (uint32_t i = 0u; i < record.entry.distinct_refuters && i < kMaxRefutersPerSubject; ++i) {
    if (refinery_hash_equal(record.refuters[i], refuter_hash)) return true;
  }
  return false;
}

bool add_refuter(TankDiskRecord* record, const Hash128& refuter_hash) {
  if (has_refuter(*record, refuter_hash)) return true;
  if (record->entry.distinct_refuters >= kMaxRefutersPerSubject) {
    return false;
  }
  if (record->entry.distinct_refuters < kMaxRefutersPerSubject) {
    record->refuters[record->entry.distinct_refuters] = refuter_hash;
  }
  record->entry.distinct_refuters += 1u;
  return true;
}

uint32_t env_threshold(const char* name, uint32_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == nullptr || *end != '\0' || parsed == 0ul || parsed > UINT32_MAX) return fallback;
  return static_cast<uint32_t>(parsed);
}

void apply_un_refuted_threshold(TankDiskRecord* record) {
  const uint32_t min_attempts =
      env_threshold("REFINERY_MIN_REFUTATIONS_ATTEMPTED",
                    REFINERY_MIN_REFUTATIONS_ATTEMPTED_DEFAULT);
  const uint32_t min_distinct =
      env_threshold("REFINERY_MIN_DISTINCT_REFUTERS",
                    REFINERY_MIN_DISTINCT_REFUTERS_DEFAULT);
  if (record->entry.status != TANK_STATUS_REFUTED &&
      record->entry.refutations_attempted >= min_attempts &&
      record->entry.distinct_refuters >= min_distinct) {
    record->entry.status = TANK_STATUS_UN_REFUTED;
  }
}

std::string trim_line(std::string line) {
  const size_t first = line.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = line.find_last_not_of(" \t\r\n");
  return line.substr(first, last - first + 1u);
}

std::string read_path_file(const char* path_file) {
  std::ifstream in(path_file);
  if (!in) return {};
  std::string line;
  while (std::getline(in, line)) {
    line = trim_line(line);
    if (line.empty() || line[0] == '#') continue;
    return line;
  }
  return {};
}

void fill_result(const TankDiskRecord& record, const Hash128& event_id, TankOpResult* result) {
  if (result == nullptr) return;
  result->subject_hash = record.entry.canonical_hash;
  result->event_id = event_id;
  result->refutations_attempted = record.entry.refutations_attempted;
  result->distinct_refuters = record.entry.distinct_refuters;
  result->status = record.entry.status;
}

int require_paths(const std::string& tank_path, const std::string& ledger_path, std::string* error) {
  if (ledger_path.empty()) {
    if (error) *error = "L-R-01: no evidence ledger path configured; refusing state-changing operation";
    return 5;
  }
  if (tank_path.empty()) {
    if (error) *error = "L-F-03: no entropy tank path configured";
    return 6;
  }
  return 0;
}

}  // namespace

std::string refinery_resolve_tank_path() {
  const char* env = std::getenv("REFINERY_ENTROPY_TANK");
  if (env && *env) return env;
  const char* candidates[] = {
      ".entropy-tank-path",
      "refinery-core/.entropy-tank-path",
      "../refinery-core/.entropy-tank-path",
  };
  for (const char* path_file : candidates) {
    const std::string resolved = read_path_file(path_file);
    if (!resolved.empty()) return resolved;
  }
  return {};
}

const char* refinery_tank_status_name(uint32_t status) {
  switch (status) {
    case TANK_STATUS_PENDING:
      return "PENDING";
    case TANK_STATUS_UN_REFUTED:
      return "UN_REFUTED";
    case TANK_STATUS_REFUTED:
      return "REFUTED";
    default:
      return "UNKNOWN";
  }
}

int refinery_tank_witness(const std::string& tank_path,
                          const std::string& ledger_path,
                          const std::string& text,
                          TankOpResult* result,
                          std::string* error) {
  if (const int rc = require_paths(tank_path, ledger_path, error); rc != 0) return rc;
  const Hash128 subject_hash = refinery_hash_string(text);
  Hash128 event_id{};
  const int ledger_rc = refinery_emit_ledger_entry(ledger_path, LEDGER_EVENT_TESTIMONY,
                                                   subject_hash, refinery_zero_hash(),
                                                   &event_id, error);
  if (ledger_rc != 0) return ledger_rc;
  auto records = load_records(tank_path);
  TankDiskRecord* record = find_record(&records, subject_hash);
  if (record == nullptr) {
    records.push_back(make_record(subject_hash));
    record = &records.back();
  }
  record->entry.latest_event_id = event_id;
  if (!save_records(tank_path, records)) {
    if (error) *error = "L-F-03: failed to write entropy tank";
    return 6;
  }
  fill_result(*record, event_id, result);
  return 0;
}

int refinery_tank_reject(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         TankOpResult* result,
                         std::string* error) {
  if (const int rc = require_paths(tank_path, ledger_path, error); rc != 0) return rc;
  Hash128 event_id{};
  const int ledger_rc = refinery_emit_ledger_entry(ledger_path, LEDGER_EVENT_REJECTION,
                                                   subject_hash, refinery_zero_hash(),
                                                   &event_id, error);
  if (ledger_rc != 0) return ledger_rc;
  auto records = load_records(tank_path);
  TankDiskRecord* record = find_record(&records, subject_hash);
  if (record == nullptr) {
    records.push_back(make_record(subject_hash));
    record = &records.back();
  }
  record->entry.latest_event_id = event_id;
  if (!save_records(tank_path, records)) {
    if (error) *error = "L-F-03: failed to write entropy tank";
    return 6;
  }
  fill_result(*record, event_id, result);
  return 0;
}

int refinery_tank_refute(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         const std::string& evidence,
                         const std::string& refuter_id,
                         bool succeeds,
                         TankOpResult* result,
                         std::string* error) {
  if (const int rc = require_paths(tank_path, ledger_path, error); rc != 0) return rc;
  if (refuter_id.empty()) {
    if (error) *error = "L-F-03: refuter identity required";
    return 7;
  }
  auto records = load_records(tank_path);
  TankDiskRecord* record = find_record(&records, subject_hash);
  if (record == nullptr) {
    records.push_back(make_record(subject_hash));
    record = &records.back();
  }
  const Hash128 refuter_hash = refinery_hash_string(refuter_id);
  if (!has_refuter(*record, refuter_hash) &&
      record->entry.distinct_refuters >= kMaxRefutersPerSubject) {
    if (error) *error = "L-F-03: distinct refuter capacity exceeded (max 16 per subject)";
    return 7;
  }
  const Hash128 evidence_hash = refinery_hash_string(evidence);
  Hash128 event_id{};
  const auto event_type =
      succeeds ? LEDGER_EVENT_REFUTATION_SUCCEEDED : LEDGER_EVENT_REFUTATION_ATTEMPTED;
  const int ledger_rc = refinery_emit_ledger_entry(ledger_path, event_type, subject_hash,
                                                   evidence_hash, &event_id, error);
  if (ledger_rc != 0) return ledger_rc;
  record->entry.refutations_attempted += 1u;
  if (!add_refuter(record, refuter_hash)) {
    if (error) *error = "L-F-03: distinct refuter capacity exceeded (max 16 per subject)";
    return 7;
  }
  record->entry.latest_event_id = event_id;
  if (succeeds) {
    record->entry.status = TANK_STATUS_REFUTED;
  } else {
    apply_un_refuted_threshold(record);
  }
  if (!save_records(tank_path, records)) {
    if (error) *error = "L-F-03: failed to write entropy tank";
    return 6;
  }
  fill_result(*record, event_id, result);
  return 0;
}

int refinery_tank_evict(const std::string& tank_path,
                        const std::string& ledger_path,
                        const Hash128& subject_hash,
                        const Hash128& refutation_event_id,
                        TankOpResult* result,
                        std::string* error) {
  if (const int rc = require_paths(tank_path, ledger_path, error); rc != 0) return rc;
  if (refinery_hash_is_zero(refutation_event_id) ||
      !refinery_ledger_has_event(ledger_path, refutation_event_id,
                                 LEDGER_EVENT_REFUTATION_SUCCEEDED, subject_hash)) {
    if (error) *error = "L-F-03: eviction requires --refutation-event-id back-reference; audit chain break refused";
    return 7;
  }
  Hash128 event_id{};
  const int ledger_rc = refinery_emit_ledger_entry(ledger_path, LEDGER_EVENT_EVICTION,
                                                   subject_hash, refutation_event_id,
                                                   &event_id, error);
  if (ledger_rc != 0) return ledger_rc;
  auto records = load_records(tank_path);
  TankDiskRecord* record = find_record(&records, subject_hash);
  if (record == nullptr) {
    records.push_back(make_record(subject_hash));
    record = &records.back();
  }
  record->entry.status = TANK_STATUS_REFUTED;
  record->entry.latest_event_id = event_id;
  if (!save_records(tank_path, records)) {
    if (error) *error = "L-F-03: failed to write entropy tank";
    return 6;
  }
  fill_result(*record, event_id, result);
  return 0;
}

int refinery_tank_promotable_hashes(const std::string& tank_path,
                                    std::vector<Hash128>* out,
                                    std::string* error) {
  if (tank_path.empty()) {
    if (error) *error = "L-F-03: no entropy tank path configured";
    return 6;
  }
  auto records = load_records(tank_path);
  for (const auto& record : records) {
    if (record.entry.status == TANK_STATUS_PENDING) {
      if (error) *error = "L-F-03: signal status=PENDING; insufficient refutation coverage";
      return 8;
    }
  }
  for (const auto& record : records) {
    if (record.entry.status == TANK_STATUS_UN_REFUTED) {
      out->push_back(record.entry.canonical_hash);
    }
  }
  if (out->empty()) {
    if (error) *error = "L-F-03: no UN_REFUTED entries available for promotion";
    return 8;
  }
  return 0;
}

}  // namespace refinery_forge
