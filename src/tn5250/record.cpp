// SPDX-License-Identifier: GPL-3.0-or-later
#include "tn5250/record.hpp"

#include "util/strings.hpp"

namespace em34xx {
namespace tn5250 {

std::vector<uint8_t> encode_record(const Record& rec) {
    std::vector<uint8_t> out;
    if (rec.data.size() > kMaxRecordData) return out;
    const size_t len = 10 + rec.data.size();
    out.reserve(len);
    out.push_back(uint8_t(len >> 8));
    out.push_back(uint8_t(len));
    out.push_back(uint8_t(kGdsRecordType >> 8));
    out.push_back(uint8_t(kGdsRecordType));
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x04);          // variable header length
    out.push_back(rec.flags);
    out.push_back(0x00);          // reserved
    out.push_back(rec.opcode);
    out.insert(out.end(), rec.data.begin(), rec.data.end());
    return out;
}

bool decode_record(const std::vector<uint8_t>& b, Record& out, std::string& error, std::string* warning) {
    if (b.size() < 10) {
        error = format("record of %zu bytes is shorter than the 10-byte header", b.size());
        return false;
    }
    const unsigned type = (unsigned(b[2]) << 8) | b[3];
    if (type != kGdsRecordType) {
        error = format("record type %04X is not GDS (12A0)", type);
        return false;
    }
    const size_t varlen = b[6];
    if (varlen < 4 || 6 + varlen > b.size()) {
        error = format("variable header length %zu is invalid", varlen);
        return false;
    }
    const size_t loglen = (size_t(b[0]) << 8) | b[1];
    if (warning && loglen != b.size())
        *warning = format("logical record length %zu, received %zu bytes", loglen, b.size());
    out.flags = b[7];
    out.opcode = b[9];
    out.data.assign(b.begin() + long(6 + varlen), b.end());
    return true;
}

const char* opcode_name(uint8_t opcode) {
    switch (opcode) {
    case kOpNoOp: return "No-op";
    case kOpInvite: return "Invite";
    case kOpOutputOnly: return "Output only";
    case kOpPutGet: return "Put/Get";
    case kOpSaveScreen: return "Save screen";
    case kOpRestoreScreen: return "Restore screen";
    case kOpReadImmediate: return "Read immediate";
    case kOpReadScreen: return "Read screen";
    case kOpCancelInvite: return "Cancel invite";
    case kOpMessageOn: return "Message light on";
    case kOpMessageOff: return "Message light off";
    default: return "reserved";
    }
}

std::string flags_text(uint8_t flags) {
    std::string s;
    auto add = [&](uint8_t bit, const char* name) {
        if (flags & bit) {
            if (!s.empty()) s += '|';
            s += name;
        }
    };
    add(kFlagErr, "ERR");
    add(kFlagAtn, "ATN");
    add(kFlagSrq, "SRQ");
    add(kFlagTrq, "TRQ");
    add(kFlagHlp, "HLP");
    return s.empty() ? "-" : s;
}

} // namespace tn5250
} // namespace em34xx
