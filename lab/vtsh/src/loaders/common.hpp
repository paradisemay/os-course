#pragma once

#include <chrono>
#include <cstddef>

namespace loaders {

struct LoaderConfig {
    std::chrono::milliseconds duration{5000};
    std::size_t threads{1};
    bool verbose{false};
};

enum class ParseOutcome {
    kSuccess,
    kHelp
};

ParseOutcome parse_arguments(int argc, char *argv[], bool allow_thread_override, LoaderConfig &config);
void print_usage(const char *program_name, bool allow_thread_override);
void run_cpu_load(const LoaderConfig &config);

}  // namespace loaders

