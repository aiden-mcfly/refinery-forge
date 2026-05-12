/**
 * bitmask_generator — golden path: 7-word sliding windows → XXH128 markers + canon blob.
 * Postgres/JSON extraction lives in forge; core consumes only the frozen .bin.
 *
 * Tokenization, word cap, window size, magic, and hash construction all derive
 * from substrate_interface.h. Forge MUST NOT redefine them locally — drift between
 * forge and refinery-core kernel is a v1 contract violation.
 */
#include "xxhash.h"

#include "ledger.h"
#include "refinery/substrate_interface.h"
#include "tank.h"

#if defined(REFINERY_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

const char* g_argv0 = nullptr;

constexpr size_t kBucketCount = 256;

enum class ForgeOperation {
  EmitSubstrate,
  HashText,
  Witness,
  Refute,
  Reject,
  Evict,
  PromoteTankToSubstrate,
};

struct Hash128Cmp {
  bool operator()(const Hash128& a, const Hash128& b) const {
    if (a.high64 != b.high64) return a.high64 < b.high64;
    return a.low64 < b.low64;
  }
};

struct ForgeOptions {
  ForgeOperation operation = ForgeOperation::EmitSubstrate;
  std::string out_path = "golden.marker.bin";
  std::string stats_out_path;
  std::string text;
  std::string subject_hash_text;
  std::string evidence;
  std::string refuter_id;
  std::string refutation_event_id_text;
  std::string promote_tank_path;
  std::string promote_substrate_path;
  /* Selah-tenant default: scripture_verses(text_quote) is the canonical verse
   * column per SELAH-ZION schema (2026-05-10). canonical_position is a dense
   * 1..N integer anchored to NABRE Catholic 73-book canon order, populated by
   * the v1__anchor_canonical_position migration (verified 2026-05-10:
   * 35,543 rows, MIN=1, MAX=35,543, 0 NULLs, 0 gaps, 0 duplicates;
   * Gen 1:1 → 1, Rev 22:21 → 35,543). Source of truth for book ordering is
   * CANONICAL_BOOKS in canon-export.js (per L-11). */
  std::string sql =
      "SELECT text_quote FROM scripture_verses "
      "WHERE text_quote IS NOT NULL AND text_quote <> '' "
      "ORDER BY canonical_position";
  bool from_postgres = false;
  bool stats_only = false;
  bool have_text = false;
  bool have_out_path = false;
  bool print_stats = false;
  bool refutation_succeeds = false;
  int limit = -1;
};

struct CanonBuildResult {
  std::string canon_utf8;
  size_t row_count = 0;
};

struct BuildStats {
  size_t canon_bytes = 0;
  size_t row_count = 0;
  size_t word_count = 0;
  size_t window_count = 0;
  size_t duplicate_windows = 0;
  size_t unique_markers = 0;
  std::array<uint32_t, kBucketCount> buckets{};
  size_t min_bucket = 0;
  size_t max_bucket = 0;
  double mean_bucket = 0.0;
  size_t p50_bucket = 0;
  size_t p90_bucket = 0;
  size_t p99_bucket = 0;
  uint64_t canon_xxh64 = 0;
};

struct BuildArtifacts {
  std::vector<Marker> markers;
  BuildStats stats;
};

/**
 * Tokenize per the shared algorithm spec:
 *   - maximal runs of bytes for which refinery_word_byte() is true
 *   - case preserved
 *   - any single token >= REFINERY_MAX_WORD_BYTES collapses to SILENCE
 *     (forge throws; main() catches and exits non-zero without emitting a .bin)
 */
std::vector<std::pair<size_t, size_t>> tokenize_canon(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && !refinery_word_byte(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= n) break;
    const size_t start = i;
    while (i < n && refinery_word_byte(static_cast<unsigned char>(s[i]))) ++i;
    const size_t len = i - start;
    if (len >= REFINERY_MAX_WORD_BYTES) {
      throw std::runtime_error(
          "word length " + std::to_string(len) +
          " >= REFINERY_MAX_WORD_BYTES=" + std::to_string(REFINERY_MAX_WORD_BYTES) +
          " — SILENCE (forge refuses to emit substrate)");
    }
    out.emplace_back(start, i);
  }
  return out;
}

