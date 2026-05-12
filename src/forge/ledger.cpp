#include "ledger.h"

#include "xxhash.h"

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace refinery_forge {
namespace {

void append_bytes(std::vector<unsigned char>* out, const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  out->insert(out->end(), p, p + len);
}

bool read_last_entry(const std::string& path, LedgerEntry* out, uint64_t* count) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *count = 0;
    std::memset(out, 0, sizeof(*out));
    return true;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff bytes = in.tellg();
  if (bytes < 0 || (bytes % static_cast<std::streamoff>(sizeof(LedgerEntry))) != 0) {
    return false;
  }
  *count = static_cast<uint64_t>(bytes / static_cast<std::streamoff>(sizeof(LedgerEntry)));
  if (*count == 0u) {
    std::memset(out, 0, sizeof(*out));
    return true;
  }
  in.seekg(static_cast<std::streamoff>((*count - 1u) * sizeof(LedgerEntry)), std::ios::beg);
  in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(sizeof(*out)));
  return static_cast<bool>(in);
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

int refinery_emit_ledger_entry(const std::string& ledger_path,
                               LedgerEventType event_type,
                               const Hash128& subject_hash,
                               const Hash128& evidence_hash,
                               Hash128* out_entry_id,
                               std::string* error) {
  if (ledger_path.empty()) {
    if (error) *error = "L-R-01: no evidence ledger path configured; refusing state-changing operation";
    return 5;
  }

  LedgerEntry previous{};
  uint64_t count = 0;
  if (!read_last_entry(ledger_path, &previous, &count)) {
    if (error) *error = "L-R-01: malformed evidence ledger";
    return 5;
  }

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

  std::ofstream out(ledger_path, std::ios::binary | std::ios::app);
  if (!out) {
    if (error) *error = "L-R-01: failed to open evidence ledger";
    return 5;
  }
  out.write(reinterpret_cast<const char*>(&entry), static_cast<std::streamsize>(sizeof(entry)));
  out.flush();
  if (!out) {
    if (error) *error = "L-R-01: failed to append evidence ledger entry";
    return 5;
  }
  if (out_entry_id) *out_entry_id = entry.entry_id;
  return 0;
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
