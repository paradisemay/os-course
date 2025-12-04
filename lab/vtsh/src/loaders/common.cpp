#include "loaders/common.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace loaders {

namespace {

std::chrono::milliseconds parse_duration(std::string_view argument) {
    double seconds = 0.0;
    std::istringstream iss{std::string(argument)};
    iss >> seconds;
    if (!iss || seconds < 0.0) {
        throw std::invalid_argument("ожидалось неотрицательное значение секунд для --duration");
    }
    constexpr double kMillisInSecond = 1000.0;
    auto millis = std::llround(seconds * kMillisInSecond);
    return std::chrono::milliseconds{millis};
}

std::size_t thread_limit() {
    constexpr std::size_t kFallbackLimit = 8;
    unsigned int hw_threads = std::thread::hardware_concurrency();
    if (hw_threads == 0) {
        return kFallbackLimit;
    }
    return static_cast<std::size_t>(hw_threads) * 4;
}

std::size_t parse_threads(std::string_view argument) {
    long long threads = 0;
    std::istringstream iss{std::string(argument)};
    iss >> threads;
    if (!iss || threads <= 0) {
        throw std::invalid_argument("значение --threads должно быть положительным целым числом");
    }
    const auto max_threads = thread_limit();
    if (static_cast<std::size_t>(threads) > max_threads) {
        std::ostringstream message;
        message << "значение --threads должно быть в диапазоне от 1 до " << max_threads;
        throw std::invalid_argument(message.str());
    }
    return static_cast<std::size_t>(threads);
}

}  // namespace

ParseOutcome parse_arguments(int argc, char *argv[], bool allow_thread_override, LoaderConfig &config) {
    config = LoaderConfig{};
    if (allow_thread_override) {
        unsigned int hw_threads = std::thread::hardware_concurrency();
        config.threads = hw_threads == 0 ? 2U : static_cast<std::size_t>(hw_threads);
    }

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        if (argument == "--help" || argument == "-h") {
            return ParseOutcome::kHelp;
        }
        if (argument == "--duration") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("после --duration требуется значение в секундах");
            }
            config.duration = parse_duration(argv[++i]);
            continue;
        }
        if (argument == "--threads") {
            if (!allow_thread_override) {
                throw std::invalid_argument("--threads доступно только для многопоточной версии загрузчика");
            }
            if (i + 1 >= argc) {
                throw std::invalid_argument("после --threads требуется положительное целое число");
            }
            config.threads = parse_threads(argv[++i]);
            continue;
        }
        if (argument == "--verbose") {
            config.verbose = true;
            continue;
        }
        std::ostringstream message;
        message << "неизвестный аргумент '" << argument << "'. Используйте --help для справки";
        throw std::invalid_argument(message.str());
    }

    if (!allow_thread_override) {
        config.threads = 1;
    }

    return ParseOutcome::kSuccess;
}

void print_usage(const char *program_name, bool allow_thread_override) {
    std::cout << "Использование: " << program_name << " [параметры]\n"
              << "  --duration <секунды>  продолжительность нагрузки (по умолчанию 5 секунд)\n";
    if (allow_thread_override) {
        std::cout << "  --threads <число>    количество потоков (по умолчанию число потоков CPU, максимум "
                  << thread_limit() << ")\n";
    }
    std::cout << "  --verbose            печать дополнительной статистики\n"
              << "  --help               показать это сообщение\n";
}

namespace {

void burn_cpu_until(const LoaderConfig &config, std::chrono::steady_clock::time_point deadline,
                    std::atomic<std::uint64_t> &total_iterations, std::size_t index,
                    std::vector<std::uint64_t> &per_thread_iterations) {
    static_cast<void>(config);
    volatile double accumulator = 1.0;
    std::uint64_t local_iterations = 0;

    while (std::chrono::steady_clock::now() < deadline) {
        accumulator = std::sin(accumulator) + std::cos(accumulator);
        accumulator = accumulator <= 0.0 ? 1.0 : accumulator;
        ++local_iterations;
    }

    total_iterations.fetch_add(local_iterations, std::memory_order_relaxed);
    per_thread_iterations[index] = local_iterations;
}

}  // namespace

void run_cpu_load(const LoaderConfig &config) {
    if (config.threads == 0) {
        throw std::invalid_argument("количество потоков должно быть положительным");
    }

    const auto deadline = std::chrono::steady_clock::now() + config.duration;
    std::atomic<std::uint64_t> total_iterations{0};
    std::vector<std::thread> workers;
    workers.reserve(config.threads);
    std::vector<std::uint64_t> per_thread(config.threads, 0);

    auto launch_worker = [&](std::size_t index) {
        workers.emplace_back([&, index]() {
            burn_cpu_until(config, deadline, total_iterations, index, per_thread);
        });
    };

    for (std::size_t index = 0; index < config.threads; ++index) {
        launch_worker(index);
    }

    for (auto &worker : workers) {
        worker.join();
    }

    const auto elapsed = config.duration.count() / 1000.0;
    std::cout << std::fixed << std::setprecision(3)
              << "CPU load finished in ~" << elapsed << " с (" << config.threads
              << " поток(ов))" << std::endl;

    if (config.verbose) {
        std::cout << "Всего итераций: " << total_iterations.load(std::memory_order_relaxed) << std::endl;
        for (std::size_t index = 0; index < per_thread.size(); ++index) {
            std::cout << "  Поток " << index << ": " << per_thread[index] << " итераций" << std::endl;
        }
    }
}

}  // namespace loaders

