#include "redis.h"

#include <hiredis/hiredis.h>

#include <algorithm>
#include <charconv>
#include <thread>
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

    std::string text_of(const redisReply &reply, const std::string &command) {
        bool text = reply.type == REDIS_REPLY_STRING || reply.type == REDIS_REPLY_STATUS;
#ifdef REDIS_REPLY_VERB
        text = text || reply.type == REDIS_REPLY_VERB;
#endif
        if (reply.type == REDIS_REPLY_ERROR) throw std::runtime_error("Redis " + command + " failed: " + reply_text(reply));
        if (!text) throw std::runtime_error("Redis " + command + " returned an unexpected reply");
        return reply_text(reply);
    }

    void validate_endpoint(const sc::ip_endpoint &endpoint) {
        if (endpoint.host.empty() || endpoint.port <= 0 || endpoint.port > 65535) {
            throw std::invalid_argument("Redis endpoint must have a host and port from 1 to 65535");
        }
    }

    // A null reply means the connection failed; connection.errstr says why.
    reply_ptr run_command(redisContext &connection, const std::vector<std::string> &command) {
        std::vector<const char *> arguments;
        std::vector<std::size_t> lengths;
        arguments.reserve(command.size());
        lengths.reserve(command.size());
        for (const auto &argument : command) {
            arguments.push_back(argument.data());
            lengths.push_back(argument.size());
        }
        return {static_cast<redisReply *>(redisCommandArgv(&connection, static_cast<int>(arguments.size()),
                                                           arguments.data(), lengths.data())),
                freeReplyObject};
    }

    void expect_ok(redisContext &connection, const std::vector<std::string> &command, const std::string_view operation) {
        const auto reply = run_command(connection, command);
        if (!reply || reply->type != REDIS_REPLY_STATUS || reply_text(*reply) != "OK") {
            throw std::runtime_error("Redis " + std::string(operation) + " failed: " +
                                     (reply ? reply_text(*reply) : connection.errstr));
        }
    }

    std::size_t count_reply(const redisReply &reply, const std::string_view command) {
        if (reply.type != REDIS_REPLY_INTEGER || reply.integer < 0) {
            throw std::runtime_error("Redis " + std::string(command) + " returned an unexpected reply");
        }
        return static_cast<std::size_t>(reply.integer);
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
        seeds_ = std::move(seeds);

        // Commands go to the first seed that answers.
        std::string last_error;
        for (std::size_t i = 0; i < seeds_.size(); ++i) {
            try {
                auto reply = execute_on(seeds_[i], {"PING"}, false);
                if (reply->type == REDIS_REPLY_STATUS && reply_text(*reply) == "PONG") {
                    current_ = i;
                    return;
                }
                last_error = "Redis seed did not return PONG";
            } catch (const std::runtime_error &error) {
                last_error = error.what();
            }
        }
        throw sc::redis_unavailable("Unable to connect to any Redis endpoint: " + last_error);
    }

    // Runs command from the current seed. When that seed can't be connected to, the next seeds are
    // tried in turn and the first that connects becomes the current one. A connection that breaks
    // mid-command isn't moved to another seed (it may already have run): that is retried once on
    // the same node, as execute_on() does, and otherwise reported.
    reply_ptr execute(const std::vector<std::string> &command) const {
        for (std::size_t tried = 1;; ++tried) {
            try {
                return execute_from(seeds_[current_], command);
            } catch (const seed_unreachable &error) {
                if (tried >= seeds_.size()) throw sc::redis_unavailable(error.what());
                current_ = (current_ + 1) % seeds_.size();
            }
        }
    }

    // A command's text reply (a bulk string, RESP3 verbatim text or a status), from the current seed
    // or from node; Redis's error as std::runtime_error.
    std::string text_from_seed(const std::vector<std::string> &command) const { return text_of(*execute(command), command.front()); }

    std::string text_on(const ip_endpoint &node, const std::vector<std::string> &command) const {
        validate_endpoint(node);
        return text_of(*execute_on(node, command, false), command.front());
    }

    std::size_t change_members(const std::string &command, const std::string &key,
                               const std::vector<std::string> &members) const {
        if (members.empty()) return 0;
        std::vector<std::string> arguments{command, key};
        arguments.insert(arguments.end(), members.begin(), members.end());
        return count_reply(*execute(arguments), command);
    }

    // Every key matching pattern: SCAN on each master of a Redis Cluster (CLUSTER NODES), or on
    // the current seed when the server isn't a cluster.
    std::vector<std::string> scan(const std::string &pattern, const std::size_t count) const {
        std::vector<std::string> found;
        for (const auto &node: masters()) {
            std::string cursor = "0";
            do {
                const auto reply = execute_on(node, {"SCAN", cursor, "MATCH", pattern, "COUNT", std::to_string(count)}, false);
                if (reply->type == REDIS_REPLY_ERROR) throw std::runtime_error("Redis SCAN failed: " + reply_text(*reply));
                if (reply->type != REDIS_REPLY_ARRAY || reply->elements != 2 || reply->element[0]->type != REDIS_REPLY_STRING ||
                    reply->element[1]->type != REDIS_REPLY_ARRAY) {
                    throw std::runtime_error("Redis SCAN returned an unexpected reply");
                }
                cursor = reply_text(*reply->element[0]);
                const auto &keys = *reply->element[1];
                for (std::size_t i = 0; i < keys.elements; ++i) found.push_back(reply_text(*keys.element[i]));
            } while (cursor != "0");
        }
        return found;
    }

