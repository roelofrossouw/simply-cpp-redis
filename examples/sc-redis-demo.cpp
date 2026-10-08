// Writes and reads back a string and a hash, then removes them.
// Servers come from SC_REDIS_DEMO_SERVER: one server, or Redis Cluster seed nodes separated
// by ';' ("redis1:6379;redis2:6379"). Unset or invalid falls back to 127.0.0.1:6379.

#include <demo_servers.h>
#include <redis.h>
#include <timer.h>

#include <iostream>
#include <string>

int main() {
    const std::string key = "sc-tmp:sc-redis-demo:string";
    const std::string hash = "sc-tmp:sc-redis-demo:hash";

    try {
        const auto seeds = sc::demo_servers("SC_REDIS_DEMO_SERVER", 6379);
        std::cout << "Redis servers:";
        for (const auto &seed : seeds) std::cout << ' ' << seed;
        std::cout << '\n';

        // [readme]
        sc::timer sw;
        sc::redis cache{seeds};
        std::cout << "Connected after " << sw << '\n';

        cache.set(key, "Hello World!");
        std::cout << key << " = " << cache.get(key).value() << '\n';

        cache.hset(hash, "name", "simply-cpp");
        cache.hset(hash, "kind", "demo");
        std::cout << hash << " name = " << cache.hget(hash, "name").value()
                << ", kind = " << cache.hget(hash, "kind").value() << '\n';

        cache.erase(key);
        cache.erase(hash);
        std::cout << "Done after " << sw << '\n';
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-redis-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
