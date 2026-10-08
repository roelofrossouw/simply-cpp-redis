# simply-cpp-redis

`sc-redis` provides a small Redis Cluster client based on hiredis. Supply a
single server or one or more Cluster seed nodes to the constructor; it follows
Redis Cluster `MOVED` and `ASK` redirections automatically.

```cpp
sc::redis standalone_cache{"redis.example.com"};

sc::redis cache(std::vector<sc::ip_endpoint>{
    {"redis1.example.com", 6379},
    {"redis2.example.com", 6379},
});
sc::redis authenticated_cache(
    {"redis.example.com", 6380, 0, true, "configured-elsewhere"});
cache.set("key", "value");
const auto value = cache.get("key");

cache.hset("key", "field", "value");
const auto field = cache.hget("key", "field");
```

The `cluster` and `authenticated` CTests are small production integration
tests. They use unique `sc-tmp:simply-cpp-redis:*` keys and remove them on
exit. `authenticated` requires `SC_REDIS_TEST_PASSWORD` in the publishing
environment; it is never stored in the repository. On each build server,
declare it in the root-owned `/etc/simply-cpp/test.env` file. Redis deployment
sources that file immediately before running CTest, so it can also hold future
test-only environment variables.
