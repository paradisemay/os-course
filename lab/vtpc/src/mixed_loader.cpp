#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr long kDefaultRepeats = 4;
constexpr std::size_t kCpuOperationsPerRepeat = 1'000'000;
constexpr std::size_t kDiskBytesPerRepeat = 8 * 1024 * 1024;
constexpr std::size_t kDefaultBlockSize = 4096;

volatile double g_mixed_sink = 0.0;

class FileGuard {
public:
  FileGuard(int fd, std::string path) : fd_(fd), path_(std::move(path)) {
  }
  FileGuard(const FileGuard&) = delete;
  FileGuard& operator=(const FileGuard&) = delete;

  FileGuard(FileGuard&& other) noexcept
      : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
    other.path_.clear();
  }

  FileGuard& operator=(FileGuard&& other) noexcept {
    if (this != &other) {
      Cleanup();
      fd_ = other.fd_;
      path_ = std::move(other.path_);
      other.fd_ = -1;
      other.path_.clear();
    }
    return *this;
  }

  ~FileGuard() {
    Cleanup();
  }

  int fd() const {
    return fd_;
  }

private:
  void Cleanup() {
    if (fd_ != -1) {
      if (close(fd_) == -1) {
        std::cerr << "Предупреждение: close: " << std::strerror(errno)
                  << std::endl;
      }
      fd_ = -1;
    }
    if (!path_.empty()) {
      if (unlink(path_.c_str()) == -1) {
        std::cerr << "Предупреждение: unlink: " << std::strerror(errno)
                  << std::endl;
      }
      path_.clear();
    }
  }

  int fd_;
  std::string path_;
};

void PrintUsage(const char* program) {
  std::cout << "Использование: " << program
            << " [--repeats N] [--cpu-ops COUNT] [--disk-size BYTES]"
            << std::endl;
}

double TimespecToSeconds(const timespec& start, const timespec& end) {
  long seconds = end.tv_sec - start.tv_sec;
  long nanoseconds = end.tv_nsec - start.tv_nsec;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1'000'000'000L;
  }
  return static_cast<double>(seconds) +
         static_cast<double>(nanoseconds) / 1'000'000'000.0;
}

std::string FormatBytes(unsigned long long bytes) {
  const char* suffixes[] = {"Б", "КиБ", "МиБ", "ГиБ"};
  double value = static_cast<double>(bytes);
  int suffix_index = 0;
  while (value >= 1024.0 && suffix_index < 3) {
    value /= 1024.0;
    ++suffix_index;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << value << ' '
      << suffixes[suffix_index];
  return oss.str();
}

long ParseLong(const std::string& value, const std::string& name) {
  try {
    long result = std::stol(value);
    if (result <= 0) {
      throw std::invalid_argument(name + " должно быть положительным");
    }
    return result;
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        "Неверное значение для " + name + ": " + ex.what()
    );
  }
}

std::size_t ParseSize(const std::string& value, const std::string& name) {
  try {
    long long parsed = std::stoll(value);
    if (parsed <= 0) {
      throw std::invalid_argument(name + " должно быть положительным");
    }
    return static_cast<std::size_t>(parsed);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        "Неверное значение для " + name + ": " + ex.what()
    );
  }
}

void RunCpuWork(std::size_t operations) {
  volatile double acc = 0.5;
  for (std::size_t i = 0; i < operations; ++i) {
    acc = std::sin(acc + static_cast<double>((i % 991) + 1) * 0.001) * 0.99991 +
          1.0;
    if (acc > 10.0) {
      acc -= 9.0;
    }
  }
  g_mixed_sink = acc;
}

void RunDiskWork(int fd, std::vector<char>& buffer, std::size_t bytes) {
  if (ftruncate(fd, 0) == -1) {
    throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
  }
  if (lseek(fd, 0, SEEK_SET) == -1) {
    throw std::runtime_error(std::string("lseek: ") + std::strerror(errno));
  }

  std::size_t remaining = bytes;
  while (remaining > 0) {
    std::size_t chunk = std::min(buffer.size(), remaining);
    ssize_t written = write(fd, buffer.data(), chunk);
    if (written == -1) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write: ") + std::strerror(errno));
    }
    remaining -= static_cast<std::size_t>(written);
  }

  if (fsync(fd) == -1) {
    throw std::runtime_error(std::string("fsync: ") + std::strerror(errno));
  }

  if (lseek(fd, 0, SEEK_SET) == -1) {
    throw std::runtime_error(std::string("lseek: ") + std::strerror(errno));
  }

  remaining = bytes;
  while (remaining > 0) {
    std::size_t chunk = std::min(buffer.size(), remaining);
    ssize_t received = read(fd, buffer.data(), chunk);
    if (received == 0) {
      throw std::runtime_error("Неожиданный конец файла при чтении");
    }
    if (received == -1) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("read: ") + std::strerror(errno));
    }
    remaining -= static_cast<std::size_t>(received);
  }

