#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <linux/sched.h>
#include <signal.h>
#include <sstream>
#include <string>
#include <utility>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <ctime>
#include <unistd.h>
#include <vector>

extern char **environ;


constexpr long long kNanosecondsInSecond = 1'000'000'000LL;

namespace {

bool IsWhitespaceOnly(const std::string &segment) {
    for (unsigned char ch : segment) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

bool ExecuteCommand(std::vector<std::string> tokens) {
    if (tokens.empty()) {
        return false;
    }

    if (tokens[0].find('/') == std::string::npos) {
        std::cerr << "minish: команда '" << tokens[0]
                  << "' должна содержать '/' (например, /bin/ls или ./ls)" << std::endl;
        return false;
    }

    int pipefd[2];
    if (pipe(pipefd) == -1) {
        std::cerr << "minish: не удалось создать канал: " << std::strerror(errno) << std::endl;
        return false;
    }

    struct timespec start_time {};
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) == -1) {
        std::cerr << "minish: не удалось получить время запуска: " << std::strerror(errno) << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    constexpr std::size_t kChildStackSize = 1024 * 1024;
    std::vector<char> child_stack(kChildStackSize);

    struct clone_args args {};
    args.flags = 0;
    args.exit_signal = SIGCHLD;
    args.stack = reinterpret_cast<__u64>(child_stack.data() + child_stack.size());
    args.stack_size = child_stack.size();

    long clone_res = syscall(SYS_clone3, &args, sizeof(args));
    if (clone_res == -1) {
        std::cerr << "minish: не удалось создать процесс через clone3: "
                  << std::strerror(errno) << std::endl;
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }

    if (clone_res == 0) {
        close(pipefd[0]);
        int flags = fcntl(pipefd[1], F_GETFD);
        if (flags != -1) {
            fcntl(pipefd[1], F_SETFD, flags | FD_CLOEXEC);
        }

        std::vector<char *> argv;
        argv.reserve(tokens.size() + 1);
        for (auto &part : tokens) {
            argv.push_back(const_cast<char *>(part.c_str()));
        }
        argv.push_back(nullptr);

        execve(argv[0], argv.data(), environ);

        int exec_err = errno;
        std::cerr << "minish: не удалось запустить '" << tokens[0] << "': "
                  << std::strerror(exec_err) << std::endl;
        ssize_t ignored = write(pipefd[1], &exec_err, sizeof(exec_err));
        (void)ignored;
        close(pipefd[1]);
        _exit(127);
    }

    close(pipefd[1]);

    bool exec_failed = false;
    int exec_err = 0;
    while (true) {
        ssize_t count = read(pipefd[0], &exec_err, sizeof(exec_err));
        if (count > 0) {
            exec_failed = true;
            break;
        }
        if (count == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        exec_failed = true;
        break;
    }
    close(pipefd[0]);

    int status = 0;
    pid_t waited = 0;
    do {
        waited = waitpid(static_cast<pid_t>(clone_res), &status, 0);
    } while (waited == -1 && errno == EINTR);

    if (waited == -1) {
        std::cerr << "minish: ошибка ожидания процесса: " << std::strerror(errno) << std::endl;
        return false;
    }

    struct timespec end_time {};
    if (clock_gettime(CLOCK_MONOTONIC, &end_time) == -1) {
        std::cerr << "minish: не удалось получить время завершения: " << std::strerror(errno) << std::endl;
        return false;
    }

    if (exec_failed) {
        return false;
    }

    long seconds = end_time.tv_sec - start_time.tv_sec;
    long nanoseconds = end_time.tv_nsec - start_time.tv_nsec;
    if (nanoseconds < 0) {
        --seconds;
        nanoseconds += kNanosecondsInSecond;
    }

    double elapsed = static_cast<double>(seconds) +
                      static_cast<double>(nanoseconds) / static_cast<double>(kNanosecondsInSecond);

    std::cout << std::fixed << std::setprecision(6) << "real=" << elapsed << " sec"
              << std::defaultfloat << std::endl;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return true;
    }

    if (WIFEXITED(status)) {
        return false;
    }

    if (WIFSIGNALED(status)) {
        return false;
    }

    return false;
}

}  // namespace

int main() {
    std::string line;

    while (true) {
        std::cout << "minish> " << std::flush;
        if (!std::getline(std::cin, line)) {
            if (std::cin.eof()) {
                std::cout << std::endl;
            }
            break;
        }

        std::vector<std::string> segments;
        size_t pos = 0;
        while (true) {
            size_t next = line.find("&&", pos);
            if (next == std::string::npos) {
                segments.push_back(line.substr(pos));
                break;
            }
            segments.push_back(line.substr(pos, next - pos));
            pos = next + 2;
        }

        bool all_whitespace = true;
        for (const auto &segment : segments) {
            if (!IsWhitespaceOnly(segment)) {
                all_whitespace = false;
                break;
            }
        }
        if (all_whitespace) {
            continue;
        }

        bool syntax_error = false;
        for (const auto &segment : segments) {
            if (IsWhitespaceOnly(segment)) {
                std::cerr << "minish: синтаксическая ошибка: пустая команда рядом с '&&'" << std::endl;
                syntax_error = true;
                break;
            }
        }
        if (syntax_error) {
            continue;
        }

        for (const auto &segment : segments) {
            std::istringstream iss(segment);
            std::vector<std::string> tokens;
            std::string token;
            while (iss >> token) {
                tokens.push_back(token);
            }

            if (tokens.empty()) {
                std::cerr << "minish: синтаксическая ошибка: пустая команда" << std::endl;
                break;
            }

            if (!ExecuteCommand(std::move(tokens))) {
                break;
            }
        }
    }

    return 0;
}
