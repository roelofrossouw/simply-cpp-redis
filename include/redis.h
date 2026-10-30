#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <ip_endpoints.h>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sc {
    // Thrown when Redis can't be reached: no connection could be made, or one broke or timed out
    // (after the one transparent retry). Redis answering with an error (WRONGTYPE, ...) is a
    // plain std::runtime_error instead, so callers can tell an outage from a bad command.
    struct redis_unavailable : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // Settings for redis(ip_endpoints, redis_options), applied to every node it connects to.
    struct redis_options {
        std::string password;                                // sent with AUTH when not empty
        int db = 0;                                          // SELECT; Redis Cluster only has 0
        std::chrono::milliseconds connect_timeout{2000};
        std::chrono::milliseconds command_timeout{0};        // 0: wait for the reply however long
    };

    // One node as CLUSTER NODES gives it ("<id> <ip:port@bus> <flags> <master> <ping> <pong> <epoch>
    // <link> <slots>...").
    struct redis_cluster_node {
        std::string id;
        ip_endpoint address;            // its client address (host empty when it doesn't know it yet)
        int bus_port = 0;               // the cluster bus port, 0 when not given
        std::vector<std::string> flags; // "myself", "master", "slave", "fail?", "fail", "handshake", "noaddr", ...
        std::string master_id;          // a replica's master; empty for a master
        std::int64_t ping_sent = 0, pong_received = 0; // milliseconds since 1970 (0: none pending)
        std::int64_t config_epoch = 0;
        std::string link;               // "connected" or "disconnected"
        std::vector<std::pair<int, int>> slots; // the slot ranges it serves (first, last), a single slot as (n, n)
        std::vector<std::string> migrating;     // slots being moved ("[1234->-<id>]"), as given

        [[nodiscard]] bool has(std::string_view flag) const;
        [[nodiscard]] bool master() const { return has("master"); }
        [[nodiscard]] bool replica() const { return has("slave"); }
        // Marked failing: "fail" (agreed) or "fail?" (this node's view).
        [[nodiscard]] bool failing() const { return has("fail") || has("fail?"); }
        // The ranges as Redis writes them: "0-5460", "5461".
        [[nodiscard]] std::vector<std::string> slot_ranges() const;
    };

    // CLUSTER NODES' text as nodes. Throws std::runtime_error for a line that isn't one.
    [[nodiscard]] std::vector<redis_cluster_node> parse_cluster_nodes(std::string_view text);

    // INFO's (or CLUSTER INFO's) text as its fields: "used_memory:1024" lines, the "# Memory"
    // headings and empty lines left out.
    [[nodiscard]] std::map<std::string, std::string> parse_info(std::string_view text);

    class redis {
    public:
        // One server or several Cluster seed nodes: "redis1;redis2:6380", a
        // std::vector<ip_endpoint> or a braced list. Entries without a port use 6379.
        // Connects (and checks with PING) straight away, throwing if no seed answers.
        // A connection that broke since its last use is replaced once, transparently.
        // Not thread-safe: use one sc::redis per thread, or lock around it.
        explicit redis(ip_endpoints seeds);
        redis(ip_endpoints seeds, redis_options options);
        ~redis();

        redis(const redis &) = delete;
        redis &operator=(const redis &) = delete;

        void set(const std::string &key, const std::string &value) const;
        [[nodiscard]] std::optional<std::string> get(const std::string &key) const;

        // Sets key to value, expiring after ttl (whole seconds), only when key doesn't exist yet
        // (SET NX EX), atomically: of several clients racing for the key, exactly one gets true.
        // Returns false, changing nothing, when the key already exists. Throws
        // std::invalid_argument for a ttl below 1 s.
        bool set_if_absent(const std::string &key, const std::string &value, std::chrono::seconds ttl) const;

        void hset(const std::string &key, const std::string &field, const std::string &value) const;
        [[nodiscard]] std::optional<std::string> hget(const std::string &key, const std::string &field) const;

        // The values of fields, in the same order; nullopt for a field (or key) that doesn't exist.
        [[nodiscard]] std::vector<std::optional<std::string>> hmget(const std::string &key,
                                                                    const std::vector<std::string> &fields) const;

        // Every field of the hash with its value; empty when the key doesn't exist.
        [[nodiscard]] std::map<std::string, std::string> hgetall(const std::string &key) const;

        // Sets. sadd and srem return how many members were added or removed - members already
        // there (or not there) don't count. A key that doesn't exist is an empty set. They take one
        // member, a braced list ({"a", "b"}, which would otherwise also fit a std::string) or a
        // vector.
        std::size_t sadd(const std::string &key, const std::string &member) const;
        std::size_t sadd(const std::string &key, std::initializer_list<std::string> members) const;
        std::size_t sadd(const std::string &key, const std::vector<std::string> &members) const;
        std::size_t srem(const std::string &key, const std::string &member) const;
        std::size_t srem(const std::string &key, std::initializer_list<std::string> members) const;
        std::size_t srem(const std::string &key, const std::vector<std::string> &members) const;
        [[nodiscard]] std::size_t scard(const std::string &key) const;
        [[nodiscard]] std::set<std::string> smembers(const std::string &key) const;
        [[nodiscard]] bool sismember(const std::string &key, const std::string &member) const;

        std::size_t erase(const std::string &key) const;

        // The keys matching pattern, a glob such as "drtp:*" (SCAN ... MATCH), in no particular
        // order: of the one server, or of every master of a Redis Cluster. It walks the keyspace
        // count keys a step, so the server isn't held up as with KEYS; a key added or removed
        // during the walk may or may not be in the result.
        [[nodiscard]] std::vector<std::string> keys(const std::string &pattern, std::size_t count = 1000) const;

        // Makes key expire after ttl (EXPIRE; whole seconds). Returns false when there is no such
        // key, so nothing was set to expire.
        bool expire(const std::string &key, std::chrono::seconds ttl) const;

        // Runs a Lua script on the server (EVAL), atomically, with its keys (KEYS[1], ...) and
        // arguments (ARGV[1], ...). In a Redis Cluster every key must be in one hash slot: one
        // key, or keys sharing a {hash tag}; the script runs on the node that holds them. Returns
        // what the script returns: a number or status as text, a string as itself, nil or false
        // as nullopt. An error, or a table (array) result, throws std::runtime_error.
        std::optional<std::string> eval(const std::string &script, const std::vector<std::string> &keys,
                                        const std::vector<std::string> &arguments = {}) const;

        // Redis Cluster's state, as the current seed sees it.
        // The nodes (CLUSTER NODES), in its order. Throws std::runtime_error when the server isn't a
        // cluster.
        [[nodiscard]] std::vector<redis_cluster_node> cluster_nodes() const;
        // CLUSTER INFO's fields: cluster_state ("ok", "fail"), cluster_current_epoch,
        // cluster_slots_assigned, cluster_known_nodes, ...
        [[nodiscard]] std::map<std::string, std::string> cluster_info() const;

        // INFO of one node (a cluster's, or the server), all sections or one ("memory",
        // "replication", "keyspace", "server"). It needn't be a seed: it's connected to as the
        // seeds are, with the same options.
        [[nodiscard]] std::map<std::string, std::string> info(const ip_endpoint &node, const std::string &section = {}) const;

        // CLUSTER FAILOVER on a replica: it and its master swap roles, with no writes lost (the
        // master stops taking them until the replica has caught up). Waits until the node reports
        // itself master (INFO replication role:master), throwing std::runtime_error when Redis
        // refuses or it hasn't within limit.
        void cluster_failover(const ip_endpoint &replica, std::chrono::seconds limit = std::chrono::seconds{30}) const;

    private:
        class implementation;
        std::unique_ptr<implementation> implementation_;
    };
}
