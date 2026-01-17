add_library(
    vtpc
    SHARED
    vtpc.c
)

target_include_directories(
    vtpc
    PUBLIC
    .
)
