#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr char kMagicValue[] = "EMAGRPH";
constexpr std::uint32_t kFormatVersion = 1;

enum class EdgeDirection : std::uint8_t {
  Bidirectional = 0,
  Outgoing = 1,
  Incoming = 2,
};

struct alignas(8) GraphHeader {
  char magic[8];
  std::uint32_t version;
  std::uint32_t node_count;
  std::uint32_t degree;
  std::uint32_t node_record_size;
  std::uint32_t edge_record_size;
  std::uint32_t reserved;
  std::uint64_t adjacency_region_offset;
};

struct alignas(8) NodeRecord {
  std::int64_t value;
  std::uint32_t id;
  std::uint32_t neighbor_count;
  std::uint64_t adjacency_offset;
};

struct alignas(8) EdgeRecord {
  std::uint32_t target_id;
  std::uint8_t direction;
  std::uint8_t reserved[3];
};

static_assert(std::is_trivially_copyable_v<GraphHeader>);
static_assert(std::is_trivially_copyable_v<NodeRecord>);
static_assert(std::is_trivially_copyable_v<EdgeRecord>);

struct Options {
  std::string file_path = "graph.bin";
  std::uint32_t node_count = 128;
  std::uint32_t degree = 4;
  double direction_probability = 0.5;
  std::int64_t target_value = 42;
  std::uint32_t max_depth = 8;
  std::uint32_t start_node = 0;
  std::uint64_t seed = 5489u;
};

struct Stats {
  double generation_seconds = 0.0;
  double traversal_seconds = 0.0;
  std::uint64_t generation_operations = 0;
  std::uint64_t traversal_operations = 0;
  bool modification_success = false;
};

[[noreturn]] void PrintUsageAndExit(const char* program) {
  std::cerr << "Использование: " << program
            << " [--file PATH] [--nodes N] [--degree K] [--direction-prob P]"
               " [--target VALUE] [--depth D] [--start NODE] [--seed S]"
            << std::endl;
  std::exit(1);
}

std::uint32_t ParseUnsigned(const std::string& text, const std::string& name) {
  try {
    unsigned long long value = std::stoull(text);
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::out_of_range(name + " превышает допустимый диапазон");
    }
    return static_cast<std::uint32_t>(value);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        "Неверное значение для " + name + ": " + ex.what()
    );
  }
}

std::uint64_t ParseUnsigned64(
    const std::string& text, const std::string& name
) {
  try {
    unsigned long long value = std::stoull(text);
    return static_cast<std::uint64_t>(value);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        "Неверное значение для " + name + ": " + ex.what()
    );
  }
}

std::int64_t ParseSigned(const std::string& text, const std::string& name) {
  try {
    long long value = std::stoll(text);
    return static_cast<std::int64_t>(value);
  } catch (const std::exception& ex) {
    throw std::invalid_argument(
        "Неверное значение для " + name + ": " + ex.what()
    );
  }
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      PrintUsageAndExit(argv[0]);
    } else if (arg == "--file") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --file");
      }
      options.file_path = argv[++i];
    } else if (arg == "--nodes") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --nodes");
      }
      options.node_count = ParseUnsigned(argv[++i], "--nodes");
    } else if (arg == "--degree") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --degree");
      }
      options.degree = ParseUnsigned(argv[++i], "--degree");
    } else if (arg == "--direction-prob") {
      if (i + 1 >= argc) {
        throw std::invalid_argument(
            "Отсутствует значение после --direction-prob"
        );
      }
      std::string value = argv[++i];
      try {
        options.direction_probability = std::stod(value);
      } catch (const std::exception& ex) {
        throw std::invalid_argument(
            "Неверное значение для --direction-prob: " + std::string(ex.what())
        );
      }
    } else if (arg == "--target") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --target");
      }
      options.target_value = ParseSigned(argv[++i], "--target");
    } else if (arg == "--depth") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --depth");
      }
      options.max_depth = ParseUnsigned(argv[++i], "--depth");
    } else if (arg == "--start") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --start");
      }
      options.start_node = ParseUnsigned(argv[++i], "--start");
    } else if (arg == "--seed") {
      if (i + 1 >= argc) {
        throw std::invalid_argument("Отсутствует значение после --seed");
      }
      options.seed = ParseUnsigned64(argv[++i], "--seed");
    } else {
      throw std::invalid_argument("Неизвестный аргумент: " + arg);
    }
  }

  if (options.node_count == 0) {
    throw std::invalid_argument("Количество вершин должно быть положительным");
  }
  if (options.degree == 0) {
    throw std::invalid_argument("Степень графа должна быть положительной");
  }
  if (options.degree >= options.node_count) {
    throw std::invalid_argument(
        "Для k-регулярного графа требуется k < количество вершин"
    );
  }
  if ((static_cast<std::uint64_t>(options.node_count) * options.degree) % 2 !=
      0) {
    throw std::invalid_argument(
        "Произведение количества вершин и степени должно быть чётным"
    );
  }
  if (options.direction_probability < 0.0 ||
      options.direction_probability > 1.0) {
    throw std::invalid_argument(
        "Вероятность направления должна находиться в диапазоне [0, 1]"
    );
  }
  if (options.start_node >= options.node_count) {
    throw std::invalid_argument(
        "Начальная вершина должна существовать в графе"
    );
  }
  return options;
}

