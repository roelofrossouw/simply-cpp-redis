#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sc {
    struct redis_endpoint {
        std::string host;
        int port = 6379;
    };

    struct redis_connection {
        std::string host;
        int port = 6379;
        int db = 0;
        bool decode_responses = true;
        std::string password;
    };

    class redis {
    public:
        explicit redis(std::string server, int port = 6379);
        explicit redis(std::vector<redis_endpoint> seeds);
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
