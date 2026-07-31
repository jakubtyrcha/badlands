// badlands_duelsim -- the balance REPORT tool.
//
// Simulates every 1v1 pairing on the roster and writes what happened: a win
// matrix, a calibration report against the fixed threat anchors, and SVG charts
// of the stat parameters over level. Headless, links only badlands_game_lib
// (no engine, no Dawn, no SDL).
//
// It reports; it never tunes and never asserts. Balance is an approximation --
// what is pinned lives in game/tests (mechanism) and in the threat table
// (calibration targets). If a cell here looks wrong because a MECHANIC is
// broken (a hunter that never fires, a golem that never arrives) that is a bug;
// a cell that is merely unbalanced is the expected state of an approximation.
//
//   ./build/badlands_duelsim --out duelsim_out

#include "duel_matrix.h"
#include "svg_chart.h"

#include "badlands_sim.hpp"
#include "threat_table.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using badlands::CreatureId;
using badlands::CreatureName;
using badlands::threat_target;

namespace {

// Deer is deliberately absent: it has no attacks at all, so every row and
// column of it would be a draw and say nothing.
const std::vector<CreatureId> kRoster = {
    CreatureId::Mercenary, CreatureId::Hunter,       CreatureId::GraveRobber,
    CreatureId::Apprentice, CreatureId::Rat,         CreatureId::Goblin,
    CreatureId::Bandit,     CreatureId::BanditArcher, CreatureId::BanditLeader,
    CreatureId::MudGolem,
};

// The four core classes, for the parameter charts.
const std::vector<CreatureId> kClasses = {
    CreatureId::Mercenary, CreatureId::Hunter, CreatureId::GraveRobber, CreatureId::Apprentice,
};

const char* kClassColours[] = {"#4a7fd0", "#3f9a55", "#8a5fb0", "#3fa8bb"};

std::string pct(float rate) {
    if (rate < 0.0f) {
        return "-";  // every sample drew: no opinion, rather than a false 0%
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.0f%%", static_cast<double>(rate * 100.0f));
    return buf;
}

bool write_matrix_md(const std::string& path, const std::vector<CreatureId>& roster,
                     const std::vector<std::vector<duelsim::MatrixCell>>& m, int32_t level,
                     int32_t samples) {
    std::ofstream f(path);
    if (!f.good()) {
        std::fprintf(stderr, "duelsim: cannot write '%s'\n", path.c_str());
        return false;
    }
    f << "# Duel matrix\n\n";
    f << "Level " << level << ", " << samples
      << " staged separations per pairing. Each cell is the ROW creature's record against the "
         "column creature: `W-L-D`, then its win rate over decided duels, then the median "
         "duration in ticks.\n\n";
    f << "Reported, not asserted. The threat column is the FIXED calibration target "
         "(game/src/threat_table.h) -- balancing moves the stats toward it, never it toward "
         "the stats.\n\n";
    f << "The matrix is NOT symmetric by construction: `[row][col]` always stages the row "
         "creature on the left at -x and the column creature on the right, and the combat seed "
         "folds both slots, so the two directions are independent fights. Comparing a cell with "
         "its transpose is therefore a read on side bias, not a consistency check.\n\n";

    f << "| vs | threat |";
    for (CreatureId c : roster) {
        f << ' ' << CreatureName(c) << " |";
    }
    f << "\n|---|---|";
    for (size_t i = 0; i < roster.size(); ++i) {
        f << "---|";
    }
    f << '\n';

    for (size_t i = 0; i < roster.size(); ++i) {
        f << "| **" << CreatureName(roster[i]) << "** | " << threat_target(roster[i], level)
          << " |";
        for (size_t j = 0; j < roster.size(); ++j) {
            const duelsim::MatrixCell& c = m[i][j];
            f << ' ' << c.wins << '-' << c.losses << '-' << c.draws << ' ' << pct(c.win_rate())
              << " <br><sub>" << c.median_ticks << "t</sub> |";
        }
        f << '\n';
    }
    f << "\nA `-` win rate means every sample of that pairing timed out.\n";
    return true;
}

struct CalibRow {
    std::string a, b;
    float threat_a = 0.0f, threat_b = 0.0f;
    float delta = 0.0f;
    float win_rate_a = 0.0f;  // a's rate against b
};

std::vector<CalibRow> calibration_rows(const std::vector<CreatureId>& roster,
                                       const std::vector<std::vector<duelsim::MatrixCell>>& m,
                                       int32_t level) {
    std::vector<CalibRow> rows;
    for (size_t i = 0; i < roster.size(); ++i) {
        for (size_t j = i; j < roster.size(); ++j) {
            // BOTH stagings. [i][j] and [j][i] are independent fights, not
            // mirrors -- run_duel fixes who starts on which side and the combat
            // seed folds both slots -- so combining them uses all the data AND
            // averages out any side bias, which reading one direction alone
            // would silently bake into the answer.
            const int32_t wins = m[i][j].wins + (i == j ? 0 : m[j][i].losses);
            const int32_t losses = m[i][j].losses + (i == j ? 0 : m[j][i].wins);
            if (wins + losses == 0) {
                continue;  // every sample timed out: no opinion to record
            }
            CalibRow r;
            r.a = CreatureName(roster[i]);
            r.b = CreatureName(roster[j]);
            r.threat_a = threat_target(roster[i], level);
            r.threat_b = threat_target(roster[j], level);
            r.delta = r.threat_a - r.threat_b;
            r.win_rate_a = static_cast<float>(wins) / static_cast<float>(wins + losses);
            rows.push_back(r);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const CalibRow& x, const CalibRow& y) {
        if (std::fabs(x.delta) != std::fabs(y.delta)) {
            return std::fabs(x.delta) < std::fabs(y.delta);
        }
        return x.a < y.a;
    });
    return rows;
}

bool write_calibration_md(const std::string& path, const std::vector<CalibRow>& rows,
                          int32_t level) {
    std::ofstream f(path);
    if (!f.good()) {
        std::fprintf(stderr, "duelsim: cannot write '%s'\n", path.c_str());
        return false;
    }
    f << "# Calibration report\n\n";
    f << "Level " << level
      << ". One row per pairing, sorted by how far apart the two threat targets are.\n\n";
    f << "The design document states one invariant and asks one open question, and this table "
         "answers both empirically rather than by modelling them:\n\n";
    f << "- **Invariant:** creatures of the same caliber should win about half their fights "
         "against each other. Read the top of the table, where the difference is 0.\n";
    f << "- **Open question:** how does a difference in threat shift the expected win ratio? "
         "Read the rest of it.\n\n";
    f << "| A | B | threat A | threat B | difference | A's win rate |\n";
    f << "|---|---|---|---|---|---|\n";
    for (const CalibRow& r : rows) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "| %s | %s | %.2f | %.2f | %+.2f | %.0f%% |\n",
                      r.a.c_str(), r.b.c_str(), static_cast<double>(r.threat_a),
                      static_cast<double>(r.threat_b), static_cast<double>(r.delta),
                      static_cast<double>(r.win_rate_a * 100.0f));
        f << buf;
    }
    f << "\nA pairing where every sample timed out is omitted: it has no opinion about who is "
         "stronger, and reporting 0% would be a lie about that.\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string out_dir = "duelsim_out";
    int32_t level = 1;
    int32_t samples = 9;
    int32_t max_level = 20;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&](int32_t fallback) {
            return (i + 1 < argc) ? std::atoi(argv[++i]) : fallback;
        };
        if (std::strcmp(a, "--out") == 0 && i + 1 < argc) {
            out_dir = argv[++i];
        } else if (std::strcmp(a, "--level") == 0) {
            level = next(level);
        } else if (std::strcmp(a, "--samples") == 0) {
            samples = next(samples);
        } else if (std::strcmp(a, "--max-level") == 0) {
            max_level = next(max_level);
        } else {
            std::fprintf(stderr,
                         "usage: badlands_duelsim [--out DIR] [--level N] [--samples N] "
                         "[--max-level N]\n");
            return 2;
        }
    }
    level = std::max(1, level);
    samples = std::max(1, samples);
    max_level = std::max(level, max_level);

    // No <filesystem> dance: the tool is run from the repo root and the caller
    // makes the directory, exactly like --record does for frame dumps.
    const std::string mk = "mkdir -p '" + out_dir + "'";
    if (std::system(mk.c_str()) != 0) {
        std::fprintf(stderr, "duelsim: cannot create '%s'\n", out_dir.c_str());
        return 1;
    }

    std::printf("duelsim: %zu creatures, level %d, %d samples each (%zu duels)\n", kRoster.size(),
                level, samples,
                kRoster.size() * kRoster.size() * static_cast<size_t>(samples));
    const auto matrix = duelsim::run_matrix(kRoster, level, samples);

    bool ok = write_matrix_md(out_dir + "/duel_matrix.md", kRoster, matrix, level, samples);
    const auto calib = calibration_rows(kRoster, matrix, level);
    ok = write_calibration_md(out_dir + "/calibration.md", calib, level) && ok;

    // The calibration scatter: the empirical answer to the doc's open question.
    {
        duelsim::ChartSpec spec;
        spec.title = "Observed win rate vs threat difference (level " + std::to_string(level) + ")";
        spec.x_label = "threat(A) - threat(B)";
        spec.y_label = "A's win rate";
        spec.has_reference = true;
        spec.reference_y = 0.5f;
        spec.reference_label = "even";
        duelsim::ChartSeries s;
        s.label = "pairing";
        s.colour = "#4a7fd0";
        s.points_only = true;
        for (const CalibRow& r : calib) {
            s.points.emplace_back(r.delta, r.win_rate_a);
        }
        spec.series.push_back(std::move(s));
        ok = duelsim::WriteChartSvg(out_dir + "/threat_calibration.svg", spec) && ok;
    }

    // The authored calibration curve itself, and the raw parameters beside it.
    {
        duelsim::ChartSpec spec;
        spec.title = "Threat targets by level (authored)";
        spec.x_label = "level";
        spec.y_label = "threat";
        for (size_t i = 0; i < kClasses.size(); ++i) {
            duelsim::ChartSeries s;
            s.label = CreatureName(kClasses[i]);
            s.colour = kClassColours[i];
            for (int32_t L = 1; L <= max_level; ++L) {
                s.points.emplace_back(static_cast<float>(L), threat_target(kClasses[i], L));
            }
            spec.series.push_back(std::move(s));
        }
        ok = duelsim::WriteChartSvg(out_dir + "/threat_targets.svg", spec) && ok;
    }

    struct StatChart {
        const char* file;
        const char* title;
        const char* y;
        float duelsim::LevelStats::*field;
    };
    const StatChart kCharts[] = {
        {"stat_hp.svg", "Max HP by level", "max hp", &duelsim::LevelStats::max_hp},
        {"stat_damage.svg", "Primary attack damage by level", "base damage",
         &duelsim::LevelStats::damage},
        {"stat_armour.svg", "Armour by level", "armour", &duelsim::LevelStats::armour},
    };
    for (const StatChart& ch : kCharts) {
        duelsim::ChartSpec spec;
        spec.title = ch.title;
        spec.x_label = "level";
        spec.y_label = ch.y;
        for (size_t i = 0; i < kClasses.size(); ++i) {
            duelsim::ChartSeries s;
            s.label = CreatureName(kClasses[i]);
            s.colour = kClassColours[i];
            for (int32_t L = 1; L <= max_level; ++L) {
                s.points.emplace_back(static_cast<float>(L),
                                      duelsim::stats_at_level(kClasses[i], L).*ch.field);
            }
            spec.series.push_back(std::move(s));
        }
        ok = duelsim::WriteChartSvg(out_dir + "/" + ch.file, spec) && ok;
    }

    std::printf("duelsim: wrote %s/{duel_matrix.md, calibration.md, threat_calibration.svg, "
                "threat_targets.svg, stat_hp.svg, stat_damage.svg, stat_armour.svg}\n",
                out_dir.c_str());
    return ok ? 0 : 1;
}