size_t align8(size_t x) { return (x + 7u) & ~size_t(7u); }

std::optional<int> parse_int(const char* s) {
  if (!s || !*s) return std::nullopt;
  char* end = nullptr;
  errno = 0;
  long value = std::strtol(s, &end, 10);
  if (errno != 0 || !end || *end != '\0') return std::nullopt;
  if (value < std::numeric_limits<int>::min() ||
      value > std::numeric_limits<int>::max()) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

std::string refinery_dirname(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return ".";
  if (slash == 0u) return "/";
  return std::string(path.substr(0u, slash));
}

std::string refinery_basename(std::string_view path) {
  const size_t slash = path.find_last_of('/');
  if (slash == std::string_view::npos) return std::string(path);
  return std::string(path.substr(slash + 1u));
}

std::string refinery_canonicalize_existing_path(const std::string& path) {
  char* resolved = realpath(path.c_str(), nullptr);
  if (resolved == nullptr) {
    throw std::runtime_error("failed to canonicalize path: " + path +
                             " (" + std::strerror(errno) + ")");
  }
  const std::string out(resolved);
  std::free(resolved);
  return out;
}

std::string refinery_canonicalize_path(const std::string& target_path) {
  errno = 0;
  char* resolved = realpath(target_path.c_str(), nullptr);
  if (resolved != nullptr) {
    const std::string out(resolved);
    std::free(resolved);
    return out;
  }
  if (errno != ENOENT) {
    throw std::runtime_error("failed to canonicalize target path: " + target_path +
                             " (" + std::strerror(errno) + ")");
  }

  const std::string parent = refinery_dirname(target_path);
  const std::string base = refinery_basename(target_path);
  const std::string canonical_parent = refinery_canonicalize_existing_path(parent);
  if (canonical_parent == "/") return canonical_parent + base;
  return canonical_parent + "/" + base;
}

std::string refinery_resolve_manifest_path(const char* argv0) {
  const char* env = std::getenv("REFINERY_SUBSTRATE_MANIFEST");
  if (env && *env) {
    if (access(env, R_OK) == 0) return env;
    throw std::runtime_error("L-F-02: REFINERY_SUBSTRATE_MANIFEST is set but unreadable: " +
                             std::string(env));
  }

  const char* cwd_candidates[] = {
      ".active-substrate-paths",
      "refinery-core/.active-substrate-paths",
  };
  for (const char* candidate : cwd_candidates) {
    if (access(candidate, R_OK) == 0) return candidate;
  }

  if (argv0 && *argv0) {
    try {
      const std::string exe_path = refinery_canonicalize_existing_path(argv0);
      const std::string exe_dir = refinery_dirname(exe_path);
      const std::string fallback =
          refinery_dirname(exe_dir) + "/refinery-core/.active-substrate-paths";
      if (access(fallback.c_str(), R_OK) == 0) return fallback;
    } catch (const std::exception&) {
    }
  }

  return {};
}

bool refinery_path_is_protected(const std::string& target_path,
                                const std::string& manifest_path) {
  const std::string canonical_target = refinery_canonicalize_path(target_path);
  std::ifstream manifest(manifest_path);
  if (!manifest) {
    throw std::runtime_error("L-F-02: failed to open manifest: " + manifest_path);
  }

  std::string line;
  while (std::getline(manifest, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) continue;
    if (line[first] == '#') continue;
    const size_t last = line.find_last_not_of(" \t");
    const std::string entry = line.substr(first, last - first + 1u);
    if (entry.empty()) continue;
    try {
      if (refinery_canonicalize_existing_path(entry) == canonical_target) return true;
    } catch (const std::exception&) {
      continue;
    }
  }
  return false;
}

void print_usage() {
  std::cout
      << "usage: refinery-forge [--out path] [--text \"canon utf-8\"]\n"
      << "                      [--from-postgres] [--sql query] [--limit N]\n"
      << "                      [--stats] [--stats-out path] [--stats-only]\n"
      << "       refinery-forge --hash-text \"signal\"\n"
      << "       refinery-forge --witness \"signal\"\n"
      << "       refinery-forge --reject <subject-hash>\n"
      << "       refinery-forge --refute <subject-hash> --evidence \"bytes\" [--refuter id] [--succeeds]\n"
      << "       refinery-forge --evict <subject-hash> --refutation-event-id <event-id>\n"
      << "       refinery-forge --promote-tank-to-substrate <tank> <substrate>\n";
}

bool set_operation(ForgeOptions* opts, ForgeOperation operation) {
  if (opts->operation != ForgeOperation::EmitSubstrate) {
    std::cerr << "choose exactly one state operation\n";
    return false;
  }
  opts->operation = operation;
  return true;
}

bool parse_args(int argc, char** argv, ForgeOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      opts->out_path = argv[++i];
      opts->have_out_path = true;
    } else if (a == "--text" && i + 1 < argc) {
      opts->text = argv[++i];
      opts->have_text = true;
    } else if (a == "--from-postgres") {
      opts->from_postgres = true;
    } else if (a == "--sql" && i + 1 < argc) {
      opts->sql = argv[++i];
    } else if (a == "--limit" && i + 1 < argc) {
      const auto parsed = parse_int(argv[++i]);
      if (!parsed || *parsed <= 0) {
        std::cerr << "invalid --limit value\n";
        return false;
      }
      opts->limit = *parsed;
    } else if (a == "--stats") {
      opts->print_stats = true;
    } else if (a == "--stats-out" && i + 1 < argc) {
      opts->stats_out_path = argv[++i];
    } else if (a == "--stats-only") {
      opts->stats_only = true;
      opts->print_stats = true;
    } else if (a == "--hash-text" && i + 1 < argc) {
      if (!set_operation(opts, ForgeOperation::HashText)) return false;
      opts->text = argv[++i];
      opts->have_text = true;
    } else if (a == "--witness" && i + 1 < argc) {
      if (!set_operation(opts, ForgeOperation::Witness)) return false;
      opts->text = argv[++i];
      opts->have_text = true;
    } else if (a == "--reject" && i + 1 < argc) {
      if (!set_operation(opts, ForgeOperation::Reject)) return false;
      opts->subject_hash_text = argv[++i];
    } else if (a == "--refute" && i + 1 < argc) {
      if (!set_operation(opts, ForgeOperation::Refute)) return false;
      opts->subject_hash_text = argv[++i];
    } else if (a == "--evidence" && i + 1 < argc) {
      opts->evidence = argv[++i];
    } else if (a == "--refuter" && i + 1 < argc) {
      opts->refuter_id = argv[++i];
    } else if (a == "--succeeds") {
      opts->refutation_succeeds = true;
    } else if (a == "--evict" && i + 1 < argc) {
      if (!set_operation(opts, ForgeOperation::Evict)) return false;
      opts->subject_hash_text = argv[++i];
    } else if (a == "--refutation-event-id" && i + 1 < argc) {
      opts->refutation_event_id_text = argv[++i];
    } else if (a == "--promote-tank-to-substrate" && i + 2 < argc) {
      if (!set_operation(opts, ForgeOperation::PromoteTankToSubstrate)) return false;
      opts->promote_tank_path = argv[++i];
      opts->promote_substrate_path = argv[++i];
    } else if (a == "-h" || a == "--help") {
      print_usage();
      std::exit(0);
    } else {
      std::cerr << "unknown argument: " << a << '\n';
      return false;
    }
  }
  if (opts->have_text && opts->from_postgres) {
    std::cerr << "choose either --text or --from-postgres\n";
    return false;
  }
  if (opts->operation != ForgeOperation::EmitSubstrate &&
      opts->operation != ForgeOperation::HashText &&
      (opts->from_postgres || opts->stats_only || opts->print_stats ||
       !opts->stats_out_path.empty())) {
    std::cerr << "state operations cannot be combined with substrate emission flags\n";
    return false;
  }
  if (opts->operation != ForgeOperation::EmitSubstrate && opts->have_out_path) {
    std::cerr << "--out is only valid for substrate emission; state operations use ledger/tank paths or positional targets\n";
    return false;
  }
  return true;
}

