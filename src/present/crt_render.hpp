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

} // namespace tn34xx
