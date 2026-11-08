cmake_minimum_required(VERSION 3.21)
project(CustomModelsConverterTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_library(vivify_converter STATIC
    ../src/vivify/VivifyBundleConvert.cpp
    ../src/vivify/VivifyDxbc.cpp
    ../src/vivify/VivifySerializedFile.cpp
    ../src/vivify/VivifyTextureDecode.cpp
)
target_include_directories(vivify_converter PUBLIC ../include/vivify)
target_compile_options(vivify_converter PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

add_executable(vivify_converter_tests converter_tests.cpp)
target_link_libraries(vivify_converter_tests PRIVATE vivify_converter)
add_test(NAME vivify_converter_unit COMMAND vivify_converter_tests)

add_executable(vivify_asset_smoke asset_smoke.cpp)
target_link_libraries(vivify_asset_smoke PRIVATE vivify_converter)
