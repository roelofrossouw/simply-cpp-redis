set(SC_TEST_LINK_LIBRARIES sc-redis)

add_sc_test(redis LABELS network TIMEOUT 30)
