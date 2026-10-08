#include "redis.h"

#include <hiredis/hiredis.h>

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

    std::string endpoint_key(const sc::ip_endpoint &endpoint) {
        return endpoint.host + ':' + std::to_string(endpoint.port);
    }

    void validate_endpoint(const sc::ip_endpoint &endpoint) {
        if (endpoint.host.empty() || endpoint.port <= 0 || endpoint.port > 65535) {
            throw std::invalid_argument("Redis endpoint must have a host and port from 1 to 65535");
        }
    }

    void expect_ok(redisContext &connection, const std::vector<std::string> &command, const std::string_view operation) {
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
        if (!reply || reply->type != REDIS_REPLY_STATUS || reply_text(*reply) != "OK") {
            throw std::runtime_error("Redis " + std::string(operation) + " failed: " +
                                     (reply ? reply_text(*reply) : connection.errstr));
        }
    }

    sc::ip_endpoint redirect_endpoint(const redisReply &reply) {
        const auto message = reply_text(reply);
        try {
            return sc::ip_endpoint::from_redis(message);
        } catch (const std::invalid_argument &) {
            throw std::runtime_error("Invalid Redis Cluster redirect: " + message);
        }
    }
}

class sc::redis::implementation {
public:
    implementation(std::vector<ip_endpoint> seeds, redis_connection connection)
        : connection_(std::move(connection)) {
        if (seeds.empty()) throw std::invalid_argument("At least one Redis endpoint is required");
        if (connection_.db < 0) throw std::invalid_argument("Redis database must not be negative");
        if (!connection_.decode_responses) {
            throw std::invalid_argument("sc::redis supports decoded string responses only");
        }
        for (const auto &seed : seeds) validate_endpoint(seed);
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
        throw std::runtime_error("Unable to connect to any Redis endpoint: " + last_error);
    }

    reply_ptr execute(const std::vector<std::string> &command) const {
        ip_endpoint endpoint = initial_endpoint_;
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
        context(const ip_endpoint &endpoint, const redis_connection &settings) : connection(nullptr, redisFree) {
            const timeval timeout{2, 0};
            connection.reset(redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port, timeout));
            if (!connection) throw std::runtime_error("Unable to allocate Redis connection");
            if (connection->err) throw std::runtime_error("Unable to connect to Redis " + endpoint_key(endpoint) +
                                                           ": " + connection->errstr);
            if (!settings.password.empty()) expect_ok(*connection, {"AUTH", settings.password}, "AUTH");
            if (settings.db != 0) expect_ok(*connection, {"SELECT", std::to_string(settings.db)}, "SELECT");
        }

        std::unique_ptr<redisContext, decltype(&redisFree)> connection;
    };

    ip_endpoint initial_endpoint_;
    redis_connection connection_;
    mutable std::unordered_map<std::string, std::unique_ptr<context>> contexts_;

    redisContext &connection_for(const ip_endpoint &endpoint) const {
        const auto key = endpoint_key(endpoint);
        const auto [it, inserted] = contexts_.try_emplace(key);
        if (inserted) it->second = std::make_unique<context>(endpoint, connection_);
        return *it->second->connection;
    }

    reply_ptr execute_on(const ip_endpoint &endpoint, const std::vector<std::string> &command,
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

sc::redis::redis(std::string server, const int port)
    : redis(std::vector<ip_endpoint>{{std::move(server), port}}) {
}

sc::redis::redis(std::vector<ip_endpoint> seeds)
    : implementation_(std::make_unique<implementation>(std::move(seeds), redis_connection{})) {
}

sc::redis::redis(redis_connection connection)
    : implementation_(std::make_unique<implementation>(
          std::vector<ip_endpoint>{{connection.host, connection.port}}, std::move(connection))) {
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
