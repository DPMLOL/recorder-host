cmake_minimum_required(VERSION 3.28)
project(recorder-host LANGUAGES CXX)

# Built by scripts/build.cmd: obs-studio is configured, built and installed into build/obs-install first.
set(OBS_INSTALL_DIR "${CMAKE_SOURCE_DIR}/build/obs-install" CACHE PATH "OBS install prefix")
file(GLOB OBS_DEPS_DIR "${CMAKE_SOURCE_DIR}/obs-studio/.deps/obs-deps-20*-x64")
list(APPEND CMAKE_PREFIX_PATH "${OBS_INSTALL_DIR}/cmake" ${OBS_DEPS_DIR})
find_package(libobs REQUIRED CONFIG)

add_executable(recorder-host
  src/main.cpp
  src/protocol.cpp
  src/recorder.cpp
  src/wall_clock.cpp
)
target_compile_features(recorder-host PRIVATE cxx_std_20)
target_compile_definitions(recorder-host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(recorder-host PRIVATE OBS::libobs)

# The host must sit next to obs.dll so libobs finds its plugins and data through its default relative paths.
install(TARGETS recorder-host RUNTIME DESTINATION bin/64bit)
install(FILES $<TARGET_PDB_FILE:recorder-host> DESTINATION bin/64bit OPTIONAL)
