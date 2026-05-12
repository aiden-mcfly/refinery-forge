#include "substrate_emit.h"

#include "xxhash.h"

#include <algorithm>
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
#include <vector>

#include <unistd.h>

namespace refinery_forge {
namespace {

struct Hash128Cmp {
  bool operator()(const Hash128& a, const Hash128& b) const {
    if (a.high64 != b.high64) return a.high64 < b.high64;
    return a.low64 < b.low64;
  }
};

size_t align8(size_t x) { return (x + 7u) & ~size_t(7u); }

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

std::string trim_line(std::string line) {
  const size_t first = line.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const size_t last = line.find_last_not_of(" \t\r\n");
  return line.substr(first, last - first + 1u);
}

std::string escape_json(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8u);
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
  const double idx = pct * static_cast<double>(buckets.size() - 1u);
  return static_cast<size_t>(buckets[static_cast<size_t>(std::llround(idx))]);
}

}  // namespace

std::vector<std::pair<size_t, size_t>> refinery_tokenize_canon(std::string_view s) {
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

BuildArtifacts refinery_build_markers(const CanonBuildResult& canon) {
  const auto words = refinery_tokenize_canon(canon.canon_utf8);
  std::map<Hash128, Marker, Hash128Cmp> uniq;
  std::array<uint32_t, kBucketCount> buckets{};
  constexpr size_t kFlattenedMax =
      (REFINERY_WINDOW_WORDS * REFINERY_MAX_WORD_BYTES) + (REFINERY_WINDOW_WORDS - 1u);
  char flat[kFlattenedMax];

  for (size_t i = 0; i + REFINERY_WINDOW_WORDS <= words.size(); ++i) {
    size_t off = 0;
    for (size_t j = 0; j < REFINERY_WINDOW_WORDS; ++j) {
      const auto& w = words[i + j];
      const size_t len = w.second - w.first;
      std::memcpy(flat + off, canon.canon_utf8.data() + w.first, len);
      off += len;
      if (j + 1u < REFINERY_WINDOW_WORDS) flat[off++] = ' ';
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
    const size_t span1 = words[i + (REFINERY_WINDOW_WORDS - 1u)].second;
    if (span0 > static_cast<size_t>(std::numeric_limits<uint32_t>::max())) {
      throw std::runtime_error("marker canon_offset exceeds uint32_t — SILENCE");
    }
    const size_t total_span = span1 - span0;
    if (total_span > static_cast<size_t>(std::numeric_limits<uint16_t>::max())) {
      throw std::runtime_error("marker span_bytes exceeds uint16_t — SILENCE");
    }
    Marker marker{};
    marker.hash_id = hid;
    marker.canon_offset = static_cast<uint32_t>(span0);
    marker.span_bytes = static_cast<uint16_t>(total_span);
    marker.pad = 0u;
    uniq.emplace(hid, marker);
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
      words.size() >= REFINERY_WINDOW_WORDS ? words.size() - (REFINERY_WINDOW_WORDS - 1u) : 0u;
  stats.unique_markers = markers.size();
  stats.duplicate_windows = stats.window_count >= stats.unique_markers
                                ? stats.window_count - stats.unique_markers
                                : 0u;
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

bool refinery_write_stats_file(const char* path, const BuildStats& stats, const std::string& sql,
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

void refinery_print_stats(const BuildStats& stats) {
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
    const std::string entry = trim_line(line);
    if (entry.empty() || entry[0] == '#') continue;
    try {
      if (refinery_canonicalize_existing_path(entry) == canonical_target) return true;
    } catch (const std::exception&) {
      continue;
    }
  }
  return false;
}

int refinery_write_marker_index_file(const char* path, const std::string& canon_utf8,
                                     const std::vector<Marker>& markers, uint64_t canon_xxh,
                                     const char* argv0, std::string* error) {
  if (markers.empty()) {
    if (error) *error = "marker set is empty";
    return 2;
  }
  if (markers.size() > REFINERY_MAX_MARKERS) {
    if (error) {
      *error = "marker_count=" + std::to_string(markers.size()) +
               " exceeds lawful cap REFINERY_MAX_MARKERS=" +
               std::to_string(REFINERY_MAX_MARKERS) + " — SILENCE";
    }
    return 3;
  }

  const uint64_t markers_offset = sizeof(SubstrateHeader);
  const uint64_t markers_bytes = static_cast<uint64_t>(markers.size()) * sizeof(Marker);
  const uint64_t canon_offset = align8(static_cast<size_t>(markers_offset + markers_bytes));
  const uint64_t canon_size = canon_utf8.size();
  const size_t total_size = static_cast<size_t>(canon_offset + canon_size);

  const std::string manifest = refinery_resolve_manifest_path(argv0);
  if (!manifest.empty() && refinery_path_is_protected(path, manifest)) {
    if (error) {
      *error = "L-F-02 VIOLATION: target path " + std::string(path) +
               " canonicalizes to a protected entry in " + manifest +
               " — refusing to overwrite live kernel substrate. SILENCE.";
    }
    return 4;
  }
  if (manifest.empty()) {
    std::cerr << "L-F-02: no active-substrate manifest found; protected-path check skipped\n";
  }

  std::vector<uint8_t> buf(total_size, 0u);
  SubstrateHeader hdr{};
  std::memcpy(hdr.magic, REFINERY_MAGIC_STR, REFINERY_MAGIC_LEN);
  hdr.format_version = REFINERY_FORMAT_VERSION_OPP51_1;
  hdr.marker_count = static_cast<uint32_t>(markers.size());
  hdr.canon_xxh64 = canon_xxh;
  hdr.markers_offset = markers_offset;
  hdr.markers_bytes = markers_bytes;
  hdr.canon_offset = canon_offset;
  hdr.canon_size = canon_size;
  std::memset(hdr.reserved, 0, sizeof(hdr.reserved));

  std::memcpy(buf.data(), &hdr, sizeof(hdr));
  std::memcpy(buf.data() + markers_offset, markers.data(),
              markers.size() * sizeof(Marker));
  std::memcpy(buf.data() + canon_offset, canon_utf8.data(), canon_utf8.size());

  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (error) *error = "failed to open output path: " + std::string(path);
    return 1;
  }
  out.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
  if (!out) {
    if (error) *error = "short write";
    return 1;
  }
  return 0;
}

int refinery_emit_substrate_from_text(std::string_view canon_utf8, const char* out_path,
                                      const char* argv0, BuildStats* out_stats,
                                      std::string* error) {
  CanonBuildResult canon{std::string(canon_utf8), 1u};
  const auto words = refinery_tokenize_canon(canon.canon_utf8);
  if (words.size() < REFINERY_WINDOW_WORDS) {
    if (error) {
      *error = "canon must contain at least " + std::to_string(REFINERY_WINDOW_WORDS) +
               " word-class tokens";
    }
    return 2;
  }
  const BuildArtifacts artifacts = refinery_build_markers(canon);
  if (out_stats) *out_stats = artifacts.stats;
  return refinery_write_marker_index_file(out_path, canon.canon_utf8, artifacts.markers,
                                          artifacts.stats.canon_xxh64, argv0, error);
}

}  // namespace refinery_forge
