#include <redis.h>
#include <sc_test.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
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
    const auto *password = std::getenv("SC_REDIS_TEST_PASSWORD");
    if (!password || !*password) {
        std::cerr << "SC_REDIS_TEST_PASSWORD is required for the authenticated Redis integration test\n";
        return EXIT_FAILURE;
    }

    const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string prefix = "sc-tmp:simply-cpp-redis:authenticated:" + suffix;
    const auto first_key = prefix + ":first";
    const auto second_key = prefix + ":second";
    const auto hash_key = prefix + ":hash";

    SECTION("Authenticated Redis string and hash values");
    sc::redis client{sc::redis_connection{"vms", 20006, 0, true, password}};
    key_cleanup cleanup{client, {first_key, second_key, hash_key}};

    client.set(first_key, "first-value");
    client.set(second_key, "second-value");
    CHECK_EQ(client.get(first_key), std::optional<std::string>{"first-value"});
    CHECK_EQ(client.get(second_key), std::optional<std::string>{"second-value"});

    client.hset(hash_key, "name", "simply-cpp");
    client.hset(hash_key, "kind", "integration-test");
    CHECK_EQ(client.hget(hash_key, "name"), std::optional<std::string>{"simply-cpp"});
    CHECK_EQ(client.hget(hash_key, "kind"), std::optional<std::string>{"integration-test"});

    TEST_SUMMARY();
}