#ifdef POSIX_FADV_DONTNEED
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
}

double MeasureIteration(
    int fd,
    std::vector<char>& buffer,
    std::size_t cpu_ops,
    std::size_t disk_bytes
) {
  timespec start{};
  timespec end{};
  if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
    throw std::runtime_error(
        std::string("clock_gettime: ") + std::strerror(errno)
    );
  }
  RunCpuWork(cpu_ops);
  RunDiskWork(fd, buffer, disk_bytes);
  if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
    throw std::runtime_error(
        std::string("clock_gettime: ") + std::strerror(errno)
    );
  }
  return TimespecToSeconds(start, end);
}

struct Options {
  long repeats;
  std::size_t cpu_ops;
  std::size_t disk_bytes;
};

Options ParseOptions(int argc, char** argv) {
  Options options{
      .repeats = kDefaultRepeats,
      .cpu_ops = kCpuOperationsPerRepeat,
      .disk_bytes = kDiskBytesPerRepeat
  };
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsage(argv[0]);
      std::exit(0);
    } else if (arg == "--repeats") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --repeats");
      }
      options.repeats = ParseLong(argv[++i], "--repeats");
    } else if (arg == "--cpu-ops") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --cpu-ops");
      }
      options.cpu_ops = ParseSize(argv[++i], "--cpu-ops");
    } else if (arg == "--disk-size") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --disk-size");
      }
      options.disk_bytes = ParseSize(argv[++i], "--disk-size");
    } else {
      throw std::invalid_argument("Неизвестный аргумент: " + arg);
    }
  }
  return options;
}

std::string DescribeFilesystem(const std::string& path) {
  struct statvfs info{};
  if (statvfs(path.c_str(), &info) == -1) {
    return "неизвестно";
  }
  std::ostringstream oss;
  oss << "размер блока файловой системы " << info.f_bsize << " Б";
  return oss.str();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options = ParseOptions(argc, argv);

    std::string path =
        std::string("/tmp/mixed_loader_") + std::to_string(getpid()) + ".dat";
    std::vector<char> buffer(kDefaultBlockSize, '\1');

    int fd = open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd == -1) {
      throw std::runtime_error(std::string("open: ") + std::strerror(errno));
    }

    FileGuard file_guard(fd, path);

    const std::string fs_info = DescribeFilesystem(path);
    std::cout << "Смешанный нагрузчик" << std::endl;
    std::cout << "Повторов: " << options.repeats
              << ", CPU-операций/повтор: " << options.cpu_ops
              << ", дисковых байт/повтор: " << FormatBytes(options.disk_bytes)
              << std::endl;
    std::cout << "Файловая система: " << fs_info << std::endl;

    const double warmup = MeasureIteration(
        file_guard.fd(), buffer, options.cpu_ops, options.disk_bytes
    );
    std::cout << std::fixed << std::setprecision(6)
              << "Оценка времени выполнения: ~" << warmup * options.repeats
              << " сек" << std::endl;

    timespec start{};
    timespec end{};
    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
      throw std::runtime_error(
          std::string("clock_gettime: ") + std::strerror(errno)
      );
    }

    for (long i = 0; i < options.repeats; ++i) {
      RunCpuWork(options.cpu_ops);
      RunDiskWork(file_guard.fd(), buffer, options.disk_bytes);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
      throw std::runtime_error(
          std::string("clock_gettime: ") + std::strerror(errno)
      );
    }

    const double elapsed = TimespecToSeconds(start, end);
    const unsigned long long total_cpu_ops =
        static_cast<unsigned long long>(options.cpu_ops) *
        static_cast<unsigned long long>(options.repeats);
    const unsigned long long total_disk_bytes =
        static_cast<unsigned long long>(options.disk_bytes) *
        static_cast<unsigned long long>(options.repeats);

    std::cout << "Фактическая длительность: " << elapsed << " сек" << std::endl;
    std::cout << "CPU операций суммарно: " << total_cpu_ops << std::endl;
    std::cout << "Дисковые данные суммарно: " << FormatBytes(total_disk_bytes)
              << std::endl;
    std::cout << std::setprecision(3)
              << "Средняя доля CPU-операций: " << total_cpu_ops / elapsed
              << " оп/с" << std::endl;
    std::cout << std::setprecision(3) << "Средний дисковый поток: "
              << (total_disk_bytes / elapsed / (1024.0 * 1024.0)) << " МиБ/с"
              << std::endl;
    std::cout << "Контрольное значение: " << g_mixed_sink << std::endl;
  } catch (const std::exception& ex) {
    std::cerr << "Ошибка: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