std::string escape_json(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char ch : s) {
    switch (ch) {
      case '\\':
        out += "\\\\";
        break;
      case '"':
        out += "\\\"";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

size_t percentile_bucket(std::array<uint32_t, kBucketCount> buckets, double pct) {
  std::sort(buckets.begin(), buckets.end());
  const double idx = pct * static_cast<double>(buckets.size() - 1);
  return static_cast<size_t>(buckets[static_cast<size_t>(std::llround(idx))]);
}

BuildArtifacts build_markers(const CanonBuildResult& canon) {
  const auto words = tokenize_canon(canon.canon_utf8);
  std::map<Hash128, Marker, Hash128Cmp> uniq;
  std::array<uint32_t, kBucketCount> buckets{};

  /* Stack buffer sized to the spec ceiling. Same shape as the kernel's
   * SpectroscopyRing flattened buffer so output bytes are bit-identical. */
  constexpr size_t kFlattenedMax =
      (REFINERY_WINDOW_WORDS * REFINERY_MAX_WORD_BYTES) + (REFINERY_WINDOW_WORDS - 1u);
  char flat[kFlattenedMax];

  for (size_t i = 0; i + REFINERY_WINDOW_WORDS <= words.size(); ++i) {
    /* Construct flattened buffer per shared algorithm spec:
     *   token[i] ++ 0x20 ++ token[i+1] ++ 0x20 ++ ... ++ token[i+6]
     * (no leading/trailing 0x20; exactly 6 separators between 7 tokens). */
    size_t off = 0;
    for (size_t j = 0; j < REFINERY_WINDOW_WORDS; ++j) {
      const auto& w = words[i + j];
      const size_t len = w.second - w.first;
      std::memcpy(flat + off, canon.canon_utf8.data() + w.first, len);
      off += len;
      if (j + 1u < REFINERY_WINDOW_WORDS) {
        flat[off++] = ' ';
      }
    }
    const XXH128_hash_t h = XXH3_128bits(flat, off);
    Hash128 hid{};
    hid.low64 = h.low64;
    hid.high64 = h.high64;
    const size_t bucket =
        static_cast<size_t>((hid.low64 ^ hid.high64) & (kBucketCount - 1u));
    buckets[bucket] += 1u;
    if (uniq.count(hid)) continue;

    const size_t span0 = words[i].first;
    const size_t span1 = words[i + (REFINERY_WINDOW_WORDS - 1)].second;
    if (span0 > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
      throw std::runtime_error("marker canon_offset exceeds uint32_t — SILENCE");
    }
    const size_t total_span = span1 - span0;
    if (total_span > static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
      throw std::runtime_error("marker span_bytes exceeds uint16_t — SILENCE");
    }
    Marker m{};
    m.hash_id = hid;
    m.canon_offset = static_cast<uint32_t>(span0);
    m.span_bytes = static_cast<uint16_t>(total_span);
    m.pad = 0;
    uniq.emplace(hid, m);
  }

  std::vector<Marker> markers;
  markers.reserve(uniq.size());
  for (const auto& kv : uniq) markers.push_back(kv.second);
  std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
    Hash128Cmp cmp;
    return cmp(a.hash_id, b.hash_id);
  });

  BuildStats stats{};
  stats.canon_bytes = canon.canon_utf8.size();
  stats.row_count = canon.row_count;
  stats.word_count = words.size();
  stats.window_count =
      words.size() >= REFINERY_WINDOW_WORDS ? words.size() - (REFINERY_WINDOW_WORDS - 1) : 0;
  stats.unique_markers = markers.size();
  stats.duplicate_windows = stats.window_count >= stats.unique_markers
                                ? stats.window_count - stats.unique_markers
                                : 0;
  stats.buckets = buckets;
  const auto [min_it, max_it] = std::minmax_element(buckets.begin(), buckets.end());
  stats.min_bucket = static_cast<size_t>(*min_it);
  stats.max_bucket = static_cast<size_t>(*max_it);
  stats.mean_bucket = static_cast<double>(stats.window_count) / static_cast<double>(kBucketCount);
  stats.p50_bucket = percentile_bucket(buckets, 0.50);
  stats.p90_bucket = percentile_bucket(buckets, 0.90);
  stats.p99_bucket = percentile_bucket(buckets, 0.99);
  stats.canon_xxh64 =
      static_cast<uint64_t>(XXH64(canon.canon_utf8.data(), canon.canon_utf8.size(), 0));

  return {std::move(markers), stats};
}

