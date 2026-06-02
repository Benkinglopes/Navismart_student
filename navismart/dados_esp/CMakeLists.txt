cmake_minimum_required(VERSION 3.16)
project(dados_esp LANGUAGES CXX)

add_executable(dados_esp
    main.cpp
    acquisition.cpp
    treatment.cpp
    comms.cpp
)

target_include_directories(dados_esp PRIVATE
    .
    ../display_esp
)
