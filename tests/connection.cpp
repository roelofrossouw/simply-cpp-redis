// Connection handling against a small fake Redis server on loopback, so dropped connections and
// unresponsive servers can be produced on demand: redis_options (AUTH, timeouts) and the
// transparent reconnect after a connection breaks.

#include <redis.h>
#include <sc_test.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
    // Speaks just enough RESP for PING, AUTH, SET and GET, and can drop every connection or stop
    // answering.
    class fake_redis {
    public:
        explicit fake_redis(std::string password = {}) : password_(std::move(password)) {
            listener_ = socket(AF_INET, SOCK_STREAM, 0);
            const int yes = 1;
            setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (bind(listener_, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 ||
                listen(listener_, 16) != 0) {
                throw std::runtime_error("fake Redis could not listen");
            }
            socklen_t size = sizeof address;
            getsockname(listener_, reinterpret_cast<sockaddr *>(&address), &size);
            port_ = ntohs(address.sin_port);
            acceptor_ = std::thread{[this] { accept_loop(); }};
        }

        ~fake_redis() {
            stopping_ = true;
            acceptor_.join();
            for (auto &client: clients_) client.join();
            close(listener_);
        }

        [[nodiscard]] std::string endpoint() const { return "127.0.0.1:" + std::to_string(port_); }
        [[nodiscard]] int connections() const { return connections_; }
        void set_silent(const bool silent) { silent_ = silent; }

        // Closes every open connection, as a restarting server would.
        void drop_connections() {
            const std::lock_guard lock{mutex_};
            for (const int socket_fd: open_) shutdown(socket_fd, SHUT_RDWR);
        }

    private:
        std::string password_;
        int listener_ = -1;
        int port_ = 0;
        std::atomic<bool> stopping_{false};
        std::atomic<bool> silent_{false};
        std::atomic<int> connections_{0};
        std::thread acceptor_;
        std::vector<std::thread> clients_;
        std::mutex mutex_;
        std::vector<int> open_;
        std::map<std::string, std::string> data_;

        // Waits up to 20ms for socket_fd to be readable, so the loops can notice stopping_.
        static bool readable(const int socket_fd) {
            pollfd waiting{socket_fd, POLLIN, 0};
            return poll(&waiting, 1, 20) == 1;
        }

        void accept_loop() {
            while (!stopping_) {
                if (!readable(listener_)) continue;
                const int client = accept(listener_, nullptr, nullptr);
                if (client < 0) continue;
                ++connections_;
                {
                    const std::lock_guard lock{mutex_};
                    open_.push_back(client);
                }
                clients_.emplace_back([this, client] { serve(client); });
            }
        }

        // One complete RESP array of bulk strings from the front of buffer, or nullopt.
        static std::optional<std::vector<std::string>> parse(std::string &buffer) {
            if (buffer.empty() || buffer[0] != '*') return std::nullopt;
            std::size_t position = buffer.find("\r\n");
            if (position == std::string::npos) return std::nullopt;
            const int count = std::stoi(buffer.substr(1, position - 1));
            position += 2;
            std::vector<std::string> command;
            for (int i = 0; i < count; ++i) {
                const auto end = buffer.find("\r\n", position);
                if (end == std::string::npos) return std::nullopt;
                const auto length = std::stoul(buffer.substr(position + 1, end - position - 1));
                if (buffer.size() < end + 2 + length + 2) return std::nullopt;
                command.push_back(buffer.substr(end + 2, length));
                position = end + 2 + length + 2;
            }
            buffer.erase(0, position);
            return command;
        }

        std::string answer(const std::vector<std::string> &command) {
            const std::lock_guard lock{mutex_};
            if (command.empty()) return "-ERR empty\r\n";
            if (command[0] == "PING") return "+PONG\r\n";
            if (command[0] == "AUTH") return command.size() == 2 && command[1] == password_ ? "+OK\r\n" : "-WRONGPASS\r\n";
            if (command[0] == "SET" && command.size() == 3) {
                data_[command[1]] = command[2];
                return "+OK\r\n";
            }
            if (command[0] == "GET" && command.size() == 2) {
                const auto found = data_.find(command[1]);
                if (found == data_.end()) return "$-1\r\n";
                return "$" + std::to_string(found->second.size()) + "\r\n" + found->second + "\r\n";
            }
            return "-ERR unsupported\r\n";
        }

        void serve(const int client) {
            std::string buffer;
            char chunk[4096];
            while (!stopping_) {
                if (!readable(client)) continue;
                const auto received = recv(client, chunk, sizeof chunk, 0);
                if (received <= 0) break;
                buffer.append(chunk, static_cast<std::size_t>(received));
                while (auto command = parse(buffer)) {
                    if (silent_) continue;
                    const auto reply = answer(*command);
                    send(client, reply.data(), reply.size(), 0);
                }
            }
            {
                const std::lock_guard lock{mutex_};
                std::erase(open_, client);
            }
            close(client);
        }
    };

    template<typename Work>
    std::chrono::milliseconds time(Work work) {
        const auto start = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    }
}

int main() {
    SECTION("Set and get");
    fake_redis server;
    sc::redis client{server.endpoint()};
    client.set("first", "1");
    CHECK_EQ(client.get("first").value_or(""), std::string{"1"});
    CHECK(!client.get("missing").has_value());

    SECTION("An error reply is a plain runtime_error, not redis_unavailable");
    {
        bool plain = false;
        try {
            client.hset("first", "field", "value"); // the fake server doesn't know HSET
        } catch (const sc::redis_unavailable &) {
        } catch (const std::runtime_error &) {
            plain = true;
        }
        CHECK(plain);
        CHECK_NOTHROW(client.set("still", "connected"));
    }

    SECTION("A broken connection is replaced transparently");
    const int before = server.connections();
    server.drop_connections();
    CHECK_NOTHROW(client.set("second", "2"));
    CHECK_EQ(client.get("second").value_or(""), std::string{"2"});
    CHECK(server.connections() > before);

    SECTION("AUTH with redis_options");
    {
        fake_redis protected_server{"secret"};
        sc::redis authenticated{protected_server.endpoint(), {.password = "secret"}};
        CHECK_NOTHROW(authenticated.set("key", "value"));
        CHECK_THROWS_AS((sc::redis{protected_server.endpoint(), {.password = "wrong"}}), std::runtime_error);
    }

    SECTION("A command timeout stops waiting for a server that doesn't answer");
    {
        sc::redis impatient{server.endpoint(), {.command_timeout = 200ms}};
        server.set_silent(true);
        bool threw = false;
        const auto waited = time([&] {
            try {
                impatient.set("third", "3");
            } catch (const sc::redis_unavailable &) {
                threw = true;
            }
        });
        CHECK(threw);
        // The stale connection times out, then the fresh one does: about 2 x 200ms.
        CHECK(waited < 2s);

        server.set_silent(false);
        CHECK_NOTHROW(impatient.set("third", "3"));
        CHECK_EQ(impatient.get("third").value_or(""), std::string{"3"});
    }

    SECTION("A connect timeout stops waiting for a server that isn't there");
    {
        bool threw = false;
        const auto waited = time([&] {
            try {
                // 10.255.255.1 is not routed, so the connect gets no answer at all.
                sc::redis unreachable{"10.255.255.1:6379", {.connect_timeout = 200ms}};
            } catch (const sc::redis_unavailable &) {
                threw = true;
            }
        });
        CHECK(threw);
        CHECK(waited < 2s);
    }

    TEST_SUMMARY();
}