bool write_stats_file(const char* path, const BuildStats& stats, const std::string& sql,
                      bool from_postgres) {
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  out << "{\n";
  out << "  \"source\": \"" << (from_postgres ? "postgres" : "text") << "\",\n";
  out << "  \"sql\": \"" << escape_json(sql) << "\",\n";
  out << "  \"canon_bytes\": " << stats.canon_bytes << ",\n";
  out << "  \"row_count\": " << stats.row_count << ",\n";
  out << "  \"word_count\": " << stats.word_count << ",\n";
  out << "  \"window_count\": " << stats.window_count << ",\n";
  out << "  \"unique_markers\": " << stats.unique_markers << ",\n";
  out << "  \"duplicate_windows\": " << stats.duplicate_windows << ",\n";
  out << "  \"canon_xxh64\": " << stats.canon_xxh64 << ",\n";
  out << "  \"bucket_count\": " << kBucketCount << ",\n";
  out << "  \"bucket_min\": " << stats.min_bucket << ",\n";
  out << "  \"bucket_max\": " << stats.max_bucket << ",\n";
  out << "  \"bucket_mean\": " << std::fixed << std::setprecision(4) << stats.mean_bucket
      << ",\n";
  out << "  \"bucket_p50\": " << stats.p50_bucket << ",\n";
  out << "  \"bucket_p90\": " << stats.p90_bucket << ",\n";
  out << "  \"bucket_p99\": " << stats.p99_bucket << "\n";
  out << "}\n";
  return static_cast<bool>(out);
}

