#include <exception>
#include <iostream>

#include "loaders/linear_regression.hpp"

int main(int argc, char* argv[]) {
  loaders::RegressionConfig config{};
  try {
    const auto outcome =
        loaders::parse_regression_arguments(argc, argv, config);
    if (outcome == loaders::RegressionParseOutcome::kHelp) {
      loaders::print_regression_usage(argv[0]);
      return 0;
    }
    const auto stats = loaders::run_linear_regression(config);
    loaders::print_regression_summary(config, stats);
  } catch (const std::exception& ex) {
    std::cerr << "cpu_loader: " << ex.what() << std::endl;
    loaders::print_regression_usage(argv[0]);
    return 1;
  }

  return 0;
}
