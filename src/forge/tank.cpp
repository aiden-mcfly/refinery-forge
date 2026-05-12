/**
 * tank.cpp — entropy-tank v2 file producer (G18 spectroscopy-reachable surface).
 *
 * On-disk v2 layout (read top-to-bottom; little-endian host):
 *   [0..64)       TankFileHeader            (magic "SELAHTN2", format_version=2, offsets)
 *   [EO..EO+EB)   TankEntry[entry_count]    (EO=header.entries_offset=64, each entry 64 bytes)
 *   [EO+EB..CO)   refuter bookkeeping       (private: 16 Hash128 slots per entry = 256 bytes)
 *                 + 8-byte alignment pad to canon_offset
 *   [CO..CO+CS)   canon UTF-8 blob          (CO=header.canon_offset, CS=header.canon_size)
 *
 * Compatibility:
 *   - Files with first-8-bytes magic == "SELAHTNK" are v1 (no header; 56-byte entry stream
 *     interleaved with 256-byte refuter blocks; total 312 bytes per record). New witness
 *     operations against a v1 tank refuse with exit 9. Promotion is allowed in
 *     identity-only mode with a stderr warning.
 *   - Files with other magic bytes refuse with exit 11.
 *
 * Concurrency:
 *   - All mutating ops hold flock(LOCK_EX) on BOTH the ledger and the tank for the full
 *     read-modify-write cycle. Lock-acquisition order is mandatory: ledger first, tank
 *     second; release in reverse (tank first, ledger second).
 *   - Create-race: open(O_CREAT) + flock + fstat double-check. Only the size==0 winner
 *     writes the zero-state header.
 *
 * Determinism (L-R-02): tank-emit may carry no time/random sources of nondeterminism.
 *   Ledger sequence + entry_id chain remain content-addressed.
 */
#include "tank.h"

#include "substrate_emit.h"
#include "xxhash.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace refinery_forge {
namespace {

constexpr size_t kMaxRefutersPerSubject = REFINERY_TANK_MAX_REFUTERS;
constexpr size_t kRefuterBlockBytes = kMaxRefutersPerSubject * sizeof(Hash128);
constexpr size_t kAlign = 8u;

/* Compile-time size assertions for the on-disk shapes we serialize. */
static_assert(sizeof(TankFileHeader) == 64u, "TankFileHeader on-disk size must be 64");
static_assert(sizeof(TankEntry) == 64u, "TankEntry on-disk size must be 64");

/* v1 legacy on-disk shapes (read-only support for backward compatibility). */
#pragma pack(push, 4)
struct V1TankEntryOnDisk {
  uint8_t magic[REFINERY_TANK_MAGIC_LEN];   /*  0..8  */
  Hash128 canonical_hash;                    /*  8..24 */
  uint32_t refutations_attempted;            /* 24..28 */
  uint32_t distinct_refuters;                /* 28..32 */
  uint32_t status;                           /* 32..36 */
  uint32_t pad;                              /* 36..40 */
  Hash128 latest_event_id;                   /* 40..56 */
};
#pragma pack(pop)
static_assert(sizeof(V1TankEntryOnDisk) == 56u, "V1TankEntryOnDisk must be 56 bytes");
constexpr size_t kV1RecordBytes = sizeof(V1TankEntryOnDisk) + kRefuterBlockBytes;
static_assert(kV1RecordBytes == 312u, "V1 record must be 312 bytes");

inline size_t align_up_8(size_t x) { return (x + (kAlign - 1u)) & ~(kAlign - 1u); }

struct TankRecord {
  TankEntry entry{};
  std::array<Hash128, kMaxRefutersPerSubject> refuters{};
  std::string signal_text;   /* in-memory copy of the canon-blob slice for this entry */
};

enum class TankFormat { Unknown, Empty, V1, V2 };

/* --------------------------------------------------------------------------
 * Locked file primitives
 * -------------------------------------------------------------------------- */

int tank_open_locked(const std::string& tank_path, int* out_fd, std::string* error) {
  if (tank_path.empty()) {
    if (error) *error = "L-F-03: no entropy tank path configured";
    return 6;
  }
  const int fd = open(tank_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    if (error) *error = "L-F-03: failed to open entropy tank";
    return 6;
  }
  if (flock(fd, LOCK_EX) != 0) {
    if (error) *error = "L-F-03: failed to lock entropy tank";
    close(fd);
    return 6;
  }
  *out_fd = fd;
  return 0;
}

void tank_close_locked(int fd) {
  if (fd < 0) return;
  flock(fd, LOCK_UN);
  close(fd);
}

int read_exact(int fd, void* buf, size_t n, off_t off, std::string* error) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  size_t remaining = n;
  off_t cur = off;
  while (remaining > 0u) {
    const ssize_t got = pread(fd, p, remaining, cur);
    if (got <= 0) {
      if (error) *error = "L-F-03: short read on entropy tank";
      return 6;
    }
    p += got;
    cur += got;
    remaining -= static_cast<size_t>(got);
  }
  return 0;
}

