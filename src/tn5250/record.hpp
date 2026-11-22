// SPDX-License-Identifier: GPL-3.0-or-later
// TN5250 logical record framing (RFC 1205 section 3).
//
// A logical record is a 6-byte fixed header (length, record type 12A0,
// reserved), a variable header whose first byte is its own length (always 4:
// length, flags, reserved, opcode), then the 5250 data stream. On the wire it
// is followed by IAC EOR; IAC doubling is the telnet layer's business.
#pragma once

#include "tn5250/session_api.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace em34xx {
namespace tn5250 {

constexpr uint16_t kGdsRecordType = 0x12A0;

// The logical length is 16 bits and counts the 10 header bytes.
constexpr size_t kMaxRecordData = 0xFFFF - 10;

// Header + data, not yet telnet-escaped. Empty if the data is longer than
// kMaxRecordData: the length field cannot say so.
std::vector<uint8_t> encode_record(const Record& rec);

// Parse an unescaped logical record. Returns false (with a reason) if the
// record is too short or its header is malformed. A logical-length mismatch
// is tolerated and reported in `warning`.
bool decode_record(const std::vector<uint8_t>& bytes, Record& out, std::string& error,
                   std::string* warning = nullptr);

const char* opcode_name(uint8_t opcode);
std::string flags_text(uint8_t flags);

} // namespace tn5250
} // namespace em34xx
