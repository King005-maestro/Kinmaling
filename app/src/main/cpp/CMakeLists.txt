cmake_minimum_required(VERSION 3.22)
project(maliemu LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
add_library(maliemu SHARED
  jni_bridge.cpp vk_context.cpp mali_allocator.cpp
  cpu_topology.cpp box64_launcher.cpp frame_bridge.cpp)
target_compile_options(maliemu PRIVATE -O3 -fno-exceptions -DVK_USE_PLATFORM_ANDROID_KHR)
target_link_libraries(maliemu vulkan android nativewindow log)
