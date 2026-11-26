// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/config.hpp"
#include "util/file_io.hpp"
#include "util/sha256.hpp"
#include "util/strings.hpp"

#include <doctest/doctest.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace em34xx;

TEST_CASE("sha256 known answers") {
    CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(""), 0) ==
          "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string abc = "abc";
    CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(abc.data()), abc.size()) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(sha256_hex(reinterpret_cast<const uint8_t*>(two.data()), two.size()) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST_CASE("number parsing") {
    uint64_t v = 0;
    CHECK(parse_uint("1f", v, 16));
    CHECK(v == 0x1F);
    CHECK(parse_uint("0x1F", v, 10));
    CHECK(v == 0x1F);
    CHECK(parse_uint("1fh", v, 10));
    CHECK(v == 0x1F);
    CHECK(parse_uint("#31", v, 16));
    CHECK(v == 31);
    CHECK_FALSE(parse_uint("12z", v, 10));
    CHECK_FALSE(parse_uint("", v, 10));
}

TEST_CASE("word splitting honours quotes") {
    auto w = split_words("e 100 \"hello world\" 0d");
    REQUIRE(w.size() == 4);
    CHECK(w[2] == "hello world");
}

TEST_CASE("command line overrides") {
    AppConfig cfg;
    std::string err;
    const char* argv[] = {"em34xx", "--model", "3486", "--gdb=4321", "--unmapped", "log",
                          "--tn5250-host", "ibmi.example", "--tn5250-env", "KBDTYPE=USB", "--batch"};
    REQUIRE(parse_command_line(int(sizeof argv / sizeof argv[0]), const_cast<char**>(argv), cfg, err));
    CHECK(cfg.model == "3486");
    CHECK(cfg.gdb);
    CHECK(cfg.gdb_port == 4321);
    CHECK(cfg.unmapped == UnmappedPolicy::Log);
    CHECK(cfg.tn5250.host == "ibmi.example");
    CHECK(cfg.tn5250.env["KBDTYPE"] == "USB");
    CHECK(cfg.batch);
    CHECK(cfg.headless);

    AppConfig bad;
    const char* argv2[] = {"em34xx", "--station-address", "7"};
    CHECK_FALSE(parse_command_line(3, const_cast<char**>(argv2), bad, err));
}

TEST_CASE("--gdb-wait takes a port like --gdb") {
    AppConfig cfg;
    std::string err;
    const char* argv[] = {"em34xx", "--gdb-wait=4322"};
    REQUIRE(parse_command_line(2, const_cast<char**>(argv), cfg, err));
    CHECK(cfg.gdb);
    CHECK(cfg.gdb_wait);
    CHECK(cfg.gdb_port == 4322);
    AppConfig plain;
    const char* argv2[] = {"em34xx", "--gdb=4323", "--gdb-wait"};
    REQUIRE(parse_command_line(3, const_cast<char**>(argv2), plain, err));
    CHECK(plain.gdb_port == 4323);
    CHECK(plain.gdb_wait);
}

TEST_CASE("atomic file replacement") {
    const std::string path = "em34xx_test_file_io.bin";
    const auto contents = [&] {
        std::ifstream f(path, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    };
    std::string err;
    const uint8_t a[] = {'o', 'n', 'e'}, b[] = {'t', 'w', 'o', '!'};
    REQUIRE(write_file_atomic(path, a, sizeof a, err));
    CHECK(contents() == "one");
    REQUIRE(write_file_atomic(path, b, sizeof b, err));   // replaces
    CHECK(contents() == "two!");
    CHECK_FALSE(std::ifstream(path + ".tmp").good());
    uint64_t size = 0;
    CHECK(file_size(path, size));
    CHECK(size == 4);
    CHECK_FALSE(file_size(path + ".absent", size));

    // A failed write reports the error and keeps the previous file.
    CHECK_FALSE(write_file_atomic("em34xx_no_such_dir/x.bin", a, sizeof a, err));
    CHECK_FALSE(err.empty());
    CHECK_FALSE(replace_file(path + ".absent", path, err));
    CHECK(contents() == "two!");
    std::remove(path.c_str());
}
