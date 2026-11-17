#pragma once

// A throwaway Redis Cluster for the tests: three cluster-enabled masters (valkey-server or
// redis-server, as throwaway_server.h finds it) on free localhost ports, each in its own temporary
// folder, no persistence and no password. The slots are split three ways with CLUSTER ADDSLOTS
// (Redis 6.0 has no ADDSLOTSRANGE), the nodes meet with CLUSTER MEET, and the cluster is ready once
// every node reports cluster_state:ok and knows the other two. Every process and folder goes when
// the object goes. The cluster bus is port + 10000 (Redis 6.0 can't choose it), so the ports come
// from 20000-39999, where both are below 65536.

#include "throwaway_server.h"

#include <hiredis/hiredis.h>

#include <memory>
#include <string_view>

namespace sc_redis_test {
    // Whether a localhost port can be bound right now.
    inline bool bindable(const int port) {
        const int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool ok = bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof address) == 0;
        close(fd);
        return ok;
    }

    class throwaway_cluster {
    public:
        // Starts the three nodes and joins them; ready() says whether the cluster reached
        // cluster_state:ok within 30 seconds.
        explicit throwaway_cluster(const std::filesystem::path &binary) {
            std::error_code error;
            const auto base = std::filesystem::temp_directory_path(error);
            if (error) return;
            std::string pattern = (base / "sc-redis-cluster-XXXXXX").string();
            if (!mkdtemp(pattern.data())) return;
            folder_ = pattern;

            std::random_device random;
            std::uniform_int_distribution<int> pick{20000, 39999};
            for (int node = 0; node < 3; ++node) {
                int port = 0;
                for (int attempt = 0; attempt < 200 && port == 0; ++attempt) {
                    const int candidate = pick(random);
                    bool taken = false;
                    for (const auto &used: ports_) taken = taken || used == candidate;
                    if (!taken && bindable(candidate) && bindable(candidate + 10000)) port = candidate;
                }
                if (port == 0) return;
                ports_.push_back(port);
                const auto dir = folder_ / std::to_string(port);
                std::filesystem::create_directories(dir, error);
                if (error) return;
                std::vector<std::string> args{binary.string(), "--port", std::to_string(port), "--bind", "127.0.0.1",
                                              "--cluster-enabled", "yes", "--cluster-config-file", (dir / "nodes.conf").string(),
                                              "--cluster-node-timeout", "5000", "--save", "", "--appendonly", "no",
                                              "--dir", dir.string(), "--daemonize", "no", "--loglevel", "warning",
                                              "--logfile", (dir / "server.log").string()};
                const pid_t pid = fork();
                if (pid == 0) {
                    std::vector<char *> argv;
                    for (auto &arg: args) argv.push_back(arg.data());
                    argv.push_back(nullptr);
                    execv(argv[0], argv.data());
                    _exit(127);
                }
                if (pid < 0) return;
                pids_.push_back(pid);
                bool up = false;
                for (int i = 0; i < 100 && !up; ++i) {
                    if (accepts(port)) up = true;
                    else std::this_thread::sleep_for(std::chrono::milliseconds{50});
                }
                if (!up) return;
            }

            // A third of the slots each: 0-5460, 5461-10922, 10923-16383.
            constexpr int slots = 16384;
            for (int node = 0; node < 3; ++node) {
                std::vector<std::string> command{"CLUSTER", "ADDSLOTS"};
                const int first = node * slots / 3, last = (node + 1) * slots / 3;
                for (int slot = first; slot < last; ++slot) command.push_back(std::to_string(slot));
                if (!ok(ports_[node], command)) return;
            }
            for (int node = 1; node < 3; ++node)
                if (!ok(ports_[0], {"CLUSTER", "MEET", "127.0.0.1", std::to_string(ports_[node])})) return;

            for (int i = 0; i < 300 && !ready_; ++i) {
                bool all = true;
                for (const auto port: ports_) {
                    const auto info = text(port, {"CLUSTER", "INFO"});
                    const auto nodes = text(port, {"CLUSTER", "NODES"});
                    all = all && info.find("cluster_state:ok") != std::string::npos &&
                          info.find("cluster_known_nodes:3") != std::string::npos &&
                          nodes.find(" :") == std::string::npos;   // every node knows every address
                }
                if (all) ready_ = true;
                else std::this_thread::sleep_for(std::chrono::milliseconds{100});
            }
        }

        ~throwaway_cluster() {
            for (const auto pid: pids_) kill(pid, SIGTERM);
            for (const auto pid: pids_) {
                int status = 0;
                waitpid(pid, &status, 0);
            }
            std::error_code ignored;
            if (!folder_.empty()) std::filesystem::remove_all(folder_, ignored);
        }

        throwaway_cluster(const throwaway_cluster &) = delete;
        throwaway_cluster &operator=(const throwaway_cluster &) = delete;

        [[nodiscard]] bool ready() const { return ready_; }
        // The seeds, "127.0.0.1:port;127.0.0.1:port;127.0.0.1:port".
        [[nodiscard]] std::string seeds() const {
            std::string out;
            for (const auto port: ports_) out += (out.empty() ? "" : ";") + std::string{"127.0.0.1:"} + std::to_string(port);
            return out;
        }

    private:
        using context_ptr = std::unique_ptr<redisContext, decltype(&redisFree)>;
        using reply_ptr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

        static reply_ptr ask(const int port, const std::vector<std::string> &command) {
            const timeval timeout{2, 0};
            context_ptr context{redisConnectWithTimeout("127.0.0.1", port, timeout), redisFree};
            if (!context || context->err) return {nullptr, freeReplyObject};
            std::vector<const char *> argv;
            std::vector<std::size_t> lengths;
            for (const auto &part: command) {
                argv.push_back(part.data());
                lengths.push_back(part.size());
            }
            return {static_cast<redisReply *>(redisCommandArgv(context.get(), static_cast<int>(argv.size()), argv.data(), lengths.data())),
                    freeReplyObject};
        }

        static bool ok(const int port, const std::vector<std::string> &command) {
            const auto reply = ask(port, command);
            return reply && reply->type == REDIS_REPLY_STATUS;
        }

        static std::string text(const int port, const std::vector<std::string> &command) {
            const auto reply = ask(port, command);
            if (!reply) return {};
            bool text = reply->type == REDIS_REPLY_STRING;
#ifdef REDIS_REPLY_VERB // hiredis 1.0+ (RESP3); jammy's 0.14 has none
            text = text || reply->type == REDIS_REPLY_VERB;
#endif
            if (!text) return {};
            return {reply->str, reply->len};
        }

        std::filesystem::path folder_;
        std::vector<int> ports_;
        std::vector<pid_t> pids_;
        bool ready_ = false;
    };
}
