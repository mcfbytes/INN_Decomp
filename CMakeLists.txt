cmake_minimum_required(VERSION 3.20)
project(inn_decomp C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND EMSCRIPTEN)
  set(CMAKE_BUILD_TYPE Release) # -O3; debug info would multiply the wasm size
elseif(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# The machine and its services; frontends live in runtime/frontend.
file(GLOB DOSRT_SOURCES CONFIGURE_DEPENDS runtime/src/*.c)
add_library(dosrt STATIC ${DOSRT_SOURCES})
target_include_directories(dosrt PUBLIC runtime/src)

# Lifted (hand-decompiled) sources live in the private tree; the public build runs without them.
set(INN_LIFTED_DIR "${CMAKE_SOURCE_DIR}/private/lifted" CACHE PATH "Lifted sources (private tree)")
if(EXISTS "${INN_LIFTED_DIR}")
  file(GLOB_RECURSE INN_LIFTED_SOURCES FOLLOW_SYMLINKS CONFIGURE_DEPENDS "${INN_LIFTED_DIR}/*.c")
  file(GLOB_RECURSE INN_LIFTED_MODULES FOLLOW_SYMLINKS CONFIGURE_DEPENDS "${INN_LIFTED_DIR}/*/module.txt")
  set(INN_LIFTED_TABLE "${CMAKE_BINARY_DIR}/lifted_table.c")
  add_custom_command(OUTPUT "${INN_LIFTED_TABLE}"
    COMMAND python3 -I "${CMAKE_SOURCE_DIR}/tools/gen_lift_table.py" "${INN_LIFTED_DIR}" "${INN_LIFTED_TABLE}"
    DEPENDS ${INN_LIFTED_SOURCES} ${INN_LIFTED_MODULES} "${CMAKE_SOURCE_DIR}/tools/gen_lift_table.py")
  target_sources(dosrt PRIVATE ${INN_LIFTED_SOURCES} "${INN_LIFTED_TABLE}")
  target_include_directories(dosrt PRIVATE "${INN_LIFTED_DIR}/include")
else()
  set(INN_LIFTED_TABLE "${CMAKE_BINARY_DIR}/lifted_table.c")
  file(WRITE "${INN_LIFTED_TABLE}" "#include \"lift.h\"\nvoid lifted_register_all(Machine *m) { (void)m; }\n")
  target_sources(dosrt PRIVATE "${INN_LIFTED_TABLE}")
endif()

# Vendored third-party code (docs/third_party.md) is kept untouched, so its warnings are too.
set(NUKED_OPL3 third_party/nuked-opl3/opl3.c)
target_sources(dosrt PRIVATE ${NUKED_OPL3})
target_include_directories(dosrt PRIVATE third_party/nuked-opl3)
set_source_files_properties(${NUKED_OPL3} PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(dosrt PUBLIC m)

set(INN_FRONTEND_SOURCES runtime/frontend/frontend_sdl.c runtime/frontend/frame_clock.c
    runtime/frontend/input_script.c runtime/frontend/audio_sdl.c runtime/frontend/host_rules.c)

if(EMSCRIPTEN)
  # Browser build (docs/web.md). The preload directory bakes a local C: drive into inn.data for
  # development; leave it empty for anything published, since that drive holds the user's media.
  set(INN_WEB_PRELOAD_DIR "" CACHE PATH "Local C: drive directory to package as /c (dev only)")
  add_executable(inn ${INN_FRONTEND_SOURCES})
  set_target_properties(inn PROPERTIES SUFFIX ".html")
  target_link_libraries(inn dosrt)
  target_compile_options(inn PRIVATE -sUSE_SDL=2)
  # Lifted code can wait inside a frame (nested guest calls) and must yield to the browser there.
  # JSPI suspends without instrumenting code (Chromium-based browsers); Asyncify works everywhere
  # but instruments the call path. With Asyncify, indirect calls are ignored, so the functions that
  # reach a sleep through one are listed; the interpreter loop is not on that path (lifted code is
  # dispatched from machine_step, into which lift_dispatch may be inlined, so both are named).
  set(INN_WEB_SUSPEND "ASYNCIFY" CACHE STRING "How lifted code yields to the browser: ASYNCIFY or JSPI")
  target_compile_definitions(inn PRIVATE INN_ASYNCIFY=1)
  target_link_options(inn PRIVATE -sSTACK_SIZE=1048576)
  if(INN_WEB_SUSPEND STREQUAL "JSPI")
    target_link_options(inn PRIVATE -sJSPI)
  else()
    target_link_options(inn PRIVATE -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 -sASYNCIFY_STACK_SIZE=262144
      "-sASYNCIFY_ADD=['lift_run_nested','lift_dispatch','machine_step','frontend_nested_yield']")
  endif()
  target_link_options(inn PRIVATE -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web
    -sEXPORTED_RUNTIME_METHODS=callMain,FS -lidbfs.js -lwebsocket.js
    --shell-file ${CMAKE_SOURCE_DIR}/runtime/web/shell.html)
  if(INN_WEB_PRELOAD_DIR)
    get_filename_component(INN_WEB_PRELOAD_ABS "${INN_WEB_PRELOAD_DIR}" ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
    message(STATUS "inn: packaging ${INN_WEB_PRELOAD_ABS} as /c in inn.data (do not publish)")
    target_link_options(inn PRIVATE --preload-file ${INN_WEB_PRELOAD_ABS}@/c)
  endif()
  set_property(TARGET inn APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/web/shell.html)
  foreach(script page.js zip.js)
    configure_file(runtime/web/${script} ${CMAKE_BINARY_DIR}/${script} COPYONLY)
  endforeach()
else()
  find_package(SDL2 REQUIRED)
  add_executable(inn ${INN_FRONTEND_SOURCES})
  target_link_libraries(inn dosrt SDL2::SDL2)
  enable_testing()
  file(GLOB DOSRT_TESTS CONFIGURE_DEPENDS runtime/tests/test_*.c)
  foreach(test_src ${DOSRT_TESTS})
    get_filename_component(test_name ${test_src} NAME_WE)
    add_executable(${test_name} ${test_src})
    target_link_libraries(${test_name} dosrt)
    add_test(NAME ${test_name} COMMAND ${test_name})
  endforeach()
endif()