void print_stats(const BuildStats& stats) {
  std::cout << "rows=" << stats.row_count << '\n'
            << "canon_bytes=" << stats.canon_bytes << '\n'
            << "words=" << stats.word_count << '\n'
            << "windows=" << stats.window_count << '\n'
            << "unique_markers=" << stats.unique_markers << '\n'
            << "duplicate_windows=" << stats.duplicate_windows << '\n'
            << "canon_xxh64=" << stats.canon_xxh64 << '\n'
            << "bucket_count=" << kBucketCount << '\n'
            << "bucket_min=" << stats.min_bucket << '\n'
            << "bucket_max=" << stats.max_bucket << '\n'
            << "bucket_mean=" << std::fixed << std::setprecision(4) << stats.mean_bucket
            << '\n'
            << "bucket_p50=" << stats.p50_bucket << '\n'
            << "bucket_p90=" << stats.p90_bucket << '\n'
            << "bucket_p99=" << stats.p99_bucket << '\n';
}

int write_marker_index_file(const char* path, const std::string& canon_utf8,
                            const std::vector<Marker>& markers, uint64_t canon_xxh);

void print_tank_result(const refinery_forge::TankOpResult& result) {
  std::cout << "subject_hash=" << refinery_forge::refinery_hash_to_hex(result.subject_hash) << '\n'
            << "event_id=" << refinery_forge::refinery_hash_to_hex(result.event_id) << '\n'
            << "refutations_attempted=" << result.refutations_attempted << '\n'
            << "distinct_refuters=" << result.distinct_refuters << '\n'
            << "status=" << refinery_forge::refinery_tank_status_name(result.status) << '\n';
}

