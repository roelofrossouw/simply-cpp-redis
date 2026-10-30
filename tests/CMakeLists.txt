set(SC_TEST_LINK_LIBRARIES sc-redis)

add_sc_test(cluster TIMEOUT 90)
add_sc_test(authenticated TIMEOUT 30)
add_sc_test(connection LABELS unit TIMEOUT 30)
add_sc_test(parse LABELS unit TIMEOUT 30)