int classify_locked(int fd, off_t* out_size, TankFormat* out_fmt, std::string* error) {
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    if (error) *error = "L-F-03: failed to stat entropy tank";
    return 6;
  }
  *out_size = st.st_size;
  if (st.st_size == 0) {
    *out_fmt = TankFormat::Empty;
    return 0;
  }
  if (st.st_size < static_cast<off_t>(REFINERY_TANK_MAGIC_LEN)) {
    *out_fmt = TankFormat::Unknown;
    return 0;
  }
  uint8_t magic[REFINERY_TANK_MAGIC_LEN];
  if (const int rc = read_exact(fd, magic, sizeof(magic), 0, error); rc != 0) return rc;
  if (std::memcmp(magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN) == 0) {
    *out_fmt = TankFormat::V2;
  } else if (std::memcmp(magic, REFINERY_TANK_MAGIC_STR_V1, REFINERY_TANK_MAGIC_LEN) == 0) {
    *out_fmt = TankFormat::V1;
  } else {
    *out_fmt = TankFormat::Unknown;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * v2 header validator (G18 H2.4)
 * -------------------------------------------------------------------------- */

int validate_v2_header(const TankFileHeader& h, off_t file_size, std::string* error) {
  auto fail = [&](const char* reason) {
    if (error) *error = std::string("L-F-03: tank v2 header invalid: ") + reason;
    return 11;
  };
  if (std::memcmp(h.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN) != 0) {
    return fail("bad magic");
  }
  if (h.format_version != REFINERY_TANK_FORMAT_VERSION) return fail("bad format_version");
  if (h.entries_offset != sizeof(TankFileHeader)) return fail("entries_offset != 64");
  const uint64_t expected_entries_bytes =
      static_cast<uint64_t>(h.entry_count) * sizeof(TankEntry);
  if (h.entries_bytes != expected_entries_bytes) {
    return fail("entries_bytes != entry_count * 64");
  }
  if (h.canon_offset < h.entries_offset + h.entries_bytes) {
    return fail("canon_offset < entries_offset + entries_bytes");
  }
  if ((h.canon_offset % kAlign) != 0u) return fail("canon_offset not 8-aligned");
  if (file_size < 0 || static_cast<uint64_t>(file_size) != h.canon_offset + h.canon_size) {
    return fail("canon_offset + canon_size != file_size");
  }
  if (h.canon_offset + h.canon_size > REFINERY_TANK_FILE_MAX_BYTES) {
    return fail("file_size exceeds 2 GiB cap");
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * v2 read / write
 * -------------------------------------------------------------------------- */

int read_v2_image(int fd, off_t file_size, TankFileHeader* out_header,
                  std::vector<TankRecord>* out_records, std::string* error) {
  TankFileHeader header{};
  if (const int rc = read_exact(fd, &header, sizeof(header), 0, error); rc != 0) return rc;
  if (const int rc = validate_v2_header(header, file_size, error); rc != 0) return rc;
  *out_header = header;

  out_records->clear();
  out_records->resize(header.entry_count);

  const off_t entries_off = static_cast<off_t>(header.entries_offset);
  const off_t refuters_off = entries_off + static_cast<off_t>(header.entries_bytes);

  /* Per-entry: read TankEntry then its refuter block. */
  for (uint32_t i = 0u; i < header.entry_count; ++i) {
    TankRecord& rec = (*out_records)[i];
    const off_t e_off = entries_off + static_cast<off_t>(i) * static_cast<off_t>(sizeof(TankEntry));
    if (const int rc = read_exact(fd, &rec.entry, sizeof(TankEntry), e_off, error); rc != 0) {
      return rc;
    }
    if (std::memcmp(rec.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN) != 0) {
      if (error) *error = "L-F-03: tank v2 header invalid: entry magic mismatch";
      return 11;
    }
    if (rec.entry.canon_size == 0u) {
      if (error) *error = "L-F-03: tank v2 header invalid: entry canon_size == 0";
      return 11;
    }
    if (static_cast<uint64_t>(rec.entry.canon_offset) + rec.entry.canon_size > header.canon_size) {
      if (error) *error = "L-F-03: tank v2 header invalid: entry canon range exceeds canon_size";
      return 11;
    }
    if (rec.entry.pad != 0u || rec.entry.trailing_pad != 0u) {
      if (error) *error = "L-F-03: tank v2 header invalid: non-zero pad in entry";
      return 11;
    }
    const off_t r_off = refuters_off + static_cast<off_t>(i) * static_cast<off_t>(kRefuterBlockBytes);
    if (const int rc = read_exact(fd, rec.refuters.data(), kRefuterBlockBytes, r_off, error);
        rc != 0) {
      return rc;
    }
    rec.signal_text.assign(rec.entry.canon_size, '\0');
    const off_t s_off = static_cast<off_t>(header.canon_offset) +
                        static_cast<off_t>(rec.entry.canon_offset);
    if (const int rc = read_exact(fd, rec.signal_text.data(), rec.entry.canon_size, s_off, error);
        rc != 0) {
      return rc;
    }
  }
  return 0;
}

int write_v2_image_locked(int fd, std::vector<TankRecord>* records, std::string* error) {
  /* Recompute canon blob offsets from scratch so we never carry over stale offsets
   * after deletions or text changes (v1 had no such concept; v2 always normalizes). */
  std::string canon_blob;
  for (TankRecord& rec : *records) {
    if (rec.signal_text.size() > std::numeric_limits<uint16_t>::max()) {
      if (error) *error = "L-F-03: witness text exceeds 65535 bytes";
      return 11;
    }
    if (rec.signal_text.empty()) {
      if (error) *error = "L-F-03: tank entry signal_text empty";
      return 11;
    }
    if (canon_blob.size() > std::numeric_limits<uint32_t>::max() - rec.signal_text.size()) {
      if (error) *error = "L-F-03: canon blob exceeds uint32_t addressable range";
      return 11;
    }
    rec.entry.canon_offset = static_cast<uint32_t>(canon_blob.size());
    rec.entry.canon_size = static_cast<uint16_t>(rec.signal_text.size());
    rec.entry.pad = 0u;
    rec.entry.trailing_pad = 0u;
    std::memcpy(rec.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN);
    canon_blob.append(rec.signal_text);
  }

  const uint64_t entries_offset = sizeof(TankFileHeader);
  const uint64_t entries_bytes =
      static_cast<uint64_t>(records->size()) * sizeof(TankEntry);
  const uint64_t refuters_bytes =
      static_cast<uint64_t>(records->size()) * kRefuterBlockBytes;
  const uint64_t pre_canon = entries_offset + entries_bytes + refuters_bytes;
  const uint64_t canon_offset = align_up_8(pre_canon);
  const uint64_t canon_size = canon_blob.size();
  const uint64_t total = canon_offset + canon_size;
  if (total > REFINERY_TANK_FILE_MAX_BYTES) {
    if (error) *error = "L-F-03: tank file would exceed 2 GiB cap";
    return 11;
  }

  TankFileHeader header{};
  std::memcpy(header.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN);
  header.format_version = REFINERY_TANK_FORMAT_VERSION;
  header.entry_count = static_cast<uint32_t>(records->size());
  header.entries_offset = entries_offset;
  header.entries_bytes = entries_bytes;
  header.canon_offset = canon_offset;
  header.canon_size = canon_size;
  std::memset(header.reserved, 0, sizeof(header.reserved));

  std::vector<uint8_t> buf(static_cast<size_t>(total), 0u);
  std::memcpy(buf.data(), &header, sizeof(header));
  for (size_t i = 0u; i < records->size(); ++i) {
    const TankRecord& rec = (*records)[i];
    std::memcpy(buf.data() + entries_offset + i * sizeof(TankEntry), &rec.entry,
                sizeof(TankEntry));
    std::memcpy(buf.data() + entries_offset + entries_bytes + i * kRefuterBlockBytes,
                rec.refuters.data(), kRefuterBlockBytes);
    std::memcpy(buf.data() + canon_offset + rec.entry.canon_offset, rec.signal_text.data(),
                rec.signal_text.size());
  }

  if (ftruncate(fd, 0) != 0) {
    if (error) *error = "L-F-03: failed to truncate entropy tank";
    return 6;
  }
  if (lseek(fd, 0, SEEK_SET) < 0) {
    if (error) *error = "L-F-03: failed to seek entropy tank";
    return 6;
  }
  size_t written = 0u;
  while (written < buf.size()) {
    const ssize_t w = write(fd, buf.data() + written, buf.size() - written);
    if (w <= 0) {
      if (errno == EINTR) continue;
      if (error) *error = "L-F-03: failed to write entropy tank";
      return 6;
    }
    written += static_cast<size_t>(w);
  }
  if (fsync(fd) != 0) {
    if (error) *error = "L-F-03: failed to fsync entropy tank";
    return 6;
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * v1 read (legacy; identity-only promotion only)
 * -------------------------------------------------------------------------- */

int read_v1_records(int fd, off_t file_size, std::vector<TankRecord>* out_records,
                    std::string* error) {
  if (file_size < 0 ||
      static_cast<uint64_t>(file_size) % kV1RecordBytes != 0u) {
    if (error) *error = "L-F-03: malformed v1 tank (size not a multiple of 312 bytes)";
    return 6;
  }
  const uint64_t count = static_cast<uint64_t>(file_size) / kV1RecordBytes;
  out_records->clear();
  out_records->reserve(static_cast<size_t>(count));
  off_t off = 0;
  for (uint64_t i = 0u; i < count; ++i) {
    V1TankEntryOnDisk v1{};
    if (const int rc = read_exact(fd, &v1, sizeof(v1), off, error); rc != 0) return rc;
    if (std::memcmp(v1.magic, REFINERY_TANK_MAGIC_STR_V1, REFINERY_TANK_MAGIC_LEN) != 0) {
      if (error) *error = "L-F-03: malformed v1 tank (entry magic mismatch)";
      return 6;
    }
    TankRecord rec{};
    std::memcpy(rec.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN);
    rec.entry.canonical_hash = v1.canonical_hash;
    rec.entry.refutations_attempted = v1.refutations_attempted;
    rec.entry.distinct_refuters = v1.distinct_refuters;
    rec.entry.status = v1.status;
    rec.entry.latest_event_id = v1.latest_event_id;
    rec.entry.canon_offset = 0u;
    rec.entry.canon_size = 0u;
    if (const int rc = read_exact(fd, rec.refuters.data(), kRefuterBlockBytes,
                                  off + static_cast<off_t>(sizeof(v1)), error); rc != 0) {
      return rc;
    }
    out_records->push_back(std::move(rec));
    off += static_cast<off_t>(kV1RecordBytes);
  }
  return 0;
}

/* --------------------------------------------------------------------------
 * record helpers (memory)
 * -------------------------------------------------------------------------- */

TankRecord* find_record(std::vector<TankRecord>* records, const Hash128& subject_hash) {
  for (TankRecord& rec : *records) {
    if (refinery_hash_equal(rec.entry.canonical_hash, subject_hash)) return &rec;
  }
  return nullptr;
}

bool has_refuter(const TankRecord& rec, const Hash128& refuter_hash) {
  const uint32_t n = rec.entry.distinct_refuters < kMaxRefutersPerSubject
                         ? rec.entry.distinct_refuters
                         : static_cast<uint32_t>(kMaxRefutersPerSubject);
  for (uint32_t i = 0u; i < n; ++i) {
    if (refinery_hash_equal(rec.refuters[i], refuter_hash)) return true;
  }
  return false;
}

bool add_refuter(TankRecord* rec, const Hash128& refuter_hash) {
  if (has_refuter(*rec, refuter_hash)) return true;
  if (rec->entry.distinct_refuters >= kMaxRefutersPerSubject) return false;
  rec->refuters[rec->entry.distinct_refuters] = refuter_hash;
  rec->entry.distinct_refuters += 1u;
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

void apply_un_refuted_threshold(TankRecord* rec) {
  const uint32_t min_attempts = env_threshold("REFINERY_MIN_REFUTATIONS_ATTEMPTED",
                                              REFINERY_MIN_REFUTATIONS_ATTEMPTED_DEFAULT);
  const uint32_t min_distinct = env_threshold("REFINERY_MIN_DISTINCT_REFUTERS",
                                              REFINERY_MIN_DISTINCT_REFUTERS_DEFAULT);
  if (rec->entry.status != TANK_STATUS_REFUTED &&
      rec->entry.refutations_attempted >= min_attempts &&
      rec->entry.distinct_refuters >= min_distinct) {
    rec->entry.status = TANK_STATUS_UN_REFUTED;
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

void fill_result(const TankRecord& rec, const Hash128& event_id, TankOpResult* result) {
  if (result == nullptr) return;
  result->subject_hash = rec.entry.canonical_hash;
  result->event_id = event_id;
  result->refutations_attempted = rec.entry.refutations_attempted;
  result->distinct_refuters = rec.entry.distinct_refuters;
  result->status = rec.entry.status;
}

int require_paths(const std::string& tank_path, const std::string& ledger_path,
                  std::string* error) {
  if (ledger_path.empty()) {
    if (error) *error =
        "L-R-01: no evidence ledger path configured; refusing state-changing operation";
    return 5;
  }
  if (tank_path.empty()) {
    if (error) *error = "L-F-03: no entropy tank path configured";
    return 6;
  }
  return 0;
}

/* Load the tank under an already-held flock. Behavior depends on format:
 *   - Empty: out_records is empty, *out_v1 false. Caller may witness/refute/etc.
 *   - V2:    parsed via validator. Caller may mutate and re-write.
 *   - V1:    parsed in identity-only mode. Caller MUST treat as read-only for new
 *            witness/refute/reject/evict ops (we surface the refusal here as exit 9).
 *   - Other: refuse with exit 11. */
int load_for_mutation(int fd, std::vector<TankRecord>* out_records,
                      TankFileHeader* out_header, std::string* error) {
  off_t file_size = 0;
  TankFormat fmt = TankFormat::Unknown;
  if (const int rc = classify_locked(fd, &file_size, &fmt, error); rc != 0) return rc;
  if (fmt == TankFormat::Empty) {
    out_records->clear();
    std::memset(out_header, 0, sizeof(*out_header));
    return 0;
  }
  if (fmt == TankFormat::V2) {
    return read_v2_image(fd, file_size, out_header, out_records, error);
  }
  if (fmt == TankFormat::V1) {
    if (error) *error =
        "L-F-03: tank v1 cannot accept new entries; migrate or recreate";
    return 9;
  }
  if (error) *error = "L-F-03: unknown tank format magic";
  return 11;
}

int load_for_promote(int fd, std::vector<TankRecord>* out_records,
                     TankFormat* out_fmt, std::string* error) {
  off_t file_size = 0;
  TankFormat fmt = TankFormat::Unknown;
  if (const int rc = classify_locked(fd, &file_size, &fmt, error); rc != 0) return rc;
  *out_fmt = fmt;
  if (fmt == TankFormat::Empty) {
    out_records->clear();
    return 0;
  }
  if (fmt == TankFormat::V2) {
    TankFileHeader header{};
    return read_v2_image(fd, file_size, &header, out_records, error);
  }
  if (fmt == TankFormat::V1) {
    return read_v1_records(fd, file_size, out_records, error);
  }
  if (error) *error = "L-F-03: unknown tank format magic";
  return 11;
}

/* --------------------------------------------------------------------------
 * mutation primitives (executed under BOTH ledger flock and tank flock)
 * -------------------------------------------------------------------------- */

struct EventPlan {
  LedgerEventType type{};
  Hash128 subject{};
  Hash128 evidence{};
};

int run_state_change(const std::string& ledger_path, const std::string& tank_path,
                     const EventPlan& plan,
                     const std::function<int(int /*tank_fd*/,
                                             std::vector<TankRecord>* /*records*/,
                                             const Hash128& /*event_id*/,
                                             TankOpResult* /*out_result*/,
                                             std::string* /*error*/)>& mutate,
                     TankOpResult* result, std::string* error) {
  if (const int rc = require_paths(tank_path, ledger_path, error); rc != 0) return rc;

  int ledger_fd = -1;
  int rc = refinery_ledger_open_locked(ledger_path, &ledger_fd, error);
  if (rc != 0) return rc;

  LedgerEntry entry{};
  rc = refinery_ledger_prepare_entry_locked(ledger_fd, plan.type, plan.subject, plan.evidence,
                                            &entry, error);
  if (rc != 0) {
    refinery_ledger_close_locked(ledger_fd);
    return rc;
  }

  int tank_fd = -1;
  rc = tank_open_locked(tank_path, &tank_fd, error);
  if (rc != 0) {
    refinery_ledger_close_locked(ledger_fd);
    return rc;
  }

  std::vector<TankRecord> records;
  TankFileHeader header{};
  rc = load_for_mutation(tank_fd, &records, &header, error);
  if (rc != 0) {
    tank_close_locked(tank_fd);
    refinery_ledger_close_locked(ledger_fd);
    return rc;
  }

  rc = mutate(tank_fd, &records, entry.entry_id, result, error);
  if (rc != 0) {
    tank_close_locked(tank_fd);
    refinery_ledger_close_locked(ledger_fd);
    return rc;
  }

  rc = write_v2_image_locked(tank_fd, &records, error);
  if (rc != 0) {
    tank_close_locked(tank_fd);
    refinery_ledger_close_locked(ledger_fd);
    return rc;
  }

  rc = refinery_ledger_append_entry_locked(ledger_fd, entry, error);
  tank_close_locked(tank_fd);
  refinery_ledger_close_locked(ledger_fd);
  return rc;
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
    case TANK_STATUS_PENDING:    return "PENDING";
    case TANK_STATUS_UN_REFUTED: return "UN_REFUTED";
    case TANK_STATUS_REFUTED:    return "REFUTED";
    default:                     return "UNKNOWN";  /* L-07: caller surface; not a routing branch */
  }
}

int refinery_tank_witness(const std::string& tank_path,
                          const std::string& ledger_path,
                          const std::string& text,
                          TankOpResult* result,
                          std::string* error) {
  if (text.empty()) {
    if (error) *error = "L-F-03: witness text empty";
    return 11;
  }
  if (text.size() > std::numeric_limits<uint16_t>::max()) {
    if (error) *error = "L-F-03: witness text exceeds 65535 bytes";
    return 11;
  }
  const Hash128 subject_hash = refinery_hash_string(text);
  EventPlan plan{LEDGER_EVENT_TESTIMONY, subject_hash, refinery_zero_hash()};
  return run_state_change(ledger_path, tank_path, plan,
      [&](int /*tank_fd*/, std::vector<TankRecord>* records, const Hash128& event_id,
          TankOpResult* res, std::string* err) -> int {
        TankRecord* rec = find_record(records, subject_hash);
        if (rec == nullptr) {
          TankRecord fresh{};
          std::memcpy(fresh.entry.magic, REFINERY_TANK_MAGIC_STR, REFINERY_TANK_MAGIC_LEN);
          fresh.entry.canonical_hash = subject_hash;
          fresh.entry.status = TANK_STATUS_PENDING;
          fresh.signal_text = text;
          records->push_back(std::move(fresh));
          rec = &records->back();
        } else {
          /* Re-witness preserves text (already on file). */
          if (rec->signal_text.empty()) rec->signal_text = text;
        }
        rec->entry.latest_event_id = event_id;
        fill_result(*rec, event_id, res);
        (void)err;
        return 0;
      },
      result, error);
}

int refinery_tank_reject(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         TankOpResult* result,
                         std::string* error) {
  EventPlan plan{LEDGER_EVENT_REJECTION, subject_hash, refinery_zero_hash()};
  return run_state_change(ledger_path, tank_path, plan,
      [&](int /*tank_fd*/, std::vector<TankRecord>* records, const Hash128& event_id,
          TankOpResult* res, std::string* err) -> int {
        TankRecord* rec = find_record(records, subject_hash);
        if (rec == nullptr) {
          if (err) *err = "L-F-03: reject target subject not present in tank";
          return 6;
        }
        rec->entry.latest_event_id = event_id;
        fill_result(*rec, event_id, res);
        return 0;
      },
      result, error);
}

int refinery_tank_refute(const std::string& tank_path,
                         const std::string& ledger_path,
                         const Hash128& subject_hash,
                         const std::string& evidence,
                         const std::string& refuter_id,
                         bool succeeds,
                         TankOpResult* result,
                         std::string* error) {
  if (refuter_id.empty()) {
    if (error) *error = "L-F-03: refuter identity required";
    return 7;
  }
  const Hash128 refuter_hash = refinery_hash_string(refuter_id);
  const Hash128 evidence_hash = refinery_hash_string(evidence);
  EventPlan plan{succeeds ? LEDGER_EVENT_REFUTATION_SUCCEEDED : LEDGER_EVENT_REFUTATION_ATTEMPTED,
                 subject_hash, evidence_hash};
  return run_state_change(ledger_path, tank_path, plan,
      [&](int /*tank_fd*/, std::vector<TankRecord>* records, const Hash128& event_id,
          TankOpResult* res, std::string* err) -> int {
        TankRecord* rec = find_record(records, subject_hash);
        if (rec == nullptr) {
          if (err) *err = "L-F-03: refute target subject not present in tank";
          return 6;
        }
        if (!has_refuter(*rec, refuter_hash) &&
            rec->entry.distinct_refuters >= kMaxRefutersPerSubject) {
          if (err) *err = "L-F-03: distinct refuter capacity exceeded (max 16 per subject)";
          return 7;
        }
        rec->entry.refutations_attempted += 1u;
        if (!add_refuter(rec, refuter_hash)) {
          if (err) *err = "L-F-03: distinct refuter capacity exceeded (max 16 per subject)";
          return 7;
        }
        rec->entry.latest_event_id = event_id;
        if (succeeds) {
          rec->entry.status = TANK_STATUS_REFUTED;
        } else {
          apply_un_refuted_threshold(rec);
        }
        fill_result(*rec, event_id, res);
        return 0;
      },
      result, error);
}

int refinery_tank_evict(const std::string& tank_path,
                        const std::string& ledger_path,
                        const Hash128& subject_hash,
                        const Hash128& refutation_event_id,
                        TankOpResult* result,
                        std::string* error) {
  if (refinery_hash_is_zero(refutation_event_id) ||
      !refinery_ledger_has_event(ledger_path, refutation_event_id,
                                 LEDGER_EVENT_REFUTATION_SUCCEEDED, subject_hash)) {
    if (error) *error =
        "L-F-03: eviction requires --refutation-event-id back-reference; audit chain break refused";
    return 7;
  }
  EventPlan plan{LEDGER_EVENT_EVICTION, subject_hash, refutation_event_id};
  return run_state_change(ledger_path, tank_path, plan,
      [&](int /*tank_fd*/, std::vector<TankRecord>* records, const Hash128& event_id,
          TankOpResult* res, std::string* err) -> int {
        TankRecord* rec = find_record(records, subject_hash);
        if (rec == nullptr) {
          if (err) *err = "L-F-03: evict target subject not present in tank";
          return 6;
        }
        rec->entry.status = TANK_STATUS_REFUTED;
        rec->entry.latest_event_id = event_id;
        fill_result(*rec, event_id, res);
        return 0;
      },
      result, error);
}

/* --------------------------------------------------------------------------
 * Promotion: tank → substrate
 * -------------------------------------------------------------------------- */

namespace {

int promote_v1_identity_only(std::vector<TankRecord>* records, const std::string& substrate_path,
                             const char* argv0, std::string* error) {
  std::fprintf(stderr, "L-F-03: tank format v1 detected; identity-only promotion mode\n");

  /* Collect UN_REFUTED entries; refuse PENDING per G17 doctrine. */
  std::vector<Hash128> hashes;
  for (const TankRecord& rec : *records) {
    if (rec.entry.status == TANK_STATUS_PENDING) {
      if (error) *error = "L-F-03: signal status=PENDING; insufficient refutation coverage";
      return 8;
    }
  }
  for (const TankRecord& rec : *records) {
    if (rec.entry.status == TANK_STATUS_UN_REFUTED) hashes.push_back(rec.entry.canonical_hash);
  }
  if (hashes.empty()) {
    if (error) *error = "L-F-03: no UN_REFUTED entries available for promotion";
    return 8;
  }

  std::vector<Marker> markers;
  markers.reserve(hashes.size());
  for (const Hash128& h : hashes) {
    Marker m{};
    m.hash_id = h;
    m.canon_offset = 0u;
    m.span_bytes = 13u;
    m.pad = 0u;
    markers.push_back(m);
  }
  std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
    if (a.hash_id.high64 != b.hash_id.high64) return a.hash_id.high64 < b.hash_id.high64;
    return a.hash_id.low64 < b.hash_id.low64;
  });
  /* Dedupe by hash_id (forge behavior; mirrors G17 sort+unique semantics). */
  markers.erase(std::unique(markers.begin(), markers.end(),
                            [](const Marker& a, const Marker& b) {
                              return a.hash_id.high64 == b.hash_id.high64 &&
                                     a.hash_id.low64 == b.hash_id.low64;
                            }),
                markers.end());

  const std::string canon = "tank-promoted";
  const uint64_t canon_xxh =
      static_cast<uint64_t>(XXH64(canon.data(), canon.size(), 0));
  return refinery_write_marker_index_file(substrate_path.c_str(), canon, markers, canon_xxh,
                                          argv0, error);
}

int promote_v2_single_witness(std::vector<TankRecord>* records, const std::string& substrate_path,
                              const char* argv0, std::string* error) {
  /* Refuse if any entry is PENDING (G17 doctrine: insufficient coverage). */
  size_t un_refuted = 0u;
  size_t un_refuted_idx = 0u;
  for (size_t i = 0u; i < records->size(); ++i) {
    const TankRecord& rec = (*records)[i];
    if (rec.entry.status == TANK_STATUS_PENDING) {
      if (error) *error = "L-F-03: signal status=PENDING; insufficient refutation coverage";
      return 8;
    }
    if (rec.entry.status == TANK_STATUS_UN_REFUTED) {
      un_refuted += 1u;
      un_refuted_idx = i;
    }
  }
  if (un_refuted == 0u) {
    if (error) *error = "L-F-03: no UN_REFUTED entries available for promotion";
    return 8;
  }
  if (un_refuted > 1u) {
    if (error) *error =
        "L-F-03: multi-entry tank promotion not specified; pin canonical aggregation "
        "rule in a follow-up event before retrying";
    return 10;
  }
  /* Single-witness parity: route through the shared text→substrate helper so output
   * is byte-identical to `--text "<same canon>" --out ...`. */
  const std::string& text = (*records)[un_refuted_idx].signal_text;
  return refinery_emit_substrate_from_text(text, substrate_path.c_str(), argv0, nullptr, error);
}

}  // namespace

int refinery_tank_promote_to_substrate(const std::string& tank_path,
                                       const std::string& ledger_path,
                                       const std::string& substrate_path,
                                       const char* argv0,
                                       std::string* error) {
  if (tank_path.empty()) {
    if (error) *error = "L-F-03: no entropy tank path configured";
    return 6;
  }

  int tank_fd = -1;
  int rc = tank_open_locked(tank_path, &tank_fd, error);
  if (rc != 0) return rc;

  std::vector<TankRecord> records;
  TankFormat fmt = TankFormat::Unknown;
  rc = load_for_promote(tank_fd, &records, &fmt, error);
  if (rc != 0) {
    tank_close_locked(tank_fd);
    return rc;
  }

  if (fmt == TankFormat::Empty) {
    if (error) *error = "L-F-03: tank file is empty; nothing to promote";
    tank_close_locked(tank_fd);
    return 8;
  }

  try {
    if (fmt == TankFormat::V1) {
      rc = promote_v1_identity_only(&records, substrate_path, argv0, error);
    } else if (fmt == TankFormat::V2) {
      rc = promote_v2_single_witness(&records, substrate_path, argv0, error);
    } else {
      if (error) *error = "L-F-03: unknown tank format magic";
      rc = 11;
    }
  } catch (const std::exception& ex) {
    if (error) *error = std::string("L-F-03: promote failed: ") + ex.what();
    rc = 1;
  }
  tank_close_locked(tank_fd);

  /* Record promotion event in ledger (informational) — only on success and only if
   * ledger path is configured. Failure here doesn't roll back the substrate write,
   * but ledger emission errors propagate. */
  if (rc == 0 && !ledger_path.empty()) {
    Hash128 promotion_event_id{};
    const Hash128 subject = refinery_hash_string(tank_path);
    const Hash128 evidence = refinery_hash_string(substrate_path);
    std::string ledger_err;
    const int ledger_rc = refinery_emit_ledger_entry(
        ledger_path, LEDGER_EVENT_PROMOTION, subject, evidence,
        &promotion_event_id, &ledger_err);
    if (ledger_rc != 0) {
      if (error) *error = ledger_err;
      return ledger_rc;
    }
  }
  return rc;
}

}  // namespace refinery_forge