struct FileHandle {
  int fd = -1;
  std::uint64_t operations = 0;

  explicit FileHandle(const std::string& path) {
    fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd == -1) {
      throw std::system_error(
          errno, std::generic_category(), "Не удалось открыть файл"
      );
    }
  }

  ~FileHandle() {
    if (fd != -1) {
      ::close(fd);
    }
  }

  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  void Write(const void* data, std::size_t size, std::uint64_t offset) {
    const char* ptr = static_cast<const char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
      ssize_t written =
          ::pwrite(fd, ptr, remaining, static_cast<off_t>(offset));
      ++operations;
      if (written == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(
            errno, std::generic_category(), "Ошибка записи pwrite"
        );
      }
      remaining -= static_cast<std::size_t>(written);
      offset += static_cast<std::uint64_t>(written);
      ptr += written;
    }
  }

  void Read(void* data, std::size_t size, std::uint64_t offset) {
    char* ptr = static_cast<char*>(data);
    std::size_t remaining = size;
    while (remaining > 0) {
      ssize_t received =
          ::pread(fd, ptr, remaining, static_cast<off_t>(offset));
      ++operations;
      if (received == 0) {
        throw std::runtime_error("Неожиданный конец файла при чтении");
      }
      if (received == -1) {
        if (errno == EINTR) {
          continue;
        }
        throw std::system_error(
            errno, std::generic_category(), "Ошибка чтения pread"
        );
      }
      remaining -= static_cast<std::size_t>(received);
      offset += static_cast<std::uint64_t>(received);
      ptr += received;
    }
  }

  void Sync() {
    if (::fsync(fd) == -1) {
      throw std::system_error(errno, std::generic_category(), "Ошибка fsync");
    }
  }
};

struct GraphData {
  GraphHeader header{};
  std::vector<NodeRecord> nodes;
  std::vector<EdgeRecord> edges;
};

