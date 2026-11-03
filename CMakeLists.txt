cmake_minimum_required(VERSION 3.25)
project(instrumentation_qualification_kit VERSION 1.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(IQK_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(IQK_FUZZ "Build libFuzzer targets (clang)" OFF)
option(IQK_TESTS "Build unit tests" ON)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

# Warnings are errors: a warning in this code base is a finding to resolve, not noise.
set(IQK_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor
    -Wold-style-cast -Wcast-align -Wnull-dereference -Wdouble-promotion -Wformat=2 -Werror)

# Hardening (checked on the built binaries by `iqk assurance hardening`).
set(IQK_HARDENING -fstack-protector-strong -fstack-clash-protection -D_FORTIFY_SOURCE=3 -fPIE -fcf-protection=full)
set(IQK_LINK_HARDENING -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack)

# Reproducibility: no build paths or dates in the binaries; the same sources and toolchain give the same bytes.
set(IQK_REPRO -ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=. -Wdate-time)
set(IQK_REPRO_LINK -Wl,--build-id=sha1)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(IQK_HARDENING -fstack-protector-strong -fstack-clash-protection -fPIE -fcf-protection=full)
endif()

add_library(iqk_core STATIC
  cpp/src/framing.cpp
  cpp/src/timesync.cpp
  cpp/src/plan.cpp
  cpp/src/loader.cpp
  cpp/src/drivers.cpp
  cpp/src/dau.cpp
  cpp/src/update.cpp
)
target_include_directories(iqk_core PUBLIC cpp/include cpp/gen)
target_compile_options(iqk_core PRIVATE ${IQK_WARNINGS} ${IQK_HARDENING} ${IQK_REPRO})
target_link_libraries(iqk_core PUBLIC OpenSSL::Crypto)

if(IQK_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
  target_compile_options(iqk_core PRIVATE -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
endif()

foreach(app daud iqkctl)
  add_executable(${app} cpp/apps/${app}.cpp)
  target_link_libraries(${app} PRIVATE iqk_core)
  target_compile_options(${app} PRIVATE ${IQK_WARNINGS} ${IQK_HARDENING} ${IQK_REPRO})
  target_link_options(${app} PRIVATE ${IQK_LINK_HARDENING} ${IQK_REPRO_LINK})
endforeach()

if(IQK_TESTS)
  find_package(GTest REQUIRED)
  enable_testing()
  file(GLOB IQK_TEST_SOURCES CONFIGURE_DEPENDS cpp/tests/*.cpp)
  add_executable(iqk_tests ${IQK_TEST_SOURCES})
  target_link_libraries(iqk_tests PRIVATE iqk_core GTest::gtest GTest::gtest_main)
  target_compile_options(iqk_tests PRIVATE -Wall -Wextra -Werror)
  include(GoogleTest)
  gtest_discover_tests(iqk_tests DISCOVERY_MODE PRE_TEST)
endif()

if(IQK_FUZZ)
  foreach(t deframer plan ptp update)
    add_executable(fuzz_${t} cpp/fuzz/fuzz_${t}.cpp)
    target_link_libraries(fuzz_${t} PRIVATE iqk_core)
    target_compile_options(fuzz_${t} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_${t} PRIVATE -fsanitize=fuzzer,address,undefined)
  endforeach()
  target_compile_options(iqk_core PRIVATE -fsanitize=fuzzer-no-link,address,undefined)
endif()
