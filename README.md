# simply-cpp-redis

`sc-redis` provides a small Redis Cluster client based on hiredis. Supply a
single server or one or more Cluster seed nodes to the constructor; it follows
Redis Cluster `MOVED` and `ASK` redirections automatically.

```cpp
sc::redis standalone_cache{"redis.example.com"};

sc::redis cache({{"redis1.example.com", 6379}, {"redis2.example.com", 6379}});
cache.set("key", "value");
const auto value = cache.get("key");

cache.hset("key", "field", "value");
const auto field = cache.hget("key", "field");
```

The `redis` CTest is a small production integration test. It uses unique
`sc-tmp:simply-cpp-redis:*` keys and removes them on exit.
