#pragma once

#include <cstddef>

namespace loaders {

struct RegressionConfig {
    long repeats;
    std::size_t samples;
};

struct RegressionResult {
    double intercept;
    double slope;
    double mse;
    std::size_t samples;
};

struct RegressionRunStats {
    double warmup_duration;
    double estimated_total_duration;
    double actual_duration;
    RegressionResult last_result;
    std::size_t total_samples;
    double sink_value;
};

enum class RegressionParseOutcome {
    kSuccess,
    kHelp
};

RegressionParseOutcome parse_regression_arguments(int argc, char **argv, RegressionConfig &config);
void print_regression_usage(const char *program_name);
RegressionRunStats run_linear_regression(const RegressionConfig &config);
void print_regression_summary(const RegressionConfig &config, const RegressionRunStats &stats);

}  // namespace loaders

