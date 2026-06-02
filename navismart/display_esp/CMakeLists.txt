cmake_minimum_required(VERSION 3.16)
project(display_esp LANGUAGES CXX)

add_executable(display_esp
    main.cpp
    screen_start.cpp
    screen_race.cpp
    writer.cpp
)

target_include_directories(display_esp PRIVATE
    .
)