int check_tank_output_allowed(const std::string& tank_path) {
  const std::string manifest = refinery_resolve_manifest_path(g_argv0);
  if (!manifest.empty() && refinery_path_is_protected(tank_path, manifest)) {
    std::cerr << "L-F-02 VIOLATION: target path " << tank_path
              << " canonicalizes to a protected entry in " << manifest
              << " — refusing entropy tank write. SILENCE.\n";
    return 4;
  }
  return 0;
}

int run_state_operation(const ForgeOptions& opts) {
  using namespace refinery_forge;

  if (opts.operation == ForgeOperation::HashText) {
    std::cout << refinery_hash_to_hex(refinery_hash_string(opts.text)) << '\n';
    return 0;
  }

  std::string error;
  const std::string ledger_path = refinery_resolve_ledger_path();
  std::string tank_path = refinery_resolve_tank_path();
  if (opts.operation == ForgeOperation::PromoteTankToSubstrate) {
    tank_path = opts.promote_tank_path;
  }

  if (opts.operation == ForgeOperation::Witness ||
      opts.operation == ForgeOperation::Refute ||
      opts.operation == ForgeOperation::Reject ||
      opts.operation == ForgeOperation::Evict) {
    const int guard = check_tank_output_allowed(tank_path);
    if (guard != 0) return guard;
  }

  TankOpResult result{};
  if (opts.operation == ForgeOperation::Witness) {
    const int rc = refinery_tank_witness(tank_path, ledger_path, opts.text, &result, &error);
    if (rc == 0) print_tank_result(result);
    else std::cerr << error << '\n';
    return rc;
  }

  if (opts.operation == ForgeOperation::PromoteTankToSubstrate) {
    std::vector<Hash128> hashes;
    const int tank_rc = refinery_tank_promotable_hashes(tank_path, &hashes, &error);
    if (tank_rc != 0) {
      std::cerr << error << '\n';
      return tank_rc;
    }
    std::vector<Marker> markers;
    markers.reserve(hashes.size());
    for (const Hash128& hash : hashes) {
      Marker marker{};
      marker.hash_id = hash;
      marker.canon_offset = 0u;
      marker.span_bytes = 13u;
      marker.pad = 0u;
      markers.push_back(marker);
    }
    std::sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) {
      Hash128Cmp cmp;
      return cmp(a.hash_id, b.hash_id);
    });
    const std::string canon = "tank-promoted";
    const uint64_t canon_xxh = static_cast<uint64_t>(XXH64(canon.data(), canon.size(), 0));
    const Hash128 subject_hash = refinery_hash_bytes(hashes.data(), hashes.size() * sizeof(Hash128));
    const Hash128 evidence_hash = refinery_hash_string(opts.promote_substrate_path);
    Hash128 promotion_event_id{};
    const int ledger_rc = refinery_emit_ledger_entry(ledger_path, LEDGER_EVENT_PROMOTION,
                                                     subject_hash, evidence_hash,
                                                     &promotion_event_id, &error);
    if (ledger_rc != 0) {
      std::cerr << error << '\n';
      return ledger_rc;
    }
    return write_marker_index_file(opts.promote_substrate_path.c_str(), canon, markers, canon_xxh);
  }

  Hash128 subject{};
  if (!refinery_parse_hash(opts.subject_hash_text, &subject)) {
    std::cerr << "invalid subject hash\n";
    return 7;
  }

  if (opts.operation == ForgeOperation::Reject) {
    const int rc = refinery_tank_reject(tank_path, ledger_path, subject, &result, &error);
    if (rc == 0) print_tank_result(result);
    else std::cerr << error << '\n';
    return rc;
  }

  if (opts.operation == ForgeOperation::Refute) {
    if (opts.evidence.empty()) {
      std::cerr << "L-F-03: --refute requires --evidence\n";
      return 7;
    }
    std::string refuter = opts.refuter_id;
    if (refuter.empty()) {
      const char* env = std::getenv("REFINERY_REFUTER_ID");
      if (env && *env) refuter = env;
    }
    const int rc = refinery_tank_refute(tank_path, ledger_path, subject, opts.evidence,
                                        refuter, opts.refutation_succeeds,
                                        &result, &error);
    if (rc == 0) print_tank_result(result);
    else std::cerr << error << '\n';
    return rc;
  }

  if (opts.operation == ForgeOperation::Evict) {
    if (opts.refutation_event_id_text.empty()) {
      std::cerr << "L-F-03: eviction requires --refutation-event-id back-reference; audit chain break refused\n";
      return 7;
    }
    Hash128 refutation_event_id{};
    if (!refinery_parse_hash(opts.refutation_event_id_text, &refutation_event_id)) {
      std::cerr << "L-F-03: invalid refutation event id\n";
      return 7;
    }
    const int rc = refinery_tank_evict(tank_path, ledger_path, subject,
                                       refutation_event_id, &result, &error);
    if (rc == 0) print_tank_result(result);
    else std::cerr << error << '\n';
    return rc;
  }

  std::cerr << "unknown state operation\n";
  return 2;
}