GraphData GenerateGraph(const Options& options) {
  std::mt19937_64 rng(options.seed);
  std::vector<std::vector<std::uint32_t>> adjacency(options.node_count);

  const std::uint64_t stub_count =
      static_cast<std::uint64_t>(options.node_count) *
      static_cast<std::uint64_t>(options.degree);
  if (stub_count > std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument(
        "Запрошено слишком много рёбер: nodes * degree превышает максимально "
        "поддерживаемый размер контейнера. Уменьшите значения --nodes и "
        "--degree, "
        "чтобы их произведение не превышало " +
        std::to_string(std::numeric_limits<std::size_t>::max())
    );
  }

  std::vector<std::uint32_t> stubs;
  stubs.reserve(static_cast<std::size_t>(stub_count));
  for (std::uint32_t node = 0; node < options.node_count; ++node) {
    adjacency[node].reserve(options.degree);
    for (std::uint32_t k = 0; k < options.degree; ++k) {
      stubs.push_back(node);
    }
  }

  const std::size_t total_stubs = stubs.size();
  constexpr std::size_t kMaxAttempts = 512;
  bool generated = false;
  for (std::size_t attempt = 0; attempt < kMaxAttempts && !generated;
       ++attempt) {
    std::shuffle(stubs.begin(), stubs.end(), rng);
    for (auto& list : adjacency) {
      list.clear();
    }
    generated = true;
    for (std::size_t i = 0; i < total_stubs; i += 2) {
      std::uint32_t u = stubs[i];
      std::uint32_t v = stubs[i + 1];
      if (u == v || std::find(adjacency[u].begin(), adjacency[u].end(), v) !=
                        adjacency[u].end()) {
        bool swapped = false;
        for (std::size_t j = i + 2; j < total_stubs; ++j) {
          std::uint32_t candidate = stubs[j];
          if (candidate == u) {
            continue;
          }
          if (std::find(adjacency[u].begin(), adjacency[u].end(), candidate) !=
              adjacency[u].end()) {
            continue;
          }
          std::swap(stubs[i + 1], stubs[j]);
          v = stubs[i + 1];
          swapped = true;
          break;
        }
        if (!swapped) {
          generated = false;
          break;
        }
      }
      if (u == v || std::find(adjacency[u].begin(), adjacency[u].end(), v) !=
                        adjacency[u].end()) {
        generated = false;
        break;
      }
      adjacency[u].push_back(v);
      adjacency[v].push_back(u);
    }
  }

  if (!generated) {
    throw std::runtime_error(
        "Не удалось сгенерировать k-регулярный граф без петель и кратных рёбер"
    );
  }

  std::vector<std::vector<EdgeDirection>> directions(options.node_count);
  for (std::uint32_t node = 0; node < options.node_count; ++node) {
    directions[node].resize(
        adjacency[node].size(), EdgeDirection::Bidirectional
    );
  }

  std::uniform_real_distribution<double> probability_dist(0.0, 1.0);
  std::uniform_int_distribution<int> orientation_flip(0, 1);

  for (std::uint32_t u = 0; u < options.node_count; ++u) {
    for (std::size_t idx = 0; idx < adjacency[u].size(); ++idx) {
      std::uint32_t v = adjacency[u][idx];
      if (u < v) {
        double roll = probability_dist(rng);
        if (roll < options.direction_probability) {
          bool forward = orientation_flip(rng) != 0;
          EdgeDirection dir_u =
              forward ? EdgeDirection::Outgoing : EdgeDirection::Incoming;
          EdgeDirection dir_v =
              forward ? EdgeDirection::Incoming : EdgeDirection::Outgoing;
          auto it_v = std::find(adjacency[v].begin(), adjacency[v].end(), u);
          std::size_t idx_v = static_cast<std::size_t>(
              std::distance(adjacency[v].begin(), it_v)
          );
          directions[u][idx] = dir_u;
          directions[v][idx_v] = dir_v;
        } else {
          auto it_v = std::find(adjacency[v].begin(), adjacency[v].end(), u);
          std::size_t idx_v = static_cast<std::size_t>(
              std::distance(adjacency[v].begin(), it_v)
          );
          directions[u][idx] = EdgeDirection::Bidirectional;
          directions[v][idx_v] = EdgeDirection::Bidirectional;
        }
      }
    }
  }

  GraphData data;
  data.nodes.resize(options.node_count);
  data.edges.reserve(static_cast<std::size_t>(stub_count));

  const std::uint64_t nodes_bytes =
      static_cast<std::uint64_t>(options.node_count) *
      static_cast<std::uint64_t>(sizeof(NodeRecord));
  const std::uint64_t base_offset = sizeof(GraphHeader) + nodes_bytes;

  std::uint64_t edges_bytes = 0;
  if (stub_count > 0) {
    if (stub_count >
        std::numeric_limits<std::uint64_t>::max() / sizeof(EdgeRecord)) {
      throw std::invalid_argument(
          "Переполнение при расчёте смещений рёбер. Уменьшите значения --nodes "
          "и --degree, "
          "чтобы произведение nodes * degree оставалось в пределах " +
          std::to_string(
              std::numeric_limits<std::uint64_t>::max() / sizeof(EdgeRecord)
          )
      );
    }
    edges_bytes = stub_count * sizeof(EdgeRecord);
  }

  if (base_offset > std::numeric_limits<std::uint64_t>::max() - edges_bytes) {
    throw std::invalid_argument(
        "Суммарный размер таблиц превышает доступный диапазон смещений. "
        "Уменьшите "
        "--nodes или --degree, чтобы произведение nodes * degree не "
        "превышало " +
        std::to_string(
            std::numeric_limits<std::uint64_t>::max() / sizeof(EdgeRecord)
        )
    );
  }
  std::uint64_t current_offset = base_offset;

  for (std::uint32_t node = 0; node < options.node_count; ++node) {
    NodeRecord record{};
    record.id = node;
    record.neighbor_count = static_cast<std::uint32_t>(adjacency[node].size());
    record.adjacency_offset = current_offset;
    record.value = static_cast<std::int64_t>(node);
    data.nodes[node] = record;
    current_offset +=
        static_cast<std::uint64_t>(adjacency[node].size()) * sizeof(EdgeRecord);
    for (std::size_t idx = 0; idx < adjacency[node].size(); ++idx) {
      EdgeRecord edge{};
      edge.target_id = adjacency[node][idx];
      edge.direction = static_cast<std::uint8_t>(directions[node][idx]);
      data.edges.push_back(edge);
    }
  }

  std::uniform_int_distribution<std::uint32_t> target_node_dist(
      0, options.node_count - 1
  );
  data.nodes[target_node_dist(rng)].value = options.target_value;

  std::strncpy(data.header.magic, kMagicValue, sizeof(data.header.magic));
  data.header.version = kFormatVersion;
  data.header.node_count = options.node_count;
  data.header.degree = options.degree;
  data.header.node_record_size = sizeof(NodeRecord);
  data.header.edge_record_size = sizeof(EdgeRecord);
  data.header.reserved = 0;
  data.header.adjacency_region_offset = base_offset;

  return data;
}

