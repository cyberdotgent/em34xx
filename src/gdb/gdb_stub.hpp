// SPDX-License-Identifier: GPL-3.0-or-later
// GDB remote serial protocol stub.
//
// GDB is told the target is an i8086 with the i386 core register set. Since
// GDB has no notion of real-mode segmentation, the stub presents a flat view:
//   - eip is the *linear* address of CS:IP; writing eip moves IP within the
//     current CS (if the address is inside that segment's 64 KiB window);
//   - memory addresses in m/M/Z packets are linear (physical) addresses;
//   - cs/ss/ds/es hold the real segment values.
// "monitor <cmd>" in GDB runs a tn34xx monitor command. Its output is
// streamed to GDB as console-output ("O<hex>") packets while the command
// runs, followed by a final "OK".
#pragma once

#include "net/socket.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace tn34xx {

class Emulator;
class Monitor;

class GdbStub {
public:
    GdbStub(Emulator& emu, Monitor* monitor);
    ~GdbStub();

    bool start(uint16_t port, std::string& error);
    void stop();

    // Block until a client has connected (used by --gdb-wait).
    void wait_for_client();
    bool client_connected() const { return connected_.load(); }

private:
    void thread_main();
    void serve(TcpSocket& client);
    bool read_packet(TcpSocket& client, std::string& packet, bool& interrupted);
    bool send_packet(TcpSocket& client, const std::string& payload);
    std::string handle(const std::string& packet, TcpSocket& client, bool& detach);
    std::string resume_and_report(TcpSocket& client, bool single_step);
    std::string stop_reply();
    std::string read_registers();
    bool write_registers(const std::string& hex);
    std::string read_register(unsigned n);
    bool write_register(unsigned n, uint32_t value);
    std::string read_memory(uint32_t addr, uint32_t len);
    bool write_memory(uint32_t addr, const std::string& hex);
    std::string query(const std::string& packet);
    std::string monitor_command(const std::string& hex_cmd, TcpSocket& client);

    Emulator& emu_;
    Monitor* monitor_;
    TcpSocket listener_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    bool no_ack_ = false;
    bool enter_no_ack_ = false;
    int last_signal_ = 5;
};

} // namespace tn34xx
