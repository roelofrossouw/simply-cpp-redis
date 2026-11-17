#pragma once

// A throwaway Valkey or Redis server for the tests: started on a free localhost port with a random
// password, no persistence, in its own temporary folder, and stopped when the object goes. The
// binary is $SC_REDIS_TEST_SERVER, else the first valkey-server or redis-server on the PATH.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace sc_redis_test {
    inline std::optional<std::filesystem::path> server_binary() {
        if (const auto *given = std::getenv("SC_REDIS_TEST_SERVER"); given && *given) return std::filesystem::path{given};
        const auto *path = std::getenv("PATH");
        if (!path) return std::nullopt;
        for (const auto *name: {"valkey-server", "redis-server"}) {
            std::stringstream folders{path};
            std::string folder;
            while (std::getline(folders, folder, ':')) {
                if (folder.empty()) continue;
                const auto candidate = std::filesystem::path{folder} / name;
                if (access(candidate.c_str(), X_OK) == 0) return candidate;
            }
        }
        return std::nullopt;
    }

    // A port nobody listens on right now (the kernel's choice for port 0).
    inline int free_port() {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return 0;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t length = sizeof address;
        int port = 0;
        if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0 &&
            getsockname(fd, reinterpret_cast<sockaddr *>(&address), &length) == 0)
            port = ntohs(address.sin_port);
        close(fd);
        return port;
    }

    inline bool accepts(const int port) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ok = connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0;
        close(fd);
        return ok;
    }

    class throwaway_server {
    public:
        // Starts the server; running() says whether it came up within five seconds.
        explicit throwaway_server(const std::filesystem::path &binary) {
            std::random_device random;
            std::uniform_int_distribution<int> hex{0, 15};
            for (int i = 0; i < 32; ++i) password_ += "0123456789abcdef"[hex(random)];
            port_ = free_port();
            if (port_ == 0) return;
            folder_ = std::filesystem::temp_directory_path() / ("sc-redis-test-" + std::to_string(getpid()) + "-" + std::to_string(port_));
            std::filesystem::create_directories(folder_);
            const auto port = std::to_string(port_);
            const auto folder = folder_.string();
            std::vector<std::string> args{binary.string(), "--port", port, "--bind", "127.0.0.1", "--requirepass", password_,
                                          "--save", "", "--appendonly", "no", "--dir", folder, "--daemonize", "no",
                                          "--loglevel", "warning", "--logfile", (folder_ / "server.log").string()};
            pid_ = fork();
            if (pid_ == 0) {
                std::vector<char *> argv;
                for (auto &arg: args) argv.push_back(arg.data());
                argv.push_back(nullptr);
                execv(argv[0], argv.data());
                _exit(127);
            }
            if (pid_ < 0) return;
            for (int i = 0; i < 100 && !running_; ++i) {
                if (accepts(port_)) running_ = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
        }

        ~throwaway_server() {
            if (pid_ > 0) {
                kill(pid_, SIGTERM);
                int status = 0;
                waitpid(pid_, &status, 0);
            }
            std::error_code ignored;
            if (!folder_.empty()) std::filesystem::remove_all(folder_, ignored);
        }

        throwaway_server(const throwaway_server &) = delete;
        throwaway_server &operator=(const throwaway_server &) = delete;

        [[nodiscard]] bool running() const { return running_; }
        [[nodiscard]] std::string host() const { return "127.0.0.1:" + std::to_string(port_); }
        [[nodiscard]] const std::string &password() const { return password_; }

    private:
        pid_t pid_ = -1;
        int port_ = 0;
        bool running_ = false;
        std::string password_;
        std::filesystem::path folder_;
    };
}
