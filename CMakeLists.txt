cmake_minimum_required(VERSION 3.22)
set(SC_MODULE redis)
project(simply-cpp-${SC_MODULE} LANGUAGES CXX)
include(cmake/sc_bootstrap.cmake)

option(SC_REDIS_BUILD_TESTS "Build simply-cpp-redis tests" ${PROJECT_IS_TOP_LEVEL})

get_sc_version()
message(STATUS "SimplyCPP ${SC_MODULE} Version: ${SC_VERSION}")

find_or_install_package(sc-core simply-cpp-dev simply-cpp 1.5.0)
config_or_install_package(hiredis libhiredis-dev hiredis)
find_library(SC_REDIS_HIREDIS_LIBRARY NAMES hiredis HINTS ${hiredis_LIBRARY_DIRS} REQUIRED)

set(SOURCE_OBJECTS include/redis.h)
set(SOURCE_LIBRARIES)
set(SOURCE_LINK_LIBRARIES)

add_sc_object(redis
        LINK_LIBRARIES PkgConfig::hiredis
        PUBLIC_LINK_LIBRARIES sc::sc-core ${SC_REDIS_HIREDIS_LIBRARY})

add_sc_libraries(DESCRIPTION "simply-cpp Redis Cluster client")
target_compile_features(sc-redis PUBLIC cxx_std_17)
target_compile_features(sc-redis-shared PUBLIC cxx_std_17)

install_sc_module(PATH_VARS CMAKE_INSTALL_INCLUDEDIR)
install(FILES cmake/SimplyCppFunctions.cmake cmake/sc_bootstrap.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sc-redis
        COMPONENT development)
install(FILES tests/sc_test.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        COMPONENT development)

if (SC_REDIS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests sc-redis-test)
endif ()

add_sc_example(sc-redis-demo RUN_AS_TEST ARGS redis1.1web.co.za redis2.1web.co.za)

package_sc_module(DEV_DEPENDS "simply-cpp-dev (>= 1.5.0)" libhiredis-dev)