private:
    struct seed_unreachable : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Runs command on seed, following Redis Cluster MOVED and ASK redirections.
    reply_ptr execute_from(const ip_endpoint &seed, const std::vector<std::string> &command) const {
        ip_endpoint endpoint = seed;
        bool asking = false;

        for (int attempt = 0; attempt < 5; ++attempt) {
            reply_ptr reply{nullptr, freeReplyObject};
            try {
                reply = execute_on(endpoint, command, asking);
            } catch (const connection_lost &) {
                throw;
            } catch (const sc::redis_unavailable &error) {
                if (attempt == 0) throw seed_unreachable(error.what()); // couldn't connect to the seed
                throw;
            }
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

    // The masters of a Redis Cluster, from CLUSTER NODES ("<id> <ip:port@cport> <flags> ..."), or
    // just the current seed when the server isn't one.
    std::vector<ip_endpoint> masters() const {
        const auto &seed = seeds_[current_];
        const auto reply = execute_on(seed, {"CLUSTER", "NODES"}, false);
        if (reply->type == REDIS_REPLY_ERROR) return {seed}; // "cluster support disabled"
        // RESP2 answers with a bulk string; RESP3 with verbatim text (hiredis 1.0 and later).
        bool text_reply = reply->type == REDIS_REPLY_STRING;
#ifdef REDIS_REPLY_VERB
        text_reply = text_reply || reply->type == REDIS_REPLY_VERB;
#endif
        if (!text_reply) throw std::runtime_error("Redis CLUSTER NODES returned an unexpected reply");
        std::vector<ip_endpoint> nodes;
        const auto text = reply_text(*reply);
        std::size_t start = 0;
        while (start < text.size()) {
            auto end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::vector<std::string_view> fields;
            for (std::size_t at = start; at < end;) {
                const auto space = std::min(text.find(' ', at), end);
                fields.emplace_back(std::string_view{text}.substr(at, space - at));
                at = space + 1;
            }
            start = end + 1;
            if (fields.size() < 3) continue;
            const auto flags = fields[2];
            if (flags.find("master") == std::string_view::npos || flags.find("fail") != std::string_view::npos ||
                flags.find("noaddr") != std::string_view::npos) {
                continue;
            }
            auto address = fields[1].substr(0, fields[1].find('@'));
            if (const auto comma = address.find(','); comma != std::string_view::npos) address = address.substr(0, comma); // ,hostname
            auto node = ip_endpoint::parse(address);
            if (node.host.empty()) node.host = seed.host; // a node that doesn't know its own address yet
            nodes.push_back(std::move(node));
        }
        if (nodes.empty()) throw std::runtime_error("Redis CLUSTER NODES lists no master");
        return nodes;
    }

    static timeval to_timeval(const std::chrono::milliseconds duration) {
        return {static_cast<decltype(timeval::tv_sec)>(duration.count() / 1000),
                static_cast<decltype(timeval::tv_usec)>(duration.count() % 1000 * 1000)};
    }

    struct context {
        context(const ip_endpoint &endpoint, const sc::redis_options &options) : connection(nullptr, redisFree) {
            connection.reset(redisConnectWithTimeout(endpoint.host.c_str(), endpoint.port,
                                                     to_timeval(options.connect_timeout)));
            if (!connection) throw std::runtime_error("Unable to allocate Redis connection");
            if (connection->err) throw sc::redis_unavailable("Unable to connect to Redis " + endpoint.to_string() +
                                                              ": " + connection->errstr);
            if (options.command_timeout.count() > 0 &&
                redisSetTimeout(connection.get(), to_timeval(options.command_timeout)) != REDIS_OK) {
                throw std::runtime_error("Unable to set the Redis command timeout for " + endpoint.to_string());
            }
            if (!options.password.empty()) expect_ok(*connection, {"AUTH", options.password}, "AUTH");
            if (options.db != 0) expect_ok(*connection, {"SELECT", std::to_string(options.db)}, "SELECT");
        }

        std::unique_ptr<redisContext, decltype(&redisFree)> connection;
    };

    std::vector<ip_endpoint> seeds_;
    mutable std::size_t current_ = 0; // the seed commands start from
    sc::redis_options options_;
    mutable std::unordered_map<std::string, std::unique_ptr<context>> contexts_;

    // The connection to endpoint, made if there is none yet. fresh says whether it was just made.
    redisContext &connection_for(const ip_endpoint &endpoint, bool &fresh) const {
        const auto key = endpoint.to_string();
        if (const auto existing = contexts_.find(key); existing != contexts_.end()) {
            fresh = false;
            return *existing->second->connection;
        }
        // Connect before storing, so a failed connect doesn't leave an empty entry behind.
        auto made = std::make_unique<context>(endpoint, options_);
        fresh = true;
        return *contexts_.emplace(key, std::move(made)).first->second->connection;
    }

    void drop_connection(const ip_endpoint &endpoint) const { contexts_.erase(endpoint.to_string()); }

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

        auto reply = run_command(connection, command);
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

sc::redis::~redis() = default;

void sc::redis::set(const std::string &key, const std::string &value) const {
    const auto reply = implementation_->execute({"SET", key, value});
    if (reply->type != REDIS_REPLY_STATUS || reply_text(*reply) != "OK") {
        throw std::runtime_error("Redis SET failed: " + reply_text(*reply));
    }
}

bool sc::redis::set_if_absent(const std::string &key, const std::string &value,
                              const std::chrono::seconds ttl) const {
    if (ttl < std::chrono::seconds{1}) throw std::invalid_argument("Redis SET NX expiry must be at least 1 second");
    const auto reply = implementation_->execute({"SET", key, value, "NX", "EX", std::to_string(ttl.count())});
    if (reply->type == REDIS_REPLY_NIL) return false;
    if (reply->type != REDIS_REPLY_STATUS || reply_text(*reply) != "OK") {
        throw std::runtime_error("Redis SET NX returned an unexpected reply");
    }
    return true;
}

namespace {
    std::optional<std::string> optional_string(const redisReply &reply, const std::string_view command) {
        if (reply.type == REDIS_REPLY_NIL) return std::nullopt;
        if (reply.type != REDIS_REPLY_STRING) {
            throw std::runtime_error("Redis " + std::string(command) + " returned an unexpected reply");
        }
        return reply_text(reply);
    }
}

std::optional<std::string> sc::redis::get(const std::string &key) const {
    return optional_string(*implementation_->execute({"GET", key}), "GET");
}

void sc::redis::hset(const std::string &key, const std::string &field, const std::string &value) const {
    const auto reply = implementation_->execute({"HSET", key, field, value});
    if (reply->type != REDIS_REPLY_INTEGER) throw std::runtime_error("Redis HSET returned an unexpected reply");
}

std::optional<std::string> sc::redis::hget(const std::string &key, const std::string &field) const {
    return optional_string(*implementation_->execute({"HGET", key, field}), "HGET");
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

std::size_t sc::redis::sadd(const std::string &key, const std::string &member) const {
    return sadd(key, std::vector{member});
}

std::size_t sc::redis::sadd(const std::string &key, const std::initializer_list<std::string> members) const {
    return sadd(key, std::vector<std::string>{members});
}

std::size_t sc::redis::sadd(const std::string &key, const std::vector<std::string> &members) const {
    return implementation_->change_members("SADD", key, members);
}

std::size_t sc::redis::srem(const std::string &key, const std::string &member) const {
    return srem(key, std::vector{member});
}

std::size_t sc::redis::srem(const std::string &key, const std::initializer_list<std::string> members) const {
    return srem(key, std::vector<std::string>{members});
}

std::size_t sc::redis::srem(const std::string &key, const std::vector<std::string> &members) const {
    return implementation_->change_members("SREM", key, members);
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
    return count_reply(*implementation_->execute({"DEL", key}), "DEL");
}

std::vector<std::string> sc::redis::keys(const std::string &pattern, const std::size_t count) const {
    if (count == 0) throw std::invalid_argument("Redis SCAN count must be at least 1");
    return implementation_->scan(pattern, count);
}

std::optional<std::string> sc::redis::eval(const std::string &script, const std::vector<std::string> &keys,
                                           const std::vector<std::string> &arguments) const {
    std::vector<std::string> command{"EVAL", script, std::to_string(keys.size())};
    command.insert(command.end(), keys.begin(), keys.end());
    command.insert(command.end(), arguments.begin(), arguments.end());
    const auto reply = implementation_->execute(command);
    switch (reply->type) {
        case REDIS_REPLY_NIL:
            return std::nullopt;
        case REDIS_REPLY_INTEGER:
            return std::to_string(reply->integer);
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            return reply_text(*reply);
        default:
            throw std::runtime_error("Redis EVAL returned a table; return a single value from the script");
    }
}

bool sc::redis::expire(const std::string &key, const std::chrono::seconds ttl) const {
    const auto reply = implementation_->execute({"EXPIRE", key, std::to_string(ttl.count())});
    if (reply->type != REDIS_REPLY_INTEGER) throw std::runtime_error("Redis EXPIRE returned an unexpected reply");
    return reply->integer == 1;
}

bool sc::redis_cluster_node::has(const std::string_view flag) const {
    return std::ranges::find(flags, flag) != flags.end();
}

std::vector<std::string> sc::redis_cluster_node::slot_ranges() const {
    std::vector<std::string> out;
    for (const auto &[first, last]: slots) out.push_back(first == last ? std::to_string(first) : std::to_string(first) + "-" + std::to_string(last));
    return out;
}

namespace {
    std::vector<std::string_view> fields_of(const std::string_view line) {
        std::vector<std::string_view> out;
        std::size_t at = 0;
        while (at < line.size()) {
            const auto space = std::min(line.find(' ', at), line.size());
            if (space > at) out.push_back(line.substr(at, space - at));
            at = space + 1;
        }
        return out;
    }

    std::int64_t number_of(const std::string_view text, const std::string_view what, const std::string_view line) {
        std::int64_t value = 0;
        const auto [end, problem] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (problem != std::errc{} || end != text.data() + text.size())
            throw std::runtime_error("Redis CLUSTER NODES: " + std::string{what} + " isn't a number in \"" + std::string{line} + "\"");
        return value;
    }
}

std::vector<sc::redis_cluster_node> sc::parse_cluster_nodes(const std::string_view text) {
    std::vector<redis_cluster_node> nodes;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        const auto fields = fields_of(line);
        if (fields.size() < 8) throw std::runtime_error("Redis CLUSTER NODES: \"" + std::string{line} + "\" has too few fields");
        redis_cluster_node node;
        node.id = fields[0];
        // ip:port@cport[,hostname]
        auto address = fields[1];
        if (const auto comma = address.find(','); comma != std::string_view::npos) address = address.substr(0, comma);
        if (const auto at = address.find('@'); at != std::string_view::npos) {
            node.bus_port = static_cast<int>(number_of(address.substr(at + 1), "the bus port", line));
            address = address.substr(0, at);
        }
        if (!address.empty() && address != ":0") node.address = ip_endpoint::parse(address);
        for (std::size_t from = 0; from <= fields[2].size();) {
            const auto comma = std::min(fields[2].find(',', from), fields[2].size());
            if (comma > from) node.flags.emplace_back(fields[2].substr(from, comma - from));
            from = comma + 1;
        }
        if (fields[3] != "-") node.master_id = fields[3];
        node.ping_sent = number_of(fields[4], "ping-sent", line);
        node.pong_received = number_of(fields[5], "pong-recv", line);
        node.config_epoch = number_of(fields[6], "config-epoch", line);
        node.link = fields[7];
        for (std::size_t i = 8; i < fields.size(); ++i) {
            const auto slot = fields[i];
            if (slot.starts_with('[')) {
                node.migrating.emplace_back(slot);
                continue;
            }
            const auto dash = slot.find('-');
            const auto first = static_cast<int>(number_of(slot.substr(0, dash), "a slot", line));
            const auto last = dash == std::string_view::npos ? first : static_cast<int>(number_of(slot.substr(dash + 1), "a slot", line));
            node.slots.emplace_back(first, last);
        }
        nodes.push_back(std::move(node));
    }
    return nodes;
}

std::map<std::string, std::string> sc::parse_info(const std::string_view text) {
    std::map<std::string, std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        auto line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        out.emplace(std::string{line.substr(0, colon)}, std::string{line.substr(colon + 1)});
    }
    return out;
}

std::vector<sc::redis_cluster_node> sc::redis::cluster_nodes() const {
    return parse_cluster_nodes(implementation_->text_from_seed({"CLUSTER", "NODES"}));
}

std::map<std::string, std::string> sc::redis::cluster_info() const {
    return parse_info(implementation_->text_from_seed({"CLUSTER", "INFO"}));
}

std::map<std::string, std::string> sc::redis::info(const ip_endpoint &node, const std::string &section) const {
    std::vector<std::string> command{"INFO"};
    if (!section.empty()) command.push_back(section);
    return parse_info(implementation_->text_on(node, command));
}

void sc::redis::cluster_failover(const ip_endpoint &replica, const std::chrono::seconds limit) const {
    const auto answer = implementation_->text_on(replica, {"CLUSTER", "FAILOVER"});
    if (answer != "OK") throw std::runtime_error("Redis CLUSTER FAILOVER on " + replica.to_string() + " answered " + answer);
    const auto until = std::chrono::steady_clock::now() + limit;
    for (;;) {
        const auto replication = info(replica, "replication");
        if (const auto role = replication.find("role"); role != replication.end() && role->second == "master") return;
        if (std::chrono::steady_clock::now() >= until)
            throw std::runtime_error("Redis " + replica.to_string() + " isn't master " + std::to_string(limit.count()) + " s after CLUSTER FAILOVER");
        std::this_thread::sleep_for(std::chrono::milliseconds{250});
    }
}