double DurationSeconds(const timespec& start, const timespec& end) {
  long seconds = end.tv_sec - start.tv_sec;
  long nanoseconds = end.tv_nsec - start.tv_nsec;
  if (nanoseconds < 0) {
    --seconds;
    nanoseconds += 1'000'000'000L;
  }
  return static_cast<double>(seconds) +
         static_cast<double>(nanoseconds) / 1'000'000'000.0;
}

void WriteGraph(FileHandle& file, const GraphData& data) {
  file.Write(&data.header, sizeof(GraphHeader), 0);
  const std::uint64_t nodes_offset = sizeof(GraphHeader);
  file.Write(
      data.nodes.data(), data.nodes.size() * sizeof(NodeRecord), nodes_offset
  );
  file.Write(
      data.edges.data(),
      data.edges.size() * sizeof(EdgeRecord),
      data.header.adjacency_region_offset
  );
  file.Sync();
}

GraphHeader ReadHeader(FileHandle& file) {
  GraphHeader header{};
  file.Read(&header, sizeof(GraphHeader), 0);
  if (std::strncmp(header.magic, kMagicValue, sizeof(header.magic)) != 0) {
    throw std::runtime_error("Формат файла не поддерживается");
  }
  if (header.version != kFormatVersion) {
    throw std::runtime_error("Неподдерживаемая версия формата файла");
  }
  if (header.node_record_size != sizeof(NodeRecord) ||
      header.edge_record_size != sizeof(EdgeRecord)) {
    throw std::runtime_error("Размеры структур не совпадают с текущей сборкой");
  }
  return header;
}

NodeRecord ReadNode(
    FileHandle& file, const GraphHeader& header, std::uint32_t node_id
) {
  std::uint64_t offset =
      sizeof(GraphHeader) +
      static_cast<std::uint64_t>(node_id) * header.node_record_size;
  NodeRecord record{};
  file.Read(&record, sizeof(NodeRecord), offset);
  return record;
}

std::vector<EdgeRecord> ReadEdges(FileHandle& file, const NodeRecord& node) {
  std::vector<EdgeRecord> edges(node.neighbor_count);
  if (!edges.empty()) {
    file.Read(
        edges.data(), edges.size() * sizeof(EdgeRecord), node.adjacency_offset
    );
  }
  return edges;
}

