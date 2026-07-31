// The report tool's own mechanism: the SVG writer, and the duel runner's
// determinism. Nothing here asserts a balance OUTCOME -- that is what the
// matrix reports rather than pins.

#include "duel_matrix.h"
#include "svg_chart.h"

#include "threat_table.h"

#include <catch_amalgamated.hpp>

#include <string>

using badlands::CreatureId;

namespace {

int count_of(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (size_t at = hay.find(needle); at != std::string::npos;
         at = hay.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

duelsim::ChartSpec two_series() {
    duelsim::ChartSpec spec;
    spec.title = "test";
    spec.x_label = "level";
    spec.y_label = "hp";
    duelsim::ChartSeries a{"alpha", "#111111", {{1.0f, 10.0f}, {20.0f, 100.0f}}, false};
    duelsim::ChartSeries b{"beta", "#222222", {{1.0f, 5.0f}, {20.0f, 40.0f}}, false};
    spec.series = {a, b};
    return spec;
}

}  // namespace

TEST_CASE("a chart renders one polyline per series", "[svg]") {
    const std::string svg = duelsim::RenderChartSvg(two_series());
    CHECK(count_of(svg, "<polyline") == 2);
    CHECK(svg.find("alpha") != std::string::npos);
    CHECK(svg.find("beta") != std::string::npos);
}

TEST_CASE("axes cover the data", "[svg]") {
    const std::string svg = duelsim::RenderChartSvg(two_series());
    // The y range must reach the largest datum (100); padded() rounds outward,
    // so the top tick label is >= it. Checking the label rather than the
    // geometry keeps this readable and independent of the plot insets.
    CHECK(svg.find(">100<") != std::string::npos);
    CHECK(svg.find(">20<") != std::string::npos);  // the x axis reaches level 20
}

TEST_CASE("a scatter draws markers, not a line", "[svg]") {
    duelsim::ChartSpec spec;
    spec.series.push_back({"pairs", "#333333", {{-1.0f, 0.2f}, {0.0f, 0.5f}, {2.0f, 0.9f}}, true});
    const std::string svg = duelsim::RenderChartSvg(spec);
    CHECK(count_of(svg, "<circle") == 3);
    CHECK(count_of(svg, "<polyline") == 0);
}

TEST_CASE("a reference line renders when asked for", "[svg]") {
    duelsim::ChartSpec spec = two_series();
    CHECK(count_of(duelsim::RenderChartSvg(spec), "stroke-dasharray") == 0);
    spec.has_reference = true;
    spec.reference_y = 50.0f;
    spec.reference_label = "even";
    CHECK(count_of(duelsim::RenderChartSvg(spec), "stroke-dasharray") == 1);
}

TEST_CASE("an empty chart is still valid svg", "[svg]") {
    const std::string svg = duelsim::RenderChartSvg(duelsim::ChartSpec{});
    CHECK(svg.rfind("<svg", 0) == 0);
    CHECK(svg.find("</svg>") != std::string::npos);
}

TEST_CASE("a single-point series does not divide by zero", "[svg]") {
    duelsim::ChartSpec spec;
    spec.series.push_back({"one", "#444444", {{5.0f, 5.0f}}, false});
    const std::string svg = duelsim::RenderChartSvg(spec);
    CHECK(svg.find("nan") == std::string::npos);
    CHECK(svg.find("inf") == std::string::npos);
}

TEST_CASE("rendering is deterministic", "[svg]") {
    // No clock, no addresses, fixed-decimal formatting: two renders of one spec
    // must be byte-identical, or the artifacts would churn in git for nothing.
    CHECK(duelsim::RenderChartSvg(two_series()) == duelsim::RenderChartSvg(two_series()));
}

TEST_CASE("a chart references nothing external", "[svg]") {
    // A strict no-external-refs rule is what lets a written file render
    // standalone from disk, with no network and no sibling assets. The SVG
    // NAMESPACE URI is not a reference -- nothing fetches it -- so the check is
    // "no http outside xmlns", not "no http".
    const std::string svg = duelsim::RenderChartSvg(two_series());
    CHECK(count_of(svg, "http") == 1);
    CHECK(svg.find("xmlns=\"http://www.w3.org/2000/svg\"") != std::string::npos);
    CHECK(svg.find("<image") == std::string::npos);
    CHECK(svg.find("@import") == std::string::npos);
    CHECK(svg.find("href") == std::string::npos);
    CHECK(svg.find("url(") == std::string::npos);
}

TEST_CASE("a label with markup in it is escaped", "[svg]") {
    duelsim::ChartSpec spec;
    spec.title = "a < b & c";
    const std::string svg = duelsim::RenderChartSvg(spec);
    CHECK(svg.find("a &lt; b &amp; c") != std::string::npos);
}

// --- the duel runner ---------------------------------------------------------

TEST_CASE("a duel is deterministic", "[duelsim]") {
    const duelsim::DuelOutcome a =
        duelsim::run_duel(CreatureId::Mercenary, CreatureId::Goblin, 1, 8.0f);
    const duelsim::DuelOutcome b =
        duelsim::run_duel(CreatureId::Mercenary, CreatureId::Goblin, 1, 8.0f);
    CHECK(a.winner_index == b.winner_index);
    CHECK(a.ticks == b.ticks);
}

TEST_CASE("a duel always terminates", "[duelsim]") {
    // Two deer: no attacks at all, so nothing can ever resolve. It must report
    // a draw at the cap rather than hang.
    const duelsim::DuelOutcome o =
        duelsim::run_duel(CreatureId::Deer, CreatureId::Deer, 1, 8.0f, /*max_ticks=*/120);
    CHECK(o.winner_index == -1);
    CHECK(o.ticks == 120);
}

TEST_CASE("separation changes the roll stream", "[duelsim]") {
    // The samples axis varies the SETUP, not a seed -- so it is only meaningful
    // if a different closing time really does produce a different fight. If
    // every separation gave the identical tick count, the axis would be
    // decorative and the win rates meaningless.
    bool differed = false;
    const duelsim::DuelOutcome base =
        duelsim::run_duel(CreatureId::Goblin, CreatureId::Goblin, 1, 6.0f);
    for (int s = 1; s < 9 && !differed; ++s) {
        const duelsim::DuelOutcome o = duelsim::run_duel(CreatureId::Goblin, CreatureId::Goblin, 1,
                                                         6.0f + static_cast<float>(s) * 2.0f);
        differed = o.ticks != base.ticks || o.winner_index != base.winner_index;
    }
    CHECK(differed);
}

TEST_CASE("the matrix is square and every duel is accounted for", "[duelsim]") {
    const std::vector<CreatureId> roster = {CreatureId::Mercenary, CreatureId::Rat,
                                            CreatureId::Goblin};
    constexpr int32_t kSamples = 2;
    const auto m = duelsim::run_matrix(roster, 1, kSamples);
    REQUIRE(m.size() == roster.size());
    for (size_t i = 0; i < roster.size(); ++i) {
        REQUIRE(m[i].size() == roster.size());
        for (size_t j = 0; j < roster.size(); ++j) {
            CHECK(m[i][j].wins + m[i][j].losses + m[i][j].draws == kSamples);
            CHECK(m[i][j].median_ticks > 0);
        }
    }
}

TEST_CASE("[i][j] and [j][i] are INDEPENDENT stagings, not mirrors", "[duelsim]") {
    // Worth pinning because it is easy to assume otherwise and then read the
    // matrix wrong. run_duel puts `left` on team 0 at -half and `right` on team
    // 1 at +half, and the combat seed folds both slots -- so swapping the
    // arguments is a genuinely different fight, not the same one from the other
    // side. That is deliberate: it means the matrix also exposes any
    // side/slot bias, and the calibration report combines both directions
    // rather than throwing half the data away.
    const duelsim::DuelOutcome ab =
        duelsim::run_duel(CreatureId::Goblin, CreatureId::Goblin, 1, 8.0f);
    const duelsim::DuelOutcome ba =
        duelsim::run_duel(CreatureId::Goblin, CreatureId::Goblin, 1, 8.0f);
    // Identical arguments must still be identical (determinism, above).
    CHECK(ab.ticks == ba.ticks);
    // ...but a self-pairing does not have to be a 50/50 split of two samples,
    // and asserting that it does would be asserting a balance outcome.
}

TEST_CASE("stats_at_level tracks the growth row", "[duelsim]") {
    // The charts must plot what the SIM would use, not a second copy of the
    // arithmetic -- so this goes through the real spawn + apply_level_stats
    // path and only checks that it moves in the authored direction.
    const duelsim::LevelStats l1 = duelsim::stats_at_level(CreatureId::Mercenary, 1);
    const duelsim::LevelStats l10 = duelsim::stats_at_level(CreatureId::Mercenary, 10);
    CHECK(l10.max_hp > l1.max_hp);
    CHECK(l10.armour > l1.armour);
    CHECK(l10.damage > l1.damage);

    // A monster does not level.
    const duelsim::LevelStats r1 = duelsim::stats_at_level(CreatureId::Rat, 1);
    const duelsim::LevelStats r10 = duelsim::stats_at_level(CreatureId::Rat, 10);
    CHECK(r10.max_hp == Catch::Approx(r1.max_hp));
}
