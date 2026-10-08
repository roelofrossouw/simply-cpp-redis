#pragma once

#include <cstddef>
#include <ip_endpoints.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sc {
    struct redis_connection {
        std::string host;
        int port = 6379;
        int db = 0;
        bool decode_responses = true;
        std::string password;
    };

    class redis {
    public:
        // One server or several Cluster seed nodes: "redis1;redis2:6380", a
        // std::vector<ip_endpoint> or a braced list. Entries without a port use 6379.
        explicit redis(ip_endpoints seeds);
        explicit redis(redis_connection connection);
        ~redis();

        redis(const redis &) = delete;
        redis &operator=(const redis &) = delete;

        void set(const std::string &key, const std::string &value) const;
        [[nodiscard]] std::optional<std::string> get(const std::string &key) const;

        void hset(const std::string &key, const std::string &field, const std::string &value) const;
        [[nodiscard]] std::optional<std::string> hget(const std::string &key, const std::string &field) const;

        std::size_t erase(const std::string &key) const;

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