bool TraverseAndModify(
    FileHandle& file,
    const Options& options,
    const GraphHeader& header,
    Stats& stats
) {
  std::vector<bool> visited(header.node_count, false);
  std::queue<std::pair<std::uint32_t, std::uint32_t>> bfs;
  bfs.push({options.start_node, 0});
  visited[options.start_node] = true;

  while (!bfs.empty()) {
    auto [node_id, depth] = bfs.front();
    bfs.pop();

    NodeRecord node = ReadNode(file, header, node_id);
    if (node.value == options.target_value) {
      node.value = options.target_value + 1;
      file.Write(
          &node,
          sizeof(NodeRecord),
          sizeof(GraphHeader) +
              static_cast<std::uint64_t>(node_id) * header.node_record_size
      );
      stats.modification_success = true;
      return true;
    }

    if (depth >= options.max_depth) {
      continue;
    }

    std::vector<EdgeRecord> edges = ReadEdges(file, node);
    for (const EdgeRecord& edge : edges) {
      EdgeDirection direction = static_cast<EdgeDirection>(edge.direction);
      if (direction == EdgeDirection::Incoming) {
        continue;
      }
      if (edge.target_id >= header.node_count) {
        continue;
      }
      if (!visited[edge.target_id]) {
        visited[edge.target_id] = true;
        bfs.push({edge.target_id, depth + 1});
      }
    }
  }
  return false;
}

Stats Run(const Options& options) {
  Stats stats;

  timespec gen_start{};
  timespec gen_end{};
  if (clock_gettime(CLOCK_MONOTONIC, &gen_start) == -1) {
    throw std::system_error(
        errno, std::generic_category(), "clock_gettime (start generation)"
    );
  }

  GraphData data = GenerateGraph(options);

  FileHandle file(options.file_path);
  WriteGraph(file, data);

  if (clock_gettime(CLOCK_MONOTONIC, &gen_end) == -1) {
    throw std::system_error(
        errno, std::generic_category(), "clock_gettime (end generation)"
    );
  }

  stats.generation_seconds = DurationSeconds(gen_start, gen_end);
  stats.generation_operations = file.operations;

  GraphHeader header = ReadHeader(file);
  std::uint64_t operations_before_traversal = file.operations;

  timespec trav_start{};
  timespec trav_end{};
  if (clock_gettime(CLOCK_MONOTONIC, &trav_start) == -1) {
    throw std::system_error(
        errno, std::generic_category(), "clock_gettime (start traversal)"
    );
  }

  TraverseAndModify(file, options, header, stats);

  if (clock_gettime(CLOCK_MONOTONIC, &trav_end) == -1) {
    throw std::system_error(
        errno, std::generic_category(), "clock_gettime (end traversal)"
    );
  }

  stats.traversal_seconds = DurationSeconds(trav_start, trav_end);
  stats.traversal_operations = file.operations - operations_before_traversal;

  return stats;
}

void PrintReport(const Options& options, const Stats& stats) {
  std::cout << std::fixed << std::setprecision(6);
  std::cout << "Параметры генерации:\n";
  std::cout << "  файл: " << options.file_path << '\n';
  std::cout << "  вершины: " << options.node_count << '\n';
  std::cout << "  степень: " << options.degree << '\n';
  std::cout << "  вероятность направления: " << options.direction_probability
            << '\n';
  std::cout << "  целевое значение: " << options.target_value << '\n';
  std::cout << "  глубина поиска: " << options.max_depth << '\n';
  std::cout << "  стартовая вершина: " << options.start_node << '\n';
  std::cout << "  seed: " << options.seed << '\n';
  std::cout << '\n';
  std::cout << "Результаты:\n";
  std::cout << "  время генерации: " << stats.generation_seconds << " с\n";
  std::cout << "  время обхода: " << stats.traversal_seconds << " с\n";
  std::cout << "  обращения при генерации: " << stats.generation_operations
            << '\n';
  std::cout << "  обращения при обходе: " << stats.traversal_operations << '\n';
  std::cout << "  модификация выполнена: "
            << (stats.modification_success ? "да" : "нет") << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options options = ParseOptions(argc, argv);
    Stats stats = Run(options);
    PrintReport(options, stats);
    return stats.modification_success ? 0 : 1;
  } catch (const std::exception& ex) {
    std::cerr << "Ошибка: " << ex.what() << std::endl;
    return 1;
  }
}
