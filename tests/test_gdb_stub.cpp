// SPDX-License-Identifier: GPL-3.0-or-later
// GDB stub packet framing, over a loopback connection.
#include "gdb/gdb_stub.hpp"
#include "machine/emulator.hpp"
#include "machine/model_info.hpp"

#include <doctest/doctest.h>

#include <cstdlib>
#include <memory>
#include <string>

using namespace em34xx;

namespace {

ModelInfo test_model() {
    ModelInfo m;
    m.name = "test";
    m.cpu = CpuLevel::I8088;
    m.address_bits = 20;
    return m;
}

// Send one packet (the stub does not verify checksums) and return the
// payload of the reply, or "<closed>" when the link drops or stays silent.
std::string transact(TcpSocket& s, const std::string& payload) {
    const std::string frame = "$" + payload + "#00";
    if (!s.send_all(frame.data(), frame.size())) return "<closed>";
    std::string reply;
    bool in_body = false;
    char c;
    while (s.recv(&c, 1, 5000) > 0) {
        if (c == '$') in_body = true;
        else if (c == '#') {
            s.send_all("+", 1);
            return reply;
        } else if (in_body) reply += c;
    }
    return "<closed>";
}

} // namespace

TEST_CASE("gdb stub: received packets are bounded by the advertised PacketSize") {
    static const ModelInfo info = test_model();
    auto machine = std::make_unique<Machine>(info);
    machine->bus().map_ram("ram", 0, 0x20000);
    Emulator emu(std::move(machine));
    GdbStub stub(emu, nullptr);

    // No ephemeral-port query in TcpSocket: try a few fixed ports.
    uint16_t port = 0;
    std::string error;
    for (uint16_t p = 47341; p < 47349 && !port; ++p)
        if (stub.start(p, error)) port = p;
    REQUIRE_MESSAGE(port != 0, error);

    TcpSocket client;
    REQUIRE_MESSAGE(client.connect("127.0.0.1", port, error), error);

    const std::string supported = transact(client, "qSupported");
    const auto at = supported.find("PacketSize=");
    REQUIRE(at != std::string::npos);
    const size_t limit = std::strtoul(supported.c_str() + at + 11, nullptr, 16);
    REQUIRE(limit >= 0x400);

    // A memory write that fills a packet exactly is accepted (the stub
    // takes the length from the data, not from the length field).
    const std::string head = "M100,1000:";
    std::string write = head + std::string(limit - head.size(), '5');
    if (write.size() % 2) write.pop_back();
    CHECK(transact(client, write) == "OK");
    CHECK(emu.with_machine([](Machine& m) { return m.bus().peek8(0x100); }) == 0x55);

    // One byte more, with no terminator in sight: the stub hangs up rather
    // than keep buffering. Nothing is sent past the byte that trips the
    // limit, so the close cannot race the send.
    const std::string flood = "$" + std::string(limit + 1, 'x');
    REQUIRE(client.send_all(flood.data(), flood.size()));
    // (Bytes still in flight from the earlier exchanges are read past.)
    char c;
    int n = 1;
    for (int i = 0; i < 64 && n > 0; ++i) n = client.recv(&c, 1, 5000);
    CHECK(n < 0);

    stub.stop();
}
