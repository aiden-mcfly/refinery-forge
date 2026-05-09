/**
 * bitmask_generator — golden path: 7-word sliding windows → XXH128 markers + canon blob.
 * Postgres/JSON extraction lives in a separate translation unit / tool (see docs/FORGE-POSTGRES.md).
 */
#include "xxhash.h"

#include "refinery/substrate_interface.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct Hash128Cmp {
  bool operator()(const Hash128& a, const Hash128& b) const {
    if (a.high64 != b.high64) return a.high64 < b.high64;
    return a.low64 < b.low64;
  }
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

int write_marker_index_file(const char* path, const std::string& canon_utf8) {
  const auto words = word_spans_ascii(canon_utf8);
  if (words.size() < 7) {
    std::cerr << "canon must contain at least 7 whitespace-separated words\n";
    return 2;
  }

  std::map<Hash128, Marker, Hash128Cmp> uniq;

  for (size_t i = 0; i + 7 <= words.size(); ++i) {
    size_t span0 = words[i].first;
    size_t span1 = words[i + 6].second;
    const void* ptr = canon_utf8.data() + span0;
    size_t len = span1 - span0;
    XXH128_hash_t h = XXH3_128bits(ptr, len);
    Hash128 hid{};
    hid.low64 = h.low64;
    hid.high64 = h.high64;
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
  std::sort(markers.begin(), markers.end(),
            [](const Marker& a, const Marker& b) {
              Hash128Cmp cmp;
              return cmp(a.hash_id, b.hash_id);
            });

  const uint64_t canon_xxh =
      static_cast<uint64_t>(XXH64(canon_utf8.data(), canon_utf8.size(), 0));

  const uint64_t markers_offset = sizeof(SubstrateHeader);
  const uint64_t markers_bytes =
      static_cast<uint64_t>(markers.size()) * sizeof(Marker);
  const uint64_t canon_offset = align8(static_cast<size_t>(markers_offset + markers_bytes));
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

} // namespace

static const char kGoldenCanon[] =
    "In the beginning God created the heavens and the earth and the spirit moved "
    "over the deep";

int main(int argc, char** argv) {
  std::string out_path = "golden.marker.bin";
  std::string text = kGoldenCanon;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (a == "--text" && i + 1 < argc) {
      text = argv[++i];
    } else if (a == "-h" || a == "--help") {
      std::cout << "usage: refinery-forge [--out path] [--text \"canon utf-8\"]\n";
      return 0;
    }
  }
  return write_marker_index_file(out_path.c_str(), text);
}
