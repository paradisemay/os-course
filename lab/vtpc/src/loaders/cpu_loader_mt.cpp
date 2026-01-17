#include <exception>
#include <iostream>

#include "loaders/common.hpp"

int main(int argc, char* argv[]) {
  loaders::LoaderConfig config;
  try {
    const auto outcome = loaders::parse_arguments(
        argc, argv, /*allow_thread_override=*/true, config
    );
    if (outcome == loaders::ParseOutcome::kHelp) {
      loaders::print_usage(argv[0], /*allow_thread_override=*/true);
      return 0;
    }
    loaders::run_cpu_load(config);
  } catch (const std::exception& ex) {
    std::cerr << "cpu_loader_mt: " << ex.what() << std::endl;
    loaders::print_usage(argv[0], /*allow_thread_override=*/true);
    return 1;
  }

  return 0;
}
