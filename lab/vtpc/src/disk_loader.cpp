#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "vtpc.h"
}

namespace {
constexpr long kDefaultRepeats = 3;
constexpr std::size_t kDefaultFileSize = 16 * 1024 * 1024;  // 16 МиБ
constexpr std::size_t kDefaultBlockSize = 4096;

void PrintUsage(const char* program) {
  std::cout << "Использование: " << program
            << " [--repeats N] [--file-size BYTES] [--block-size BYTES]"
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

std::size_t ParseSizeT(const std::string& value, const std::string& name) {
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

struct Options {
  long repeats;
  std::size_t file_size;
  std::size_t block_size;
};

int my_open(const char* path, int flags, mode_t mode) {
#ifdef USE_VTPC
  return vtpc_open(path, flags, mode);
#else
  return open(path, flags, mode);
#endif
}

int my_close(int fd) {
#ifdef USE_VTPC
  return vtpc_close(fd);
#else
  return close(fd);
#endif
}

ssize_t my_read(int fd, void* buf, size_t count) {
#ifdef USE_VTPC
  return vtpc_read(fd, buf, count);
#else
  return read(fd, buf, count);
#endif
}

ssize_t my_write(int fd, const void* buf, size_t count) {
#ifdef USE_VTPC
  return vtpc_write(fd, buf, count);
#else
  return write(fd, buf, count);
#endif
}

off_t my_lseek(int fd, off_t offset, int whence) {
#ifdef USE_VTPC
  return vtpc_lseek(fd, offset, whence);
#else
  return lseek(fd, offset, whence);
#endif
}

int my_fsync(int fd) {
#ifdef USE_VTPC
  return vtpc_fsync(fd);
#else
  return fsync(fd);
#endif
}

class FileGuard {
public:
  FileGuard(int fd, std::string path) : fd_(fd), path_(std::move(path)) {
  }

  FileGuard(const FileGuard&) = delete;
  FileGuard& operator=(const FileGuard&) = delete;

  FileGuard(FileGuard&& other) noexcept
      : fd_(std::exchange(other.fd_, -1)), path_(std::move(other.path_)) {
  }

  FileGuard& operator=(FileGuard&& other) noexcept {
    if (this != &other) {
      Cleanup();
      fd_ = std::exchange(other.fd_, -1);
      path_ = std::move(other.path_);
    }
    return *this;
  }

  ~FileGuard() {
    Cleanup();
  }

  int fd() const {
    return fd_;
  }
  const std::string& path() const {
    return path_;
  }

private:
  void Cleanup() noexcept {
    if (fd_ != -1) {
      if (my_close(fd_) == -1) {
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

Options ParseOptions(int argc, char** argv) {
  Options options{
      .repeats = kDefaultRepeats,
      .file_size = kDefaultFileSize,
      .block_size = kDefaultBlockSize
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
    } else if (arg == "--file-size") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --file-size");
      }
      options.file_size = ParseSizeT(argv[++i], "--file-size");
    } else if (arg == "--block-size") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --block-size");
      }
      options.block_size = ParseSizeT(argv[++i], "--block-size");
    } else {
      throw std::invalid_argument("Неизвестный аргумент: " + arg);
    }
  }
  return options;
}

double RunDiskIteration(
    int fd, std::vector<char>& buffer, std::size_t file_size
) {
#ifndef USE_VTPC
  if (ftruncate(fd, 0) == -1) {
    throw std::runtime_error(std::string("ftruncate: ") + std::strerror(errno));
  }
#endif
  if (my_lseek(fd, 0, SEEK_SET) == -1) {
    throw std::runtime_error(std::string("lseek: ") + std::strerror(errno));
  }

  timespec start{};
  timespec end{};
  if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
    throw std::runtime_error(
        std::string("clock_gettime: ") + std::strerror(errno)
    );
  }

  std::size_t remaining = file_size;
  while (remaining > 0) {
    std::size_t chunk = std::min(buffer.size(), remaining);
    ssize_t written = my_write(fd, buffer.data(), chunk);
    if (written == -1) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("write: ") + std::strerror(errno));
    }
    remaining -= static_cast<std::size_t>(written);
  }

  if (my_fsync(fd) == -1) {
    throw std::runtime_error(std::string("fsync: ") + std::strerror(errno));
  }

  if (my_lseek(fd, 0, SEEK_SET) == -1) {
    throw std::runtime_error(std::string("lseek: ") + std::strerror(errno));
  }

  remaining = file_size;
  while (remaining > 0) {
    std::size_t chunk = std::min(buffer.size(), remaining);
    ssize_t received = my_read(fd, buffer.data(), chunk);
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
#ifndef USE_VTPC
  posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#endif
#endif

  if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
    throw std::runtime_error(
        std::string("clock_gettime: ") + std::strerror(errno)
    );
  }

  return TimespecToSeconds(start, end);
}

std::string DetermineMountPoint(const std::string& path) {
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
        std::string("/tmp/disk_loader_") + std::to_string(getpid()) + ".dat";
    std::vector<char> buffer(options.block_size, '\0');
    for (std::size_t i = 0; i < buffer.size(); ++i) {
      buffer[i] = static_cast<char>(i % 251);
    }

    int fd = my_open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd == -1) {
      throw std::runtime_error(std::string("open: ") + std::strerror(errno));
    }
    FileGuard file(fd, path);

    const std::string mount_info = DetermineMountPoint(file.path());
    std::cout << "Дисковый нагрузчик" << std::endl;
    std::cout << "Повторов: " << options.repeats
              << ", размер файла: " << FormatBytes(options.file_size)
              << ", блок: " << FormatBytes(options.block_size) << std::endl;
    std::cout << "Характеристика файловой системы: " << mount_info << std::endl;

    const double warmup =
        RunDiskIteration(file.fd(), buffer, options.file_size);
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
      RunDiskIteration(file.fd(), buffer, options.file_size);
    }

    if (clock_gettime(CLOCK_MONOTONIC, &end) == -1) {
      throw std::runtime_error(
          std::string("clock_gettime: ") + std::strerror(errno)
      );
    }

    const double elapsed = TimespecToSeconds(start, end);
    const unsigned long long bytes =
        static_cast<unsigned long long>(options.file_size) *
        static_cast<unsigned long long>(options.repeats) * 2ULL;
    const double throughput = bytes / elapsed / (1024.0 * 1024.0);

    std::cout << "Фактическая длительность: " << elapsed << " сек" << std::endl;
    std::cout << "Передано данных: " << FormatBytes(bytes) << std::endl;
    std::cout << std::setprecision(3)
              << "Средняя пропускная способность: " << throughput << " МиБ/с"
              << std::endl;
  } catch (const std::exception& ex) {
    std::cerr << "Ошибка: " << ex.what() << std::endl;
    return 1;
  }

  return 0;
}
