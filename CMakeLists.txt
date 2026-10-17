cmake_minimum_required(VERSION 3.22)
project(SpectroCraft VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored copy of the JUCE framework (official repo, master branch)
add_subdirectory(extern/JUCE)

add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
