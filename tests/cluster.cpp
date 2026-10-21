#include <redis.h>
#include <sc_test.h>

#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {
    class key_cleanup {
    public:
        key_cleanup(sc::redis &client, std::vector<std::string> keys) : client_(client), keys_(std::move(keys)) {
        }

        ~key_cleanup() {
            for (const auto &key : keys_) {
                try {
                    client_.erase(key);
                } catch (const std::exception &error) {
                    std::cerr << "Unable to remove temporary Redis key " << key << ": " << error.what() << '\n';
                }
            }
        }

    private:
        sc::redis &client_;
        std::vector<std::string> keys_;
    };
}

int main() {
    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string prefix = "sc-tmp:simply-cpp-redis:cluster:" + suffix;
    const auto first_key = prefix + ":first";
    const auto second_key = prefix + ":second";
    const auto hash_key = prefix + ":hash";
    const auto once_key = prefix + ":once";
    const auto set_key = prefix + ":set";
    const auto script_key = prefix + ":script";

    SECTION("Redis Cluster string and hash values");
    sc::redis client{std::vector<sc::ip_endpoint>{
        {"redis1.1web.co.za", 6379},
        {"redis2.1web.co.za", 6379},
    }};
    key_cleanup cleanup{client, {first_key, second_key, hash_key, set_key, script_key, once_key}};

    client.set(first_key, "first-value");
    client.set(second_key, "second-value");
    CHECK_EQ(client.get(first_key), std::optional<std::string>{"first-value"});
    CHECK_EQ(client.get(second_key), std::optional<std::string>{"second-value"});

    client.hset(hash_key, "name", "simply-cpp");
    client.hset(hash_key, "kind", "integration-test");
    CHECK_EQ(client.hget(hash_key, "name"), std::optional<std::string>{"simply-cpp"});
    CHECK_EQ(client.hget(hash_key, "kind"), std::optional<std::string>{"integration-test"});

    SECTION("Several hash fields at once");
    const auto values = client.hmget(hash_key, {"kind", "missing", "name"});
    CHECK_EQ(values.size(), std::size_t{3});
    if (values.size() == 3) {
        CHECK_EQ(values[0], std::optional<std::string>{"integration-test"});
        CHECK(!values[1].has_value());
        CHECK_EQ(values[2], std::optional<std::string>{"simply-cpp"});
    }
    CHECK(client.hmget(hash_key, {}).empty());
    CHECK_EQ(client.hmget(prefix + ":no-such-hash", {"name"}).size(), std::size_t{1});

    const auto all = client.hgetall(hash_key);
    CHECK((all == std::map<std::string, std::string>{{"kind", "integration-test"}, {"name", "simply-cpp"}}));
    CHECK(client.hgetall(prefix + ":no-such-hash").empty());

    SECTION("Sets");
    CHECK_EQ(client.sadd(set_key, "red"), std::size_t{1});
    CHECK_EQ(client.sadd(set_key, std::vector<std::string>{"green", "blue", "red"}), std::size_t{2}); // red was there
    CHECK_EQ(client.sadd(set_key, std::vector<std::string>{}), std::size_t{0});
    CHECK_EQ(client.sadd(set_key, {"red", "green"}), std::size_t{0}); // two literals: a list, not a string
    CHECK_EQ(client.scard(set_key), std::size_t{3});
    CHECK((client.smembers(set_key) == std::set<std::string>{"blue", "green", "red"}));
    CHECK(client.sismember(set_key, "green"));
    CHECK(!client.sismember(set_key, "purple"));
    CHECK_EQ(client.srem(set_key, {"green", "purple"}), std::size_t{1}); // purple wasn't
    CHECK_EQ(client.srem(set_key, "blue"), std::size_t{1});
    CHECK((client.smembers(set_key) == std::set<std::string>{"red"}));
    CHECK_EQ(client.scard(prefix + ":no-such-set"), std::size_t{0});
    CHECK(client.smembers(prefix + ":no-such-set").empty());
    CHECK(!client.sismember(prefix + ":no-such-set", "red"));
    bool wrong_type = false;
    try {
        (void) client.scard(hash_key); // a hash is not a set: WRONGTYPE
    } catch (const sc::redis_unavailable &) {
    } catch (const std::runtime_error &) {
        wrong_type = true;
    }
    CHECK(wrong_type);

    SECTION("Keys that expire");
    {
        const auto expiring = prefix + ":expiring";
        client.set(expiring, "soon gone");
        CHECK(client.expire(expiring, std::chrono::seconds{1}));
        CHECK_EQ(client.get(expiring), std::optional<std::string>{"soon gone"});
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        CHECK(!client.get(expiring).has_value());
        CHECK(!client.expire(prefix + ":no-such-key", std::chrono::seconds{10})); // nothing to expire
        client.hset(script_key, "kept", "for now");
        CHECK(client.expire(script_key, std::chrono::hours{1})); // a hash expires as a whole
    }

    SECTION("Lua scripts with eval()");
    {
        // Writes the value only when its time is newer than the stored one, atomically.
        const std::string newer_only = R"(
            local stored = tonumber(redis.call('HGET', KEYS[1], 'time') or '0')
            if tonumber(ARGV[1]) <= stored then return 0 end
            redis.call('HSET', KEYS[1], 'time', ARGV[1], 'value', ARGV[2])
            return 1)";
        CHECK_EQ(client.eval(newer_only, {script_key}, {"200", "second"}), std::optional<std::string>{"1"});
        CHECK_EQ(client.eval(newer_only, {script_key}, {"100", "first"}), std::optional<std::string>{"0"}); // older: kept
        CHECK_EQ(client.hget(script_key, "value"), std::optional<std::string>{"second"});
        CHECK_EQ(client.eval(newer_only, {script_key}, {"300", "third"}), std::optional<std::string>{"1"});
        CHECK_EQ(client.hget(script_key, "value"), std::optional<std::string>{"third"});

        CHECK_EQ(client.eval("return ARGV[1]", {}, {"text"}), std::optional<std::string>{"text"});
        CHECK(!client.eval("return nil", {}).has_value());
        CHECK(!client.eval("return false", {}).has_value());
        CHECK_EQ(client.eval("return redis.status_reply('FINE')", {}), std::optional<std::string>{"FINE"});
        CHECK_THROWS_AS((void) client.eval("return {1, 2}", {}), std::runtime_error);
        CHECK_THROWS_AS((void) client.eval("this is not lua", {}), std::runtime_error);
        CHECK_THROWS_AS((void) client.eval("return redis.error_reply('nope')", {}), std::runtime_error);
    }

    SECTION("Set if absent, with an expiry");
    {
        CHECK(client.set_if_absent(once_key, "first", std::chrono::seconds{1}));
        CHECK(!client.set_if_absent(once_key, "second", std::chrono::seconds{1}));
        CHECK_EQ(client.get(once_key), std::optional<std::string>{"first"});
        std::this_thread::sleep_for(std::chrono::milliseconds{1500});
        CHECK(!client.get(once_key).has_value()); // expired
        CHECK(client.set_if_absent(once_key, "again", std::chrono::seconds{60})); // absent again
        CHECK_THROWS_AS((void) client.set_if_absent(once_key, "x", std::chrono::seconds{0}), std::invalid_argument);
    }

    TEST_SUMMARY();
}
