#include "devtools/console.hpp"

#include "machine/emulator.hpp"
#include "monitor/monitor.hpp"
#include "util/log.hpp"
#include "util/strings.hpp"

#include <replxx.hxx>

#include <cerrno>
#include <cstdio>

namespace tn34xx {

namespace {

class ReplxxConsole : public Console {
public:
    ReplxxConsole(Monitor& monitor, Emulator& emu, std::string history_file)
        : monitor_(monitor), emu_(emu), history_file_(std::move(history_file)) {}

    void run() override {
        rx_.install_window_change_handler();
        rx_.set_max_history_size(1000);
        if (!history_file_.empty()) rx_.history_load(history_file_);
        const std::vector<std::string> names = monitor_.command_names();
        rx_.set_completion_callback([names](const std::string& input, int& context_len) {
            replxx::Replxx::completions_t out;
            if (input.find(' ') != std::string::npos) return out;
            context_len = int(input.size());
            for (const std::string& n : names)
                if (starts_with(n, input)) out.emplace_back(n);
            return out;
        });
        log_set_sink([this](const std::string& line) { rx_.print("%s\n", line.c_str()); });

        std::fputs("tn34xx monitor; 'help' lists commands, Ctrl-D or 'quit' exits.\n", stdout);
        std::fflush(stdout);
        while (!monitor_.quit_requested() && !emu_.quit_requested()) {
            errno = 0;
            const char* input = rx_.input(emu_.running() ? "tn34xx (running)> " : "tn34xx> ");
            if (!input) {
                if (errno == EAGAIN) continue;  // Ctrl-C at the prompt or interrupt()
                emu_.request_quit(int(emu_.exit_code()));
                break;
            }
            const std::string line = input;
            if (!trim(line).empty()) rx_.history_add(line);
            monitor_.execute(line);
        }
        log_set_sink(nullptr);
        if (!history_file_.empty()) rx_.history_save(history_file_);
    }

    void interrupt() override { rx_.emulate_key_press(replxx::Replxx::KEY::control('C')); }

private:
    Monitor& monitor_;
    Emulator& emu_;
    std::string history_file_;
    replxx::Replxx rx_;
};

} // namespace

std::unique_ptr<Console> make_terminal_console(Monitor& monitor, Emulator& emu, const std::string& history_file) {
    return std::make_unique<ReplxxConsole>(monitor, emu, history_file);
}

} // namespace tn34xx
