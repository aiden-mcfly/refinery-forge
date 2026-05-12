#include "ledger.h"

#include "xxhash.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace refinery_forge {
namespace {

void append_bytes(std::vector<unsigned char>* out, const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  out->insert(out->end(), p, p + len);
}

Hash128 compute_entry_id(uint32_t event_type,
                         const Hash128& subject_hash,
                         const Hash128& evidence_hash,
                         const Hash128& prev_entry_id,
                         uint64_t sequence) {
  std::vector<unsigned char> material;
  material.reserve(4u + (3u * sizeof(Hash128)) + sizeof(uint64_t));
  append_bytes(&material, &event_type, sizeof(event_type));
  append_bytes(&material, &subject_hash, sizeof(subject_hash));
  append_bytes(&material, &evidence_hash, sizeof(evidence_hash));
  append_bytes(&material, &prev_entry_id, sizeof(prev_entry_id));
  append_bytes(&material, &sequence, sizeof(sequence));
  return refinery_hash_bytes(material.data(), material.size());
}

int hex_value(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';
  if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';
  return -1;
}

bool parse_u64_hex(std::string_view text, uint64_t* out) {
  if (text.size() != 16u) return false;
  uint64_t value = 0;
  for (char ch : text) {
    const int nibble = hex_value(ch);
    if (nibble < 0) return false;
    value = (value << 4u) | static_cast<uint64_t>(nibble);
  }
  *out = value;
  return true;
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

int read_last_entry_locked(int fd, LedgerEntry* out, uint64_t* count, std::string* error) {
  struct stat st {};
  if (fstat(fd, &st) != 0) {
    if (error) *error = "L-R-01: failed to stat evidence ledger";
    return 5;
  }
  const off_t bytes = st.st_size;
  if (bytes < 0) {
    if (error) *error = "L-R-01: failed to stat evidence ledger";
    return 5;
  }
  const size_t entry_size = sizeof(LedgerEntry);
  const off_t rem = bytes % static_cast<off_t>(entry_size);
  if (rem != 0) {
    if (error) {
      *error = "L-R-01: torn ledger (size=" + std::to_string(static_cast<long long>(bytes)) +
               ", entry=" + std::to_string(entry_size) + ", remainder=" +
               std::to_string(static_cast<long long>(rem)) +
               "); manual repair required; refusing to append";
    }
    return 12;
  }
  *count = static_cast<uint64_t>(bytes / static_cast<off_t>(entry_size));
  if (*count == 0u) {
    std::memset(out, 0, sizeof(*out));
    return 0;
  }
  const off_t last_off = bytes - static_cast<off_t>(entry_size);
  const ssize_t got =
      pread(fd, out, static_cast<size_t>(entry_size), last_off);
  if (got != static_cast<ssize_t>(entry_size)) {
    if (error) *error = "L-R-01: failed to read last evidence ledger entry";
    return 5;
  }
  if (std::memcmp(out->magic, REFINERY_LEDGER_MAGIC_STR, REFINERY_LEDGER_MAGIC_LEN) != 0) {
    if (error) *error = "L-R-01: malformed evidence ledger";
    return 5;
  }
  return 0;
}

#ifdef REFINERY_LEDGER_DEBUG_RACE
void maybe_debug_race_delay() {
  const char* delay = std::getenv("REFINERY_LEDGER_DEBUG_RACE_DELAY_US");
  if (delay == nullptr || *delay == '\0') return;
  char* end = nullptr;
  const unsigned long value = std::strtoul(delay, &end, 10);
  if (end == nullptr || *end != '\0' || value == 0ul) return;
  usleep(static_cast<useconds_t>(value));
}
#else
void maybe_debug_race_delay() {}
#endif

}  // namespace

Hash128 refinery_hash_bytes(const void* data, size_t len) {
  const XXH128_hash_t digest = XXH3_128bits(data, len);
  Hash128 out{};
  out.low64 = digest.low64;
  out.high64 = digest.high64;
  return out;
}

Hash128 refinery_hash_string(std::string_view value) {
  return refinery_hash_bytes(value.data(), value.size());
}

Hash128 refinery_zero_hash() {
  Hash128 out{};
  return out;
}

bool refinery_hash_equal(const Hash128& a, const Hash128& b) {
  return a.low64 == b.low64 && a.high64 == b.high64;
}

bool refinery_hash_is_zero(const Hash128& value) {
  return value.low64 == 0u && value.high64 == 0u;
}

std::string refinery_hash_to_hex(const Hash128& value) {
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << value.high64
      << std::setw(16) << value.low64;
  return out.str();
}

bool refinery_parse_hash(std::string_view text, Hash128* out) {
  if (text.size() == 33u && text[16] == ':') {
    return parse_u64_hex(text.substr(0u, 16u), &out->high64) &&
           parse_u64_hex(text.substr(17u, 16u), &out->low64);
  }
  if (text.size() != 32u) return false;
  return parse_u64_hex(text.substr(0u, 16u), &out->high64) &&
         parse_u64_hex(text.substr(16u, 16u), &out->low64);
}

std::string refinery_resolve_ledger_path() {
  const char* env = std::getenv("REFINERY_EVIDENCE_LEDGER");
  if (env && *env) return env;
  const char* candidates[] = {
      ".evidence-ledger-path",
      "refinery-core/.evidence-ledger-path",
      "../refinery-core/.evidence-ledger-path",
  };
  for (const char* path_file : candidates) {
    const std::string resolved = read_path_file(path_file);
    if (!resolved.empty()) return resolved;
  }
  return {};
}

int refinery_ledger_open_locked(const std::string& ledger_path, int* out_fd, std::string* error) {
  if (ledger_path.empty()) {
    if (error) *error = "L-R-01: no evidence ledger path configured; refusing state-changing operation";
    return 5;
  }
  const int fd = open(ledger_path.c_str(), O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    if (error) *error = "L-R-01: failed to open evidence ledger";
    return 5;
  }
  if (flock(fd, LOCK_EX) != 0) {
    if (error) *error = "L-R-01: failed to lock evidence ledger";
    close(fd);
    return 5;
  }
  *out_fd = fd;
  return 0;
}

void refinery_ledger_close_locked(int fd) {
  if (fd < 0) return;
  flock(fd, LOCK_UN);
  close(fd);
}

int refinery_ledger_prepare_entry_locked(int fd, LedgerEventType event_type,
                                         const Hash128& subject_hash,
                                         const Hash128& evidence_hash,
                                         LedgerEntry* out_entry,
                                         std::string* error) {
  LedgerEntry previous{};
  uint64_t count = 0;
  const int rc = read_last_entry_locked(fd, &previous, &count, error);
  if (rc != 0) return rc;

  maybe_debug_race_delay();

  LedgerEntry entry{};
  std::memcpy(entry.magic, REFINERY_LEDGER_MAGIC_STR, REFINERY_LEDGER_MAGIC_LEN);
  entry.event_type = static_cast<uint32_t>(event_type);
  entry.pad = 0u;
  entry.subject_hash = subject_hash;
  entry.evidence_hash = evidence_hash;
  entry.prev_entry_id = (count == 0u) ? refinery_zero_hash() : previous.entry_id;
  entry.sequence = count + 1u;
  entry.entry_id = compute_entry_id(entry.event_type, entry.subject_hash,
                                    entry.evidence_hash, entry.prev_entry_id,
                                    entry.sequence);
  *out_entry = entry;
  return 0;
}

int refinery_ledger_append_entry_locked(int fd, const LedgerEntry& entry, std::string* error) {
  const off_t off = lseek(fd, 0, SEEK_END);
  if (off < 0) {
    if (error) *error = "L-R-01: failed to seek evidence ledger";
    return 5;
  }
  const ssize_t wrote = write(fd, &entry, sizeof(entry));
  if (wrote != static_cast<ssize_t>(sizeof(entry))) {
    if (error) *error = "L-R-01: failed to append evidence ledger entry";
    return 5;
  }
  if (fsync(fd) != 0) {
    if (error) *error = "L-R-01: failed to fsync evidence ledger";
    return 5;
  }
  return 0;
}

int refinery_emit_ledger_entry(const std::string& ledger_path,
                               LedgerEventType event_type,
                               const Hash128& subject_hash,
                               const Hash128& evidence_hash,
                               Hash128* out_entry_id,
                               std::string* error) {
  int fd = -1;
  int rc = refinery_ledger_open_locked(ledger_path, &fd, error);
  if (rc != 0) return rc;

  LedgerEntry entry{};
  rc = refinery_ledger_prepare_entry_locked(fd, event_type, subject_hash, evidence_hash,
                                            &entry, error);
  if (rc == 0) {
    rc = refinery_ledger_append_entry_locked(fd, entry, error);
  }
  refinery_ledger_close_locked(fd);
  if (rc == 0 && out_entry_id) *out_entry_id = entry.entry_id;
  return rc;
}

bool refinery_ledger_has_event(const std::string& ledger_path,
                               const Hash128& entry_id,
                               LedgerEventType event_type,
                               const Hash128& subject_hash) {
  std::ifstream in(ledger_path, std::ios::binary);
  if (!in) return false;
  LedgerEntry entry{};
  while (in.read(reinterpret_cast<char*>(&entry), static_cast<std::streamsize>(sizeof(entry)))) {
    if (std::memcmp(entry.magic, REFINERY_LEDGER_MAGIC_STR, REFINERY_LEDGER_MAGIC_LEN) != 0) {
      return false;
    }
    if (entry.event_type == static_cast<uint32_t>(event_type) &&
        refinery_hash_equal(entry.entry_id, entry_id) &&
        refinery_hash_equal(entry.subject_hash, subject_hash)) {
      return true;
    }
  }
  return false;
}

}  // namespace refinery_forge