/* L-F-02 guard: forge may stage candidate substrate files, but it must refuse
 * to overwrite any operator-declared live kernel substrate path. Exit code 4
 * is reserved for protected-path refusal. */
int write_marker_index_file(const char* path, const std::string& canon_utf8,
                            const std::vector<Marker>& markers, uint64_t canon_xxh) {
  if (markers.empty()) {
    std::cerr << "marker set is empty\n";
    return 2;
  }
  if (markers.size() > REFINERY_MAX_MARKERS) {
    std::cerr << "marker_count=" << markers.size()
              << " exceeds lawful cap REFINERY_MAX_MARKERS=" << REFINERY_MAX_MARKERS
              << " — SILENCE\n";
    return 3;
  }

  const uint64_t markers_offset = sizeof(SubstrateHeader);
  const uint64_t markers_bytes =
      static_cast<uint64_t>(markers.size()) * sizeof(Marker);
  const uint64_t canon_offset =
      align8(static_cast<size_t>(markers_offset + markers_bytes));
  const uint64_t canon_size = canon_utf8.size();
  const size_t total_size = static_cast<size_t>(canon_offset + canon_size);

  const std::string manifest = refinery_resolve_manifest_path(g_argv0);
  if (!manifest.empty() && refinery_path_is_protected(path, manifest)) {
    std::cerr << "L-F-02 VIOLATION: target path " << path
              << " canonicalizes to a protected entry in " << manifest
              << " — refusing to overwrite live kernel substrate. SILENCE.\n";
    return 4;
  }
  if (manifest.empty()) {
    std::cerr << "L-F-02: no active-substrate manifest found; protected-path check skipped\n";
  }

  std::vector<uint8_t> buf(total_size, 0);

  SubstrateHeader hdr{};
  std::memcpy(hdr.magic, REFINERY_MAGIC_STR, REFINERY_MAGIC_LEN);
  hdr.format_version = REFINERY_FORMAT_VERSION_OPP51_1;
  hdr.marker_count = static_cast<uint32_t>(markers.size());
  hdr.canon_xxh64 = canon_xxh;
  hdr.markers_offset = markers_offset;
  hdr.markers_bytes = markers_bytes;
  hdr.canon_offset = static_cast<uint64_t>(canon_offset);
  hdr.canon_size = static_cast<uint64_t>(canon_size);
  std::memset(hdr.reserved, 0, sizeof(hdr.reserved));

  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  std::memcpy(buf.data() + markers_offset, markers.data(),
              markers.size() * sizeof(Marker));
  std::memcpy(buf.data() + canon_offset, canon_utf8.data(), canon_utf8.size());

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::cerr << "failed to open output path: " << path << '\n';
    return 1;
  }
  out.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
  if (!out) {
    std::cerr << "short write\n";
    return 1;
  }
  return 0;
}

#if defined(REFINERY_HAVE_LIBPQ)
/* Selah is the source of truth (per project_instructions: "A valid object in
 * the wrong tenant is invalid"). AIDEN was a downstream projection and is no
 * longer a lawful source. Forge resolves SELAH_DATABASE_URL only — generic
 * DATABASE_URL is rejected to prevent accidental wrong-tenant queries. */
