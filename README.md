# simply-cpp-redis

`sc-redis` provides a small Redis Cluster client based on hiredis. Supply a
single server or one or more Cluster seed nodes to the constructor as an
`sc::ip_endpoints`: a `;`-separated string, a `std::vector<sc::ip_endpoint>` or a
braced list. Entries without a port use 6379. It follows Redis Cluster `MOVED`
and `ASK` redirections automatically.

```cpp
sc::redis standalone_cache{"redis.example.com"};
sc::redis cache{"redis1.example.com;redis2.example.com:6380"};
sc::redis listed_cache({{"redis1.example.com", 6379}, {"redis2.example.com", 6380}});
sc::redis authenticated_cache(
    {"redis.example.com", 6380, 0, true, "configured-elsewhere"});
cache.set("key", "value");
const auto value = cache.get("key");

cache.hset("key", "field", "value");
const auto field = cache.hget("key", "field");
```

## Demo

`sc-redis-demo` is installed with the runtime package (`simply-cpp-redis`), so
you can check a machine can reach Redis without installing the `-dev` package:

```bash
sc-redis-demo                                                      # 127.0.0.1:6379
SC_REDIS_DEMO_SERVER="redis1.example.com;redis2.example.com:6380" sc-redis-demo
```

`SC_REDIS_DEMO_SERVER` holds one server or several Cluster seed nodes,
separated by `;` (quote the value in a shell). When it's unset or invalid the
demo falls back to `127.0.0.1:6379`. The `example-sc-redis-demo` CTest uses the
same variable, which build servers get from `/etc/simply-cpp/test.env`.

Its source is `examples/sc-redis-demo.cpp`; the code below is copied from it at
configure time, so it always matches code that compiles:

<!-- sc-example: examples/sc-redis-demo.cpp -->
```cpp
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
```
<!-- /sc-example -->

## Tests

The `cluster` and `authenticated` CTests are small production integration
tests. They use unique `sc-tmp:simply-cpp-redis:*` keys and remove them on
exit. `authenticated` requires `SC_REDIS_TEST_PASSWORD` in the publishing
environment; it is never stored in the repository. On each build server,
declare it in the root-owned `/etc/simply-cpp/test.env` file. Redis deployment
sources that file immediately before running CTest, so it can also hold future
test-only environment variables.
