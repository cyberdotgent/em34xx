// SPDX-License-Identifier: GPL-3.0-or-later
// EBCDIC <-> Unicode translation for the TN5250 side.
//
// The tables are the published single-byte EBCDIC code pages (IBM CCSID
// 37 US/Canada, 500 International, 273 Austria/Germany) as given by the
// IBM/Unicode mapping tables. Control positions (0x00-0x3F, 0xFF) map to
// their C0/C1 equivalents; callers that render screens should treat them
// as blanks.
#pragma once

#include <cstdint>
#include <string>

namespace em34xx {
namespace tn5250 {

class CodePage {
public:
    // Returns nullptr for an unsupported CCSID ("37", "037", "500", "273").
    static const CodePage* find(const std::string& ccsid);

    unsigned ccsid() const { return ccsid_; }
    char32_t to_unicode(uint8_t ebcdic) const { return table_[ebcdic]; }
    // Returns false if the character has no single-byte EBCDIC code.
    bool from_unicode(char32_t ch, uint8_t& out) const;

    // Suggested RFC 4777 keyboard type / character set for this code page.
    // Empty when we do not know a good value.
    const char* kbdtype() const { return kbdtype_; }
    const char* charset() const { return charset_; }

    CodePage(unsigned ccsid, const uint16_t* table, const char* kbdtype, const char* charset)
        : ccsid_(ccsid), table_(table), kbdtype_(kbdtype), charset_(charset) {}

private:
    unsigned ccsid_;
    const uint16_t* table_;
    const char* kbdtype_;
    const char* charset_;
};

// Append a code point to a UTF-8 string.
void append_utf8(std::string& out, char32_t cp);

} // namespace tn5250
} // namespace em34xx
