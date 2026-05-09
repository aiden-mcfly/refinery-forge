/**
 * bitmask_generator — golden path: 7-word sliding windows → XXH128 markers + canon blob.
 * Postgres/JSON extraction lives in forge; core consumes only the frozen .bin.
 */
#include "xxhash.h"

#include "refinery/substrate_interface.h"

#if defined(REFINERY_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
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

namespace {

constexpr size_t kWindowWords = 7;
constexpr size_t kBucketCount = 256;

struct Hash128Cmp {
  bool operator()(const Hash128& a, const Hash128& b) const {
    if (a.high64 != b.high64) return a.high64 < b.high64;
    return a.low64 < b.low64;
  }
};

struct ForgeOptions {
  std::string out_path = "golden.marker.bin";
  std::string stats_out_path;
  std::string text;
  std::string sql =
      "SELECT verse_text FROM canon_verses "
      "WHERE verse_text IS NOT NULL AND verse_text <> '' "
      "ORDER BY id";
  bool from_postgres = false;
  bool stats_only = false;
  bool have_text = false;
  bool print_stats = false;
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

std::vector<std::pair<size_t, size_t>> word_spans_ascii(const std::string& s) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    while (i < n && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= n) break;
    size_t start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(s[i]))) ++i;
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

void print_usage() {
  std::cout
      << "usage: refinery-forge [--out path] [--text \"canon utf-8\"]\n"
      << "                      [--from-postgres] [--sql query] [--limit N]\n"
      << "                      [--stats] [--stats-out path] [--stats-only]\n";
}

bool parse_args(int argc, char** argv, ForgeOptions* opts) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      opts->out_path = argv[++i];
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
  const auto words = word_spans_ascii(canon.canon_utf8);
  std::map<Hash128, Marker, Hash128Cmp> uniq;
  std::array<uint32_t, kBucketCount> buckets{};

  for (size_t i = 0; i + kWindowWords <= words.size(); ++i) {
    const size_t span0 = words[i].first;
    const size_t span1 = words[i + (kWindowWords - 1)].second;
    const void* ptr = canon.canon_utf8.data() + span0;
    const size_t len = span1 - span0;
    const XXH128_hash_t h = XXH3_128bits(ptr, len);
    Hash128 hid{};
    hid.low64 = h.low64;
    hid.high64 = h.high64;
    const size_t bucket =
        static_cast<size_t>((hid.low64 ^ hid.high64) & (kBucketCount - 1u));
    buckets[bucket] += 1u;
    if (uniq.count(hid)) continue;
    Marker m{};
    m.hash_id = hid;
    m.canon_offset = static_cast<uint64_t>(span0);
    m.span_bytes = static_cast<uint32_t>(len);
    std::memset(m.pad, 0, sizeof(m.pad));
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
  stats.window_count = words.size() >= kWindowWords ? words.size() - (kWindowWords - 1) : 0;
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

  std::vector<uint8_t> buf(total_size, 0);

  SubstrateHeader hdr{};
  hdr.magic = REFINERY_MAGIC;
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
std::string resolve_database_url() {
  for (const char* key : {"DATABASE_URL", "AIDEN_DATABASE_URL"}) {
    const char* value = std::getenv(key);
    if (value && *value) return value;
  }
  return {};
}

CanonBuildResult load_canon_from_postgres(const ForgeOptions& opts) {
  std::string sql = opts.sql;
  if (opts.limit > 0) sql += " LIMIT " + std::to_string(opts.limit);

  const std::string db_url = resolve_database_url();
  if (db_url.empty()) {
    throw std::runtime_error("DATABASE_URL/AIDEN_DATABASE_URL is not set");
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
  ForgeOptions opts;
  if (!parse_args(argc, argv, &opts)) {
    print_usage();
    return 2;
  }

  try {
    const CanonBuildResult canon = load_canon(opts);
    const auto words = word_spans_ascii(canon.canon_utf8);
    if (words.size() < kWindowWords) {
      std::cerr << "canon must contain at least 7 whitespace-separated words\n";
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
