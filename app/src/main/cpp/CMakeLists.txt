cmake_minimum_required(VERSION 3.22.1)
project("shiftassistant" CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(native-lib SHARED
        native-lib.cpp
        CalibrationEngine.cpp)

find_library(log-lib log)
find_library(android-lib android)

find_package(oboe REQUIRED CONFIG)

target_link_libraries(native-lib
        ${log-lib}
        ${android-lib}
        oboe::oboe)
