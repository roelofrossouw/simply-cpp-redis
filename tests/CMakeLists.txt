set(SC_TEST_LINK_LIBRARIES sc-redis)

add_sc_test(redis LABELS network TIMEOUT 30)
set_tests_properties(test-redis PROPERTIES SKIP_RETURN_CODE 77)
