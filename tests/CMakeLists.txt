# lease contract tests. Build against the INTERFACE library target and
# register with CTest. The suite is self-contained: no external deps.

find_package(Threads REQUIRED)

add_executable(access_tests access_tests.cpp)
target_link_libraries(access_tests PRIVATE lease::lease Threads::Threads)

if(MSVC)
    target_compile_options(access_tests PRIVATE /W4)
else()
    target_compile_options(access_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME access_tests COMMAND access_tests)
