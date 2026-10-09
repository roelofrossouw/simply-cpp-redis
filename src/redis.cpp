#include "redis.h"

#include <hiredis/hiredis.h>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {
    using reply_ptr = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

    // An open connection failed (closed, reset or timed out), as opposed to Redis answering
    // with an error. The connection is unusable afterwards.
    struct connection_lost : sc::redis_unavailable {
        using sc::redis_unavailable::redis_unavailable;
    };

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
    implementation(std::vector<ip_endpoint> seeds, sc::redis_options options)
        : options_(std::move(options)) {
        if (seeds.empty()) throw std::invalid_argument("At least one Redis endpoint is required");
        if (options_.db < 0) throw std::invalid_argument("Redis database must not be negative");
        if (options_.connect_timeout.count() <= 0) throw std::invalid_argument("Redis connect timeout must be positive");
        if (options_.command_timeout.count() < 0) throw std::invalid_argument("Redis command timeout must not be negative");
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
        throw sc::redis_unavailable("Unable to connect to any Redis endpoint: " + last_error);
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
    static timeval to_timeval(const std::chrono::milliseconds duration) {
        return {static_cast<decltype(timeval::tv_sec)>(duration.count() / 1000),
                static_cast<decltype(timeval::tv_usec)>(duration.count() % 1000 * 1000)};
    }

    struct context {
        context(const ip_endpoint &endpoint, const sc::redis_options &options) : connection(nullptr, redisFree) {
            connection.reset(redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port,
                                                     to_timeval(options.connect_timeout)));
            if (!connection) throw std::runtime_error("Unable to allocate Redis connection");
            if (connection->err) throw sc::redis_unavailable("Unable to connect to Redis " + endpoint_key(endpoint) +
                                                              ": " + connection->errstr);
            if (options.command_timeout.count() > 0 &&
                redisSetTimeout(connection.get(), to_timeval(options.command_timeout)) != REDIS_OK) {
                throw std::runtime_error("Unable to set the Redis command timeout for " + endpoint_key(endpoint));
            }
            if (!options.password.empty()) expect_ok(*connection, {"AUTH", options.password}, "AUTH");
            if (options.db != 0) expect_ok(*connection, {"SELECT", std::to_string(options.db)}, "SELECT");
        }

        std::unique_ptr<redisContext, decltype(&redisFree)> connection;
    };

    ip_endpoint initial_endpoint_;
    sc::redis_options options_;
    mutable std::unordered_map<std::string, std::unique_ptr<context>> contexts_;

    // The connection to endpoint, made if there is none yet. fresh says whether it was just made.
    redisContext &connection_for(const ip_endpoint &endpoint, bool &fresh) const {
        const auto key = endpoint_key(endpoint);
        if (const auto existing = contexts_.find(key); existing != contexts_.end()) {
            fresh = false;
            return *existing->second->connection;
        }
        // Connect before storing, so a failed connect doesn't leave an empty entry behind.
        auto made = std::make_unique<context>(endpoint, options_);
        fresh = true;
        return *contexts_.emplace(key, std::move(made)).first->second->connection;
    }

    // A connection that failed (closed, reset, timed out) can't be used again; dropping it
    // makes the next command to that endpoint reconnect.
    void drop_connection(const ip_endpoint &endpoint) const { contexts_.erase(endpoint_key(endpoint)); }

    // Runs command on endpoint. When a connection that was already open fails (it went stale
    // while idle, or the server restarted), it is replaced and the command tried once more.
    reply_ptr execute_on(const ip_endpoint &endpoint, const std::vector<std::string> &command,
                         const bool asking) const {
        bool fresh = false;
        try {
            return execute_once(connection_for(endpoint, fresh), endpoint, command, asking);
        } catch (const connection_lost &) {
            if (fresh) throw;
        }
        return execute_once(connection_for(endpoint, fresh), endpoint, command, asking);
    }

    reply_ptr execute_once(redisContext &connection, const ip_endpoint &endpoint,
                           const std::vector<std::string> &command, const bool asking) const {
        if (asking) {
            reply_ptr asking_reply{static_cast<redisReply *>(redisCommand(&connection, "ASKING")), freeReplyObject};
            if (!asking_reply) {
                const std::string error = connection.errstr;
                drop_connection(endpoint);
                throw connection_lost("Redis ASKING failed: " + error);
            }
            if (asking_reply->type == REDIS_REPLY_ERROR) {
                throw std::runtime_error("Redis ASKING failed: " + reply_text(*asking_reply));
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
        if (!reply) {
            const std::string error = connection.errstr;
            drop_connection(endpoint);
            throw connection_lost("Redis command failed: " + error);
        }
        return reply;
    }
};

namespace {
    std::vector<sc::ip_endpoint> with_default_port(const sc::ip_endpoints &seeds) {
        std::vector<sc::ip_endpoint> endpoints = seeds;
        for (auto &endpoint: endpoints) {
            if (endpoint.port == 0) endpoint.port = 6379;
        }
        return endpoints;
    }
}

sc::redis::redis(ip_endpoints seeds) : redis(std::move(seeds), redis_options{}) {
}

sc::redis::redis(ip_endpoints seeds, redis_options options)
    : implementation_(std::make_unique<implementation>(with_default_port(seeds), std::move(options))) {
}

namespace {
    sc::redis_options options_from(const sc::redis_connection &connection) {
        if (!connection.decode_responses) {
            throw std::invalid_argument("sc::redis supports decoded string responses only");
        }
        return {connection.password, connection.db};
    }
}

sc::redis::redis(redis_connection connection)
    : implementation_(std::make_unique<implementation>(
          std::vector<ip_endpoint>{{connection.host, connection.port}}, options_from(connection))) {
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

std::vector<std::optional<std::string>> sc::redis::hmget(const std::string &key,
                                                         const std::vector<std::string> &fields) const {
    if (fields.empty()) return {};
    std::vector<std::string> command{"HMGET", key};
    command.insert(command.end(), fields.begin(), fields.end());
    const auto reply = implementation_->execute(command);
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements != fields.size()) {
        throw std::runtime_error("Redis HMGET returned an unexpected reply");
    }
    std::vector<std::optional<std::string>> values;
    values.reserve(fields.size());
    for (std::size_t i = 0; i < reply->elements; ++i) {
        const auto &element = *reply->element[i];
        if (element.type == REDIS_REPLY_NIL) values.emplace_back(std::nullopt);
        else if (element.type == REDIS_REPLY_STRING) values.emplace_back(reply_text(element));
        else throw std::runtime_error("Redis HMGET returned an unexpected value");
    }
    return values;
}

std::map<std::string, std::string> sc::redis::hgetall(const std::string &key) const {
    const auto reply = implementation_->execute({"HGETALL", key});
    // RESP2 answers with a flat array of field, value, field, value...; RESP3 with a map.
    bool flat_pairs = reply->type == REDIS_REPLY_ARRAY;
#ifdef REDIS_REPLY_MAP
    flat_pairs = flat_pairs || reply->type == REDIS_REPLY_MAP;
#endif
    if (!flat_pairs || reply->elements % 2 != 0) throw std::runtime_error("Redis HGETALL returned an unexpected reply");
    std::map<std::string, std::string> fields;
    for (std::size_t i = 0; i < reply->elements; i += 2) {
        const auto &field = *reply->element[i];
        const auto &value = *reply->element[i + 1];
        if (field.type != REDIS_REPLY_STRING || value.type != REDIS_REPLY_STRING) {
            throw std::runtime_error("Redis HGETALL returned an unexpected value");
        }
        fields.emplace(reply_text(field), reply_text(value));
    }
    return fields;
}

namespace {
    std::size_t count_reply(const redisReply &reply, const std::string_view command) {
        if (reply.type != REDIS_REPLY_INTEGER || reply.integer < 0) {
            throw std::runtime_error("Redis " + std::string(command) + " returned an unexpected reply");
        }
        return static_cast<std::size_t>(reply.integer);
    }
}

std::size_t sc::redis::sadd(const std::string &key, const std::string &member) const {
    return sadd(key, std::vector{member});
}

std::size_t sc::redis::sadd(const std::string &key, const std::initializer_list<std::string> members) const {
    return sadd(key, std::vector<std::string>{members});
}

std::size_t sc::redis::sadd(const std::string &key, const std::vector<std::string> &members) const {
    if (members.empty()) return 0;
    std::vector<std::string> command{"SADD", key};
    command.insert(command.end(), members.begin(), members.end());
    return count_reply(*implementation_->execute(command), "SADD");
}

std::size_t sc::redis::srem(const std::string &key, const std::string &member) const {
    return srem(key, std::vector{member});
}

std::size_t sc::redis::srem(const std::string &key, const std::initializer_list<std::string> members) const {
    return srem(key, std::vector<std::string>{members});
}

std::size_t sc::redis::srem(const std::string &key, const std::vector<std::string> &members) const {
    if (members.empty()) return 0;
    std::vector<std::string> command{"SREM", key};
    command.insert(command.end(), members.begin(), members.end());
    return count_reply(*implementation_->execute(command), "SREM");
}

std::size_t sc::redis::scard(const std::string &key) const {
    return count_reply(*implementation_->execute({"SCARD", key}), "SCARD");
}

std::set<std::string> sc::redis::smembers(const std::string &key) const {
    const auto reply = implementation_->execute({"SMEMBERS", key});
    // RESP2 answers with an array; RESP3 with a set.
    bool members_list = reply->type == REDIS_REPLY_ARRAY;
#ifdef REDIS_REPLY_SET
    members_list = members_list || reply->type == REDIS_REPLY_SET;
#endif
    if (!members_list) throw std::runtime_error("Redis SMEMBERS returned an unexpected reply");
    std::set<std::string> members;
    for (std::size_t i = 0; i < reply->elements; ++i) {
        const auto &member = *reply->element[i];
        if (member.type != REDIS_REPLY_STRING) throw std::runtime_error("Redis SMEMBERS returned an unexpected value");
        members.insert(reply_text(member));
    }
    return members;
}

bool sc::redis::sismember(const std::string &key, const std::string &member) const {
    const auto reply = implementation_->execute({"SISMEMBER", key, member});
    if (reply->type != REDIS_REPLY_INTEGER) throw std::runtime_error("Redis SISMEMBER returned an unexpected reply");
    return reply->integer == 1;
}

std::size_t sc::redis::erase(const std::string &key) const {
    const auto reply = implementation_->execute({"DEL", key});
    if (reply->type != REDIS_REPLY_INTEGER || reply->integer < 0) {
        throw std::runtime_error("Redis DEL returned an unexpected reply");
    }
    return static_cast<std::size_t>(reply->integer);
}
