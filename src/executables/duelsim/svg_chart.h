// A line/scatter chart as a self-contained <svg> string.
//
// Deliberately tiny and hand-rolled: SVG is text, this needs axes and
// polylines, and taking on a charting dependency for that would be absurd. No
// external references of any kind -- no fonts, no stylesheets, no images -- so
// a written file renders standalone in any browser.
//
// Pure: no game types, no engine, no world. Rendering is a deterministic
// function of the spec (no clock, no addresses), which is what lets a test
// compare two renders byte for byte.

#pragma once

#include <string>
#include <utility>
#include <vector>

namespace duelsim {

struct ChartSeries {
    std::string label;
    std::string colour = "#4a7fd0";  // any CSS colour literal
    std::vector<std::pair<float, float>> points;
    // Draw discrete markers instead of a connected line -- what a scatter
    // (observed win rate vs threat difference) wants and a curve does not.
    bool points_only = false;
};

struct ChartSpec {
    std::string title;
    std::string x_label;
    std::string y_label;
    std::vector<ChartSeries> series;
    int width = 720;
    int height = 440;
    // Optional horizontal reference line (e.g. the 50% mark a calibration
    // scatter is read against). Drawn only when `has_reference` is set.
    bool has_reference = false;
    float reference_y = 0.0f;
    std::string reference_label;
};

// Axis ranges cover the union of every series' data, padded outward to round
// numbers. An empty spec renders a valid, empty chart rather than failing.
std::string RenderChartSvg(const ChartSpec& spec);

// Writes RenderChartSvg(spec) to `path`. False (after a message on stderr) if
// the file cannot be opened.
bool WriteChartSvg(const std::string& path, const ChartSpec& spec);

}  // namespace duelsim
