// CRT presentation: turns a terminal's native raster into what its tube
// shows, at a given output size.
//
// Observed on a real 3486/3477-class tube (photos of the screen):
//  - each raster line is drawn by the beam as a distinct horizontal stripe,
//    with a visibly darker gap to the next line;
//  - along a line the beam is continuous, so adjacent lit dots merge and
//    stroke ends are soft;
//  - the raster is inset in the glass with unequal horizontal and vertical
//    margins (Framebuffer::border_x / border_y).
//
// The output is computed per output pixel, so the scan-line structure is
// sampled at the final resolution and does not alias into moire when the
// window is resized.
#pragma once

#include "machine/peripherals.hpp"

#include <cstdint>
#include <vector>

namespace tn34xx {

struct CrtParams {
    bool enabled = true;      // false: sharp pixels, no scan-line structure
    // Beam profile across scan lines: Gaussian sigma in units of the line
    // pitch. ~0.30 leaves a dark gap between lines like the photographed
    // tube; larger values blend lines together.
    double beam_sigma = 0.30;
    // Beam spread along a line, as a Gaussian sigma in dots. Together with
    // the bloom gain this widens a 2-dot stroke to ~4 dots (a quarter of a
    // 16-dot cell), as photographed on a real tube.
    double spread = 0.9;
    // Brightness gain before clipping (phosphor saturation / bloom).
    double gain = 1.7;
};

// Size of the whole tube image (raster plus border) that fits in
// max_w x max_h while keeping the frame's display aspect.
void crt_fit(const Framebuffer& fb, int max_w, int max_h, int& out_w, int& out_h);

// Render fb into an out_w x out_h ARGB image of the whole tube.
void crt_render(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p, std::vector<uint32_t>& out);

// Incremental form for front ends that redraw the same tube many times: the
// beam spread along each raster line is computed once per changed raster line
// and kept, and only the output rows that a changed raster line reaches are
// recomputed. The image is identical to crt_render()'s.
class CrtRenderer {
public:
    // Brings `out` (owned by the caller, untouched between calls) up to date
    // with fb. Returns the range of output rows rewritten, [row0, row1);
    // row0 == row1 when nothing changed.
    struct Rows {
        int row0 = 0, row1 = 0;
        bool empty() const { return row0 >= row1; }
    };
    Rows render(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p, std::vector<uint32_t>& out);
    // Forget the cache (the next render recomputes everything).
    void invalidate() { valid_ = false; }

private:
    void setup(const Framebuffer& fb, int out_w, int out_h, const CrtParams& p);
    void spread_line(const Framebuffer& fb, int line);

    bool valid_ = false;
    // Geometry the cache was built for.
    int fb_w_ = 0, fb_h_ = 0, out_w_ = 0, out_h_ = 0;
    double border_x_ = 0, border_y_ = 0;
    CrtParams params_;
    std::vector<uint32_t> last_;   // raster of the previous call

    int ox0_ = 0, ox1_ = 0, oy0_ = 0, oy1_ = 0;
    // Horizontal sampling per output column: centre dot and tap weights.
    std::vector<int> xi_;
    std::vector<float> xw_;
    // Raster line contributions per output row: first line and weights.
    std::vector<int> row_line0_;
    std::vector<std::vector<float>> row_weights_;
    // Beam spread along each raster line at output columns (RGB floats).
    std::vector<float> spread_;
};

} // namespace tn34xx
