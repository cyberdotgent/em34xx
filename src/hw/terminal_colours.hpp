// SPDX-License-Identifier: GPL-3.0-or-later
// The colours em34xx draws terminal screens with, shared by every display
// device. The terminals' cells say which colour (gun bits on the 5250
// units, a 3270 colour number on the 3483); the shades are the emulator's
// own choice, compared with photos of real colour units (project owner).
// Monochrome units draw grey levels, which the presentation tints with the
// phosphor.
#pragma once

#include <cstdint>

namespace em34xx {
namespace colours {

constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kGreen = 0xFF20E040u;
constexpr uint32_t kWhite = 0xFFF0F0F0u;
constexpr uint32_t kRed = 0xFFF03030u;
constexpr uint32_t kTurquoise = 0xFF30E0E0u;
constexpr uint32_t kYellow = 0xFFF0F040u;
constexpr uint32_t kPink = 0xFFF070D0u;
constexpr uint32_t kBlue = 0xFF5878FFu;
// Normal intensity is close to full brightness on a real tube (photos).
constexpr uint32_t kGreyNormal = 0xFFD0D0D0u;
constexpr uint32_t kGreyBright = 0xFFFFFFFFu;

} // namespace colours
} // namespace em34xx
