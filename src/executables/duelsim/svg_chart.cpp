#include "svg_chart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

namespace duelsim {

namespace {

// Plot-area insets, leaving room for the axis labels and the legend.
constexpr int kLeft = 64;
constexpr int kRight = 168;  // the legend column
constexpr int kTop = 44;
constexpr int kBottom = 52;

// Fixed decimals rather than the default float formatting: identical specs
// must produce identical bytes, and %g's shortest-representation rules are
// harder to reason about than "always three places".
std::string num(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", static_cast<double>(v));
    return buf;
}

std::string tick_label(float v) {
    char buf[32];
    // Integers read as integers -- a level axis labelled "5.00" is noise.
    if (std::fabs(v - std::round(v)) < 1e-4f) {
        std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(std::lround(v)));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    }
    return buf;
}

// XML-escape the few characters that can appear in a caller-supplied label.
std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

struct Range {
    float lo = 0.0f, hi = 1.0f;
};

// Pad a data range outward to something round, and never return a zero-width
// range (a single-point series would otherwise divide by zero when mapped).
Range padded(float lo, float hi) {
    if (!(lo <= hi)) {  // also catches the no-data case, where lo/hi are +/-inf
        return Range{0.0f, 1.0f};
    }
    if (hi - lo < 1e-6f) {
        const float pad = std::max(1.0f, std::fabs(hi) * 0.1f);
        return Range{lo - pad, hi + pad};
    }
    const float span = hi - lo;
    // Round the step to 1/2/5 x 10^n so the tick labels are readable.
    const float raw = span / 4.0f;
    const float mag = std::pow(10.0f, std::floor(std::log10(raw)));
    const float norm = raw / mag;
    const float step = (norm <= 1.0f ? 1.0f : norm <= 2.0f ? 2.0f : norm <= 5.0f ? 5.0f : 10.0f) * mag;
    return Range{std::floor(lo / step) * step, std::ceil(hi / step) * step};
}

}  // namespace