std::string resolve_database_url() {
  const char* value = std::getenv("SELAH_DATABASE_URL");
  if (value && *value) return value;
  return {};
}

CanonBuildResult load_canon_from_postgres(const ForgeOptions& opts) {
  std::string sql = opts.sql;
  if (opts.limit > 0) sql += " LIMIT " + std::to_string(opts.limit);

  const std::string db_url = resolve_database_url();
  if (db_url.empty()) {
    throw std::runtime_error("SELAH_DATABASE_URL is not set");
  }

  PGconn* conn = PQconnectdb(db_url.c_str());
  if (!conn) throw std::runtime_error("PQconnectdb returned null");
  const auto finish = [&]() { PQfinish(conn); };

  if (PQstatus(conn) != CONNECTION_OK) {
    const std::string msg = PQerrorMessage(conn) ? PQerrorMessage(conn) : "unknown";
    finish();
    throw std::runtime_error("postgres connect failed: " + msg);
  }

  PGresult* res = PQexec(conn, sql.c_str());
  if (!res) {
    finish();
    throw std::runtime_error("postgres query returned null result");
  }
  const auto clear = [&]() { PQclear(res); };

  if (PQresultStatus(res) != PGRES_TUPLES_OK) {
    const std::string msg = PQresultErrorMessage(res) ? PQresultErrorMessage(res) : "unknown";
    clear();
    finish();
    throw std::runtime_error("postgres query failed: " + msg);
  }
  if (PQnfields(res) != 1) {
    clear();
    finish();
    throw std::runtime_error("query must return exactly one text column");
  }

  CanonBuildResult out{};
  const int rows = PQntuples(res);
  out.row_count = static_cast<size_t>(rows);
  for (int i = 0; i < rows; ++i) {
    if (PQgetisnull(res, i, 0)) continue;
    const char* value = PQgetvalue(res, i, 0);
    if (!value || !*value) continue;
    if (!out.canon_utf8.empty()) out.canon_utf8.push_back('\n');
    out.canon_utf8 += value;
  }

  clear();
  finish();
  return out;
}
#endif

CanonBuildResult load_canon(const ForgeOptions& opts) {
  if (opts.have_text) return {opts.text, 1};
  if (opts.from_postgres) {
#if defined(REFINERY_HAVE_LIBPQ)
    return load_canon_from_postgres(opts);
#else
    throw std::runtime_error("this build does not include libpq support");
#endif
  }
  static const char kGoldenCanon[] =
      "In the beginning God created the heavens and the earth and the spirit moved "
      "over the deep";
  return {kGoldenCanon, 1};
}

}  // namespace

int main(int argc, char** argv) {
  g_argv0 = (argc > 0) ? argv[0] : nullptr;
  ForgeOptions opts;
  if (!parse_args(argc, argv, &opts)) {
    print_usage();
    return 2;
  }

  try {
    if (opts.operation != ForgeOperation::EmitSubstrate) {
      return run_state_operation(opts);
    }

    const CanonBuildResult canon = load_canon(opts);
    const auto words = tokenize_canon(canon.canon_utf8);
    if (words.size() < REFINERY_WINDOW_WORDS) {
      std::cerr << "canon must contain at least " << REFINERY_WINDOW_WORDS
                << " word-class tokens\n";
      return 2;
    }

    const BuildArtifacts artifacts = build_markers(canon);

    if (opts.print_stats) print_stats(artifacts.stats);
    if (!opts.stats_out_path.empty() &&
        !write_stats_file(opts.stats_out_path.c_str(), artifacts.stats, opts.sql,
                          opts.from_postgres)) {
      std::cerr << "failed to write stats file: " << opts.stats_out_path << '\n';
      return 1;
    }
    if (opts.stats_only) return 0;

    return write_marker_index_file(opts.out_path.c_str(), canon.canon_utf8,
                                   artifacts.markers, artifacts.stats.canon_xxh64);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
