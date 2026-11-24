// SPDX-License-Identifier: GPL-3.0-or-later
// 3486 family (3486, 3487): where its board keeps the display controls that
// differ from the 3477's. docs/bringup-3486.md has the observations.
#pragma once

#include "devices/ibm3477/display.hpp"

namespace em34xx {

constexpr Ibm3477DisplayLayout kIbm3486DisplayLayout = {
    // Retrace as a fraction of the frame. The display test requires the
    // retrace flag (2012F bit 3) to drop within 3 to 9 status reads of its
    // start and to come back one field later [observed windows]; the 3477's
    // 1/90 lasts 10 reads here. 1/140 (about 120 us) is in the middle
    // [hypothesis: the real length is not known].
    140,
    // 20128 bit 3 inverts the retrace flag [hypothesis: a sync polarity
    // select; observed: set on tube types 3-5, clear on type 2].
    {0x28, 0x08},
    // The signature selector is 2012F bits 5-4 [observed].
    4,
    // 20128 bits 4-3 are constants of the tube here, so bit 4 is not
    // "picture on". The firmware writes the register without bit 6 while it
    // changes the mode and with it when the screen is ready [observed], so
    // bit 6 stands in for it [hypothesis].
    {0x28, 0x40},
    // The firmware never sets 20128 bit 7. It writes 20127 with bit 6 set
    // each time it places the cursor and with the bit clear to take it
    // away [observed].
    {0x27, 0x40},
};

} // namespace em34xx
