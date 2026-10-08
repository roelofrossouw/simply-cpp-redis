// Writes and reads back a string and a hash, then removes them.
// Usage: sc-redis-demo [host[:port] | [ipv6]:port ...]   (default 127.0.0.1:6379)
// Pass a single server, or one or more Redis Cluster seed nodes.

#include <redis.h>
#include <timer.h>

#include <iostream>
#include <string>
#include <vector>

int main(int argc, char *argv[]) {
    const std::string key = "sc-tmp:sc-redis-demo:string";
    const std::string hash = "sc-tmp:sc-redis-demo:hash";

    try {
        std::vector<sc::ip_endpoint> seeds;
        for (int i = 1; i < argc; ++i) seeds.push_back(sc::ip_endpoint::parse(argv[i], 6379));
        if (seeds.empty()) seeds.push_back({"127.0.0.1", 6379});

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
