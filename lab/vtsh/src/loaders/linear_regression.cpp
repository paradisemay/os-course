#include "loaders/linear_regression.hpp"

#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace loaders {
namespace {

constexpr std::size_t kDefaultSampleSize = 200'000;
constexpr long kDefaultRepeats = 5;
constexpr double kTrueSlope = 2.5;
constexpr double kTrueIntercept = -1.0;
constexpr double kNoiseStdDev = 5.0;
volatile double g_cpu_sink = 0.0;

struct TimedRun {
    double duration;
    RegressionResult result;
};

void print_usage_internal(const char *program_name) {
    std::cout << "Использование: " << program_name
              << " [--repeats N] [--samples N]" << std::endl;
}

double timespec_to_seconds(const timespec &start, const timespec &end) {
    long seconds = end.tv_sec - start.tv_sec;
    long nanoseconds = end.tv_nsec - start.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += 1'000'000'000L;
    }
    return static_cast<double>(seconds) +
           static_cast<double>(nanoseconds) / 1'000'000'000.0;
}

RegressionResult run_cpu_workload(std::size_t sample_size) {
    static thread_local std::mt19937 rng(1337);
    std::uniform_real_distribution<double> x_distribution(-100.0, 100.0);
    std::normal_distribution<double> noise_distribution(0.0, kNoiseStdDev);

    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xx = 0.0;
    double sum_xy = 0.0;
    double sum_yy = 0.0;

    for (std::size_t i = 0; i < sample_size; ++i) {
        const double x = x_distribution(rng);
        const double noise = noise_distribution(rng);
        const double y = kTrueSlope * x + kTrueIntercept + noise;

        sum_x += x;
        sum_y += y;
        sum_xx += x * x;
        sum_xy += x * y;
        sum_yy += y * y;
    }

    RegressionResult result{};
    result.samples = sample_size;

    if (sample_size == 0) {
        g_cpu_sink = 0.0;
        return result;
    }

    const double n = static_cast<double>(sample_size);
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::fabs(denominator) < 1e-12) {
        result.slope = 0.0;
        result.intercept = sum_y / n;
    } else {
        result.slope = (n * sum_xy - sum_x * sum_y) / denominator;
        result.intercept = (sum_y - result.slope * sum_x) / n;
    }

    const double intercept = result.intercept;
    const double slope = result.slope;
    const double residual_sum_squares =
        sum_yy - 2.0 * intercept * sum_y - 2.0 * slope * sum_xy +
        2.0 * intercept * slope * sum_x + n * intercept * intercept +
        slope * slope * sum_xx;
    const double non_negative_sse = residual_sum_squares < 0.0 ? 0.0 : residual_sum_squares;
    result.mse = non_negative_sse / n;

    g_cpu_sink = intercept + slope + result.mse;
    return result;
}

TimedRun measure_single_iteration(std::size_t sample_size) {
    timespec start{};
    timespec end{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        throw std::runtime_error(std::string("clock_gettime: ") + std::strerror(errno));
    }
    RegressionResult result = run_cpu_workload(sample_size);
    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
        throw std::runtime_error(std::string("clock_gettime: ") + std::strerror(errno));
    }
    TimedRun timed_run{};
    timed_run.duration = timespec_to_seconds(start, end);
    timed_run.result = result;
    return timed_run;
}

}  // namespace

RegressionParseOutcome parse_regression_arguments(int argc, char **argv, RegressionConfig &config) {
    config.repeats = kDefaultRepeats;
    config.samples = kDefaultSampleSize;

    for (int i = 1; i < argc; ++i) {
        std::string argument(argv[i]);
        if (argument == "--help" || argument == "-h") {
            return RegressionParseOutcome::kHelp;
        }
        if (argument == "--repeats") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Отсутствует значение после --repeats");
            }
            try {
                long value = std::stol(argv[++i]);
                if (value <= 0) {
                    throw std::invalid_argument("Число повторов должно быть положительным");
                }
                config.repeats = value;
            } catch (const std::exception &ex) {
                throw std::invalid_argument(std::string("Неверное значение для --repeats: ") + ex.what());
            }
            continue;
        }
        if (argument == "--samples") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("Отсутствует значение после --samples");
            }
            try {
                long long value = std::stoll(argv[++i]);
                if (value <= 0) {
                    throw std::invalid_argument("Размер выборки должен быть положительным");
                }
                if (static_cast<unsigned long long>(value) >
                    std::numeric_limits<std::size_t>::max()) {
                    throw std::invalid_argument("Размер выборки превышает допустимый предел");
                }
                config.samples = static_cast<std::size_t>(value);
            } catch (const std::exception &ex) {
                throw std::invalid_argument(std::string("Неверное значение для --samples: ") + ex.what());
            }
            continue;
        }
        throw std::invalid_argument("Неизвестный аргумент: " + argument);
    }

    return RegressionParseOutcome::kSuccess;
}

void print_regression_usage(const char *program_name) {
    print_usage_internal(program_name);
}

RegressionRunStats run_linear_regression(const RegressionConfig &config) {
    if (config.repeats <= 0) {
        throw std::invalid_argument("Число повторов должно быть положительным");
    }

    const TimedRun warmup = measure_single_iteration(config.samples);

    timespec start{};
    timespec end{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        throw std::runtime_error(std::string("clock_gettime: ") + std::strerror(errno));
    }

    RegressionResult last_result{};
    for (long i = 0; i < config.repeats; ++i) {
        last_result = run_cpu_workload(config.samples);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
        throw std::runtime_error(std::string("clock_gettime: ") + std::strerror(errno));
    }

    RegressionRunStats stats{};
    stats.warmup_duration = warmup.duration;
    stats.estimated_total_duration = warmup.duration * static_cast<double>(config.repeats);
    stats.actual_duration = timespec_to_seconds(start, end);
    stats.last_result = last_result;
    stats.total_samples = config.samples * static_cast<std::size_t>(config.repeats);
    stats.sink_value = g_cpu_sink;
    return stats;
}

void print_regression_summary(const RegressionConfig &config, const RegressionRunStats &stats) {
    std::cout << "CPU-нагрузчик" << std::endl;
    std::cout << "Повторов: " << config.repeats << ", точек на повтор: " << config.samples
              << std::endl;
    std::cout << std::fixed << std::setprecision(6)
              << "Оценка времени выполнения: ~" << stats.estimated_total_duration << " сек"
              << std::endl;
    std::cout << "Фактическая длительность: " << stats.actual_duration << " сек" << std::endl;
    std::cout << "Совокупное число точек: " << stats.total_samples << std::endl;
    std::cout << std::setprecision(6)
              << "Итоговый наклон: " << stats.last_result.slope
              << ", свободный член: " << stats.last_result.intercept << std::endl;
    std::cout << "Среднеквадратичная ошибка: " << stats.last_result.mse << std::endl;
    std::cout << "Контрольное значение: " << stats.sink_value << std::endl;
}

}  // namespace loaders

