add_executable(disk_loader disk_loader.cpp)
target_include_directories(disk_loader PRIVATE ../lib)
target_compile_definitions(disk_loader PRIVATE USE_VTPC)
target_link_libraries(disk_loader PRIVATE vtpc)

add_executable(disk_loader_std disk_loader.cpp)
target_include_directories(disk_loader_std PRIVATE ../lib)
# No USE_VTPC definition
target_link_libraries(disk_loader_std PRIVATE vtpc) # It needs vtpc headers? No, but let's keep it simple. It includes vtpc.h inside extern C.
