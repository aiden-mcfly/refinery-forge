#ifndef REFINERY_FORGE_SUBSTRATE_EMIT_H
#define REFINERY_FORGE_SUBSTRATE_EMIT_H

#include "refinery/substrate_interface.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace refinery_forge {

constexpr size_t kBucketCount = 256u;

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

std::vector<std::pair<size_t, size_t>> refinery_tokenize_canon(std::string_view s);
BuildArtifacts refinery_build_markers(const CanonBuildResult& canon);
bool refinery_write_stats_file(const char* path, const BuildStats& stats, const std::string& sql,
                               bool from_postgres);
void refinery_print_stats(const BuildStats& stats);

std::string refinery_resolve_manifest_path(const char* argv0);
bool refinery_path_is_protected(const std::string& target_path,
                                const std::string& manifest_path);

int refinery_write_marker_index_file(const char* path, const std::string& canon_utf8,
                                     const std::vector<Marker>& markers, uint64_t canon_xxh,
                                     const char* argv0, std::string* error);

int refinery_emit_substrate_from_text(std::string_view canon_utf8, const char* out_path,
                                      const char* argv0, BuildStats* out_stats,
                                      std::string* error);

}  // namespace refinery_forge

#endif /* REFINERY_FORGE_SUBSTRATE_EMIT_H */
