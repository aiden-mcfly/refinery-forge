/**
 * bitmask_generator — golden path: 7-word sliding windows → XXH128 markers + canon blob.
 *
 * G18: the substrate-emit pipeline (tokenize → build markers → write file) lives in
 * `substrate_emit.cpp` and is shared between `--text` substrate emission and
 * `--promote-tank-to-substrate` single-witness promotion. Forge MUST NOT duplicate
 * that pipeline here; any divergence is a v1 contract violation (drift between forge
 * .bin output and kernel spectroscopy lookups).
 *
 * Tokenization, word cap, window size, magic, and hash construction all derive
 * from substrate_interface.h. Forge MUST NOT redefine them locally.
 */
#include "xxhash.h"

#include "ledger.h"
#include "refinery/substrate_interface.h"
#include "substrate_emit.h"
#include "tank.h"

#if defined(REFINERY_HAVE_LIBPQ)
#include <libpq-fe.h>
#endif

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace {

using refinery_forge::BuildArtifacts;
using refinery_forge::BuildStats;
using refinery_forge::CanonBuildResult;

const char* g_argv0 = nullptr;

enum class ForgeOperation {
  EmitSubstrate,
  HashText,
  Witness,
  Refute,
  Reject,
  Evict,
  PromoteTankToSubstrate,
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
    std::cerr << "--out is only valid for substrate emission; "
                 "state operations use ledger/tank paths or positional targets\n";
    return false;
  }
  return true;
}

void print_tank_result(const refinery_forge::TankOpResult& result) {
  std::cout << "subject_hash=" << refinery_forge::refinery_hash_to_hex(result.subject_hash) << '\n'
            << "event_id=" << refinery_forge::refinery_hash_to_hex(result.event_id) << '\n'
            << "refutations_attempted=" << result.refutations_attempted << '\n'
            << "distinct_refuters=" << result.distinct_refuters << '\n'
            << "status=" << refinery_forge::refinery_tank_status_name(result.status) << '\n';
}

int check_tank_output_allowed(const std::string& tank_path) {
  const std::string manifest = refinery_forge::refinery_resolve_manifest_path(g_argv0);
  if (!manifest.empty() && refinery_forge::refinery_path_is_protected(tank_path, manifest)) {
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
    /* G18: tank promote → substrate path goes entirely through tank.cpp, which
     * dispatches single-witness v2 promotion through `refinery_emit_substrate_from_text`
     * (the SAME helper called by the --text emission path below). That shared call
     * site is the parity invariant locked by L-F-03.promote.spectroscopy. */
    const int rc = refinery_tank_promote_to_substrate(opts.promote_tank_path, ledger_path,
                                                       opts.promote_substrate_path, g_argv0,
                                                       &error);
    if (rc != 0) std::cerr << error << '\n';
    return rc;
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
      std::cerr
          << "L-F-03: eviction requires --refutation-event-id back-reference; "
             "audit chain break refused\n";
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

  std::cerr << "unknown state operation\n";  /* L-07: caller-facing error, not a routing branch */
  return 2;
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

int emit_substrate(const ForgeOptions& opts) {
  using namespace refinery_forge;

  if (opts.have_text) {
    /* --text path: route through the shared substrate-emit helper so that the
     * resulting .bin is byte-identical to `--promote-tank-to-substrate` output
     * for the same canon (this is the L-F-03.promote.spectroscopy parity rule). */
    BuildStats stats{};
    std::string error;
    const int rc = refinery_emit_substrate_from_text(opts.text, opts.out_path.c_str(), g_argv0,
                                                     &stats, &error);
    if (rc != 0) {
      if (!error.empty()) std::cerr << error << '\n';
      return rc;
    }
    if (opts.print_stats) refinery_print_stats(stats);
    if (!opts.stats_out_path.empty() &&
        !refinery_write_stats_file(opts.stats_out_path.c_str(), stats, opts.sql,
                                   opts.from_postgres)) {
      std::cerr << "failed to write stats file: " << opts.stats_out_path << '\n';
      return 1;
    }
    return 0;
  }

  /* --from-postgres / default golden canon path: same pipeline, different input. */
  const CanonBuildResult canon = load_canon(opts);
  const auto words = refinery_tokenize_canon(canon.canon_utf8);
  if (words.size() < REFINERY_WINDOW_WORDS) {
    std::cerr << "canon must contain at least " << REFINERY_WINDOW_WORDS
              << " word-class tokens\n";
    return 2;
  }
  const BuildArtifacts artifacts = refinery_build_markers(canon);

  if (opts.print_stats) refinery_print_stats(artifacts.stats);
  if (!opts.stats_out_path.empty() &&
      !refinery_write_stats_file(opts.stats_out_path.c_str(), artifacts.stats, opts.sql,
                                 opts.from_postgres)) {
    std::cerr << "failed to write stats file: " << opts.stats_out_path << '\n';
    return 1;
  }
  if (opts.stats_only) return 0;

  std::string error;
  const int rc = refinery_write_marker_index_file(opts.out_path.c_str(), canon.canon_utf8,
                                                  artifacts.markers,
                                                  artifacts.stats.canon_xxh64,
                                                  g_argv0, &error);
  if (rc != 0 && !error.empty()) std::cerr << error << '\n';
  return rc;
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
    return emit_substrate(opts);
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << '\n';
    return 1;
  }
}
