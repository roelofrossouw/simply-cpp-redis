#include "redis.h"

#include <hiredis/hiredis.h>

#include <charconv>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
    using reply_ptr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    std::string reply_text(const redisReply &reply) {
        return reply.str ? std::string(reply.str, reply.len) : "unknown Redis error";
    }

    std::string endpoint_key(const sc::redis_endpoint &endpoint) {
        return endpoint.host + ':' + std::to_string(endpoint.port);
    }

    sc::redis_endpoint redirect_endpoint(const redisReply &reply) {
        const auto message = reply_text(reply);
        const auto first_space = message.find(' ');
        const auto second_space = message.find(' ', first_space + 1);
        if (first_space == std::string::npos || second_space == std::string::npos) {
            throw std::runtime_error("Invalid Redis Cluster redirect: " + message);
        }

        const auto address = message.substr(second_space + 1);
        const auto port_separator = address.rfind(':');
        if (port_separator == std::string::npos) {
            throw std::runtime_error("Invalid Redis Cluster redirect address: " + address);
        }

        sc::redis_endpoint endpoint{address.substr(0, port_separator), 0};
        const auto port_text = std::string_view{address}.substr(port_separator + 1);
        const auto [end, error] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), endpoint.port);
        if (error != std::errc{} || end != port_text.data() + port_text.size() || endpoint.host.empty() ||
            endpoint.port <= 0 || endpoint.port > 65535) {
            throw std::runtime_error("Invalid Redis Cluster redirect address: " + address);
        }
        return endpoint;
    }
}

class sc::redis::implementation {
public:
    explicit implementation(std::vector<redis_endpoint> seeds) {
        if (seeds.empty()) throw std::invalid_argument("At least one Redis Cluster seed is required");
        initial_endpoint_ = seeds.front();

        std::string last_error;
        for (const auto &seed : seeds) {
            try {
                auto reply = execute_on(seed, {"PING"}, false);
                if (reply->type == REDIS_REPLY_STATUS && reply_text(*reply) == "PONG") return;
                last_error = "Redis seed did not return PONG";
            } catch (const std::runtime_error &error) {
                last_error = error.what();
            }
        }
        throw std::runtime_error("Unable to connect to any Redis Cluster seed: " + last_error);
    }

    reply_ptr execute(const std::vector<std::string> &command) const {
        redis_endpoint endpoint = initial_endpoint_;
        bool asking = false;

        for (int attempt = 0; attempt < 5; ++attempt) {
            auto reply = execute_on(endpoint, command, asking);
            asking = false;
            if (reply->type != REDIS_REPLY_ERROR) return reply;

            const auto message = reply_text(*reply);
            if (message.starts_with("MOVED ") || message.starts_with("ASK ")) {
                endpoint = redirect_endpoint(*reply);
                asking = message.starts_with("ASK ");
                continue;
            }
            throw std::runtime_error("Redis command failed: " + message);
        }
        throw std::runtime_error("Redis Cluster redirected the command too many times");
    }

private:
    struct context {
        explicit context(const redis_endpoint &endpoint) : connection(nullptr, redisFree) {
            const timeval timeout{2, 0};
            connection.reset(redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port, timeout));
            if (!connection) throw std::runtime_error("Unable to allocate Redis connection");
            if (connection->err) throw std::runtime_error("Unable to connect to Redis " + endpoint_key(endpoint) +
                                                           ": " + connection->errstr);
        }

        std::unique_ptr<redisContext, decltype(&redisFree)> connection;
    };

    redis_endpoint initial_endpoint_;
    mutable std::unordered_map<std::string, std::unique_ptr<context>> contexts_;

    redisContext &connection_for(const redis_endpoint &endpoint) const {
        const auto key = endpoint_key(endpoint);
        const auto [it, inserted] = contexts_.try_emplace(key);
        if (inserted) it->second = std::make_unique<context>(endpoint);
        return *it->second->connection;
    }

    reply_ptr execute_on(const redis_endpoint &endpoint, const std::vector<std::string> &command,
                         const bool asking) const {
        auto &connection = connection_for(endpoint);
        if (asking) {
            reply_ptr asking_reply{static_cast<redisReply *>(redisCommand(&connection, "ASKING")), freeReplyObject};
            if (!asking_reply || asking_reply->type == REDIS_REPLY_ERROR) {
                throw std::runtime_error("Redis ASKING failed: " +
                                         (asking_reply ? reply_text(*asking_reply) : connection.errstr));
            }
        }

        std::vector<const char *> arguments;
        std::vector<std::size_t> lengths;
        arguments.reserve(command.size());
        lengths.reserve(command.size());
        for (const auto &argument : command) {
            arguments.push_back(argument.data());
            lengths.push_back(argument.size());
        }

        reply_ptr reply{static_cast<redisReply *>(redisCommandArgv(
                            &connection, static_cast<int>(arguments.size()), arguments.data(), lengths.data())),
                        freeReplyObject};
        if (!reply) throw std::runtime_error("Redis command failed: " + std::string(connection.errstr));
        return reply;
    }
};

sc::redis::redis(std::vector<redis_endpoint> seeds) : implementation_(std::make_unique<implementation>(std::move(seeds))) {
}

sc::redis::~redis() = default;

void sc::redis::set(const std::string &key, const std::string &value) const {
    const auto reply = implementation_->execute({"SET", key, value});
    if (reply->type != REDIS_REPLY_STATUS || reply_text(*reply) != "OK") {
        throw std::runtime_error("Redis SET failed: " + reply_text(*reply));
    }
}

std::optional<std::string> sc::redis::get(const std::string &key) const {
    const auto reply = implementation_->execute({"GET", key});
    if (reply->type == REDIS_REPLY_NIL) return std::nullopt;
    if (reply->type != REDIS_REPLY_STRING) throw std::runtime_error("Redis GET returned an unexpected reply");
    return reply_text(*reply);
}

void sc::redis::hset(const std::string &key, const std::string &field, const std::string &value) const {
    const auto reply = implementation_->execute({"HSET", key, field, value});
    if (reply->type != REDIS_REPLY_INTEGER) throw std::runtime_error("Redis HSET returned an unexpected reply");
}

std::optional<std::string> sc::redis::hget(const std::string &key, const std::string &field) const {
    const auto reply = implementation_->execute({"HGET", key, field});
    if (reply->type == REDIS_REPLY_NIL) return std::nullopt;
    if (reply->type != REDIS_REPLY_STRING) throw std::runtime_error("Redis HGET returned an unexpected reply");
    return reply_text(*reply);
}

std::size_t sc::redis::erase(const std::string &key) const {
    const auto reply = implementation_->execute({"DEL", key});
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer < 0) {
        throw std::runtime_error("Redis DEL returned an unexpected reply");
    }
    return static_cast<std::size_t>(reply->integer);
}