std::string RenderChartSvg(const ChartSpec& spec) {
    const int w = std::max(240, spec.width);
    const int h = std::max(180, spec.height);
    const float px0 = static_cast<float>(kLeft);
    const float px1 = static_cast<float>(w - kRight);
    const float py0 = static_cast<float>(h - kBottom);  // y grows DOWN in svg
    const float py1 = static_cast<float>(kTop);

    float minx = std::numeric_limits<float>::infinity();
    float maxx = -std::numeric_limits<float>::infinity();
    float miny = std::numeric_limits<float>::infinity();
    float maxy = -std::numeric_limits<float>::infinity();
    for (const ChartSeries& s : spec.series) {
        for (const auto& [x, y] : s.points) {
            minx = std::min(minx, x);
            maxx = std::max(maxx, x);
            miny = std::min(miny, y);
            maxy = std::max(maxy, y);
        }
    }
    if (spec.has_reference) {
        miny = std::min(miny, spec.reference_y);
        maxy = std::max(maxy, spec.reference_y);
    }
    const Range rx = padded(minx, maxx);
    const Range ry = padded(miny, maxy);

    auto mapx = [&](float x) { return px0 + (x - rx.lo) / (rx.hi - rx.lo) * (px1 - px0); };
    auto mapy = [&](float y) { return py0 + (y - ry.lo) / (ry.hi - ry.lo) * (py1 - py0); };

    std::ostringstream o;
    o << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << w << "\" height=\"" << h
      << "\" viewBox=\"0 0 " << w << " " << h << "\">\n";
    // Theme-agnostic: an explicit light plot background, so the chart reads the
    // same whatever the page around it does.
    o << "<rect width=\"" << w << "\" height=\"" << h << "\" fill=\"#fbfaf7\"/>\n";
    o << "<g font-family=\"Georgia,serif\" font-size=\"12\" fill=\"#3a3630\">\n";
    o << "<text x=\"" << kLeft << "\" y=\"24\" font-size=\"15\">" << esc(spec.title)
      << "</text>\n";

    // Grid + ticks: 5 lines per axis, at the rounded step padded() chose.
    for (int i = 0; i <= 4; ++i) {
        const float t = static_cast<float>(i) / 4.0f;
        const float yv = ry.lo + t * (ry.hi - ry.lo);
        const float yp = mapy(yv);
        o << "<line x1=\"" << num(px0) << "\" y1=\"" << num(yp) << "\" x2=\"" << num(px1)
          << "\" y2=\"" << num(yp) << "\" stroke=\"#e2ded4\"/>\n";
        o << "<text x=\"" << num(px0 - 8.0f) << "\" y=\"" << num(yp + 4.0f)
          << "\" text-anchor=\"end\" fill=\"#6b6355\">" << tick_label(yv) << "</text>\n";

        const float xv = rx.lo + t * (rx.hi - rx.lo);
        const float xp = mapx(xv);
        o << "<text x=\"" << num(xp) << "\" y=\"" << num(py0 + 20.0f)
          << "\" text-anchor=\"middle\" fill=\"#6b6355\">" << tick_label(xv) << "</text>\n";
    }
    o << "<line x1=\"" << num(px0) << "\" y1=\"" << num(py0) << "\" x2=\"" << num(px1)
      << "\" y2=\"" << num(py0) << "\" stroke=\"#8a8172\"/>\n";
    o << "<line x1=\"" << num(px0) << "\" y1=\"" << num(py0) << "\" x2=\"" << num(px0)
      << "\" y2=\"" << num(py1) << "\" stroke=\"#8a8172\"/>\n";
    o << "<text x=\"" << num((px0 + px1) * 0.5f) << "\" y=\"" << (h - 14)
      << "\" text-anchor=\"middle\" fill=\"#6b6355\">" << esc(spec.x_label) << "</text>\n";
    o << "<text x=\"18\" y=\"" << num((py0 + py1) * 0.5f)
      << "\" text-anchor=\"middle\" fill=\"#6b6355\" transform=\"rotate(-90 18 "
      << num((py0 + py1) * 0.5f) << ")\">" << esc(spec.y_label) << "</text>\n";

    if (spec.has_reference) {
        const float yp = mapy(spec.reference_y);
        o << "<line x1=\"" << num(px0) << "\" y1=\"" << num(yp) << "\" x2=\"" << num(px1)
          << "\" y2=\"" << num(yp)
          << "\" stroke=\"#b5342f\" stroke-width=\"1\" stroke-dasharray=\"5 4\"/>\n";
        if (!spec.reference_label.empty()) {
            o << "<text x=\"" << num(px1 - 4.0f) << "\" y=\"" << num(yp - 5.0f)
              << "\" text-anchor=\"end\" fill=\"#b5342f\">" << esc(spec.reference_label)
              << "</text>\n";
        }
    }

    int legend_row = 0;
    for (const ChartSeries& s : spec.series) {
        if (s.points_only) {
            for (const auto& [x, y] : s.points) {
                o << "<circle cx=\"" << num(mapx(x)) << "\" cy=\"" << num(mapy(y))
                  << "\" r=\"3.5\" fill=\"" << esc(s.colour) << "\" fill-opacity=\"0.75\"/>\n";
            }
        } else if (!s.points.empty()) {
            o << "<polyline fill=\"none\" stroke=\"" << esc(s.colour)
              << "\" stroke-width=\"2\" points=\"";
            for (size_t i = 0; i < s.points.size(); ++i) {
                if (i != 0) {
                    o << ' ';
                }
                o << num(mapx(s.points[i].first)) << ',' << num(mapy(s.points[i].second));
            }
            o << "\"/>\n";
        }
        const float ly = static_cast<float>(kTop + 8 + legend_row * 20);
        o << "<rect x=\"" << (w - kRight + 16) << "\" y=\"" << num(ly - 8.0f)
          << "\" width=\"12\" height=\"3\" fill=\"" << esc(s.colour) << "\"/>\n";
        o << "<text x=\"" << (w - kRight + 34) << "\" y=\"" << num(ly)
          << "\" fill=\"#3a3630\">" << esc(s.label) << "</text>\n";
        ++legend_row;
    }

    o << "</g>\n</svg>\n";
    return o.str();
}

bool WriteChartSvg(const std::string& path, const ChartSpec& spec) {
    std::ofstream f(path);
    if (!f.good()) {
        std::fprintf(stderr, "duelsim: cannot write '%s'\n", path.c_str());
        return false;
    }
    f << RenderChartSvg(spec);
    return f.good();
}

}  // namespace duelsim
