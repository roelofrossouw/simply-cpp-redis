#include <redis.h>
#include <sc_test.h>

#include <chrono>
#include <iostream>
#include <set>
#include <string>
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
    const auto set_key = prefix + ":set";

    SECTION("Redis Cluster string and hash values");
    sc::redis client{std::vector<sc::ip_endpoint>{
        {"redis1.1web.co.za", 6379},
        {"redis2.1web.co.za", 6379},
    }};
    key_cleanup cleanup{client, {first_key, second_key, hash_key, set_key}};

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

    TEST_SUMMARY();
}
