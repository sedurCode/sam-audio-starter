cmake_minimum_required(VERSION 3.16)
project(nupc_host_test CXX)

# Native (host) build of the NUPC engine + bare-metal schedulers against a reference FFT shim.
#   cmake -S test/host -B out/host-test -DCMAKE_BUILD_TYPE=Release && cmake --build out/host-test && out/host-test/test_nupc
#   add -DNUPC_TSAN=ON for ThreadSanitizer
set(NUPC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../ARM/src/nupc)
add_executable(test_nupc test_nupc.cpp
    ${NUPC_DIR}/NupcPlatform.cpp
    ${NUPC_DIR}/BareMetalTaskScheduler.cpp
    ${NUPC_DIR}/NeuralNonUniformPartitionedConvolution.cpp)
target_include_directories(test_nupc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/shim ${NUPC_DIR})
target_compile_definitions(test_nupc PRIVATE NUPC_BARE_METAL BUFFER_SIZE_SAMPLES=64)
target_compile_features(test_nupc PRIVATE cxx_std_11)
target_compile_options(test_nupc PRIVATE -Wall -Wextra -Wno-unused-parameter)
find_package(Threads REQUIRED)
target_link_libraries(test_nupc PRIVATE Threads::Threads)
option(NUPC_TSAN "ThreadSanitizer" OFF)
if(NUPC_TSAN)
    target_compile_options(test_nupc PRIVATE -fsanitize=thread -g)
    target_link_options(test_nupc PRIVATE -fsanitize=thread)
endif()
