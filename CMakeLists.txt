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

# Build configurations (docs/native.md section 10). INN_NATIVE_HW: drivers call the host APIs;
# INN_NATIVE: the native INN client, which implies it.
option(INN_NATIVE "Native INN client: no CPU interpretation while INN runs" OFF)
option(INN_NATIVE_HW "Lifted drivers reach devices through the host APIs" OFF)
if(INN_NATIVE)
  set(INN_NATIVE_HW ON)
endif()
if(EMSCRIPTEN)
  if(INN_NATIVE)
    set(INN_WEB_SUSPEND "BOTH" CACHE STRING "How the native client yields: BOTH, JSPI or ASYNCIFY")
    if(NOT INN_WEB_SUSPEND MATCHES "^(BOTH|JSPI|ASYNCIFY)$")
      message(FATAL_ERROR "INN_NATIVE in the browser needs -DINN_WEB_SUSPEND=BOTH, JSPI or ASYNCIFY")
    endif()
    # The native C stack is suspended whole (JSPI) and unwound by longjmp at process exit; JavaScript
    # longjmp would put invoke_* frames on it, which JSPI cannot suspend across. Asyncify instruments
    # the same objects with the legacy wasm exception encoding.
    add_compile_options(-fwasm-exceptions -sSUPPORT_LONGJMP=wasm)
    add_link_options(-fwasm-exceptions -sSUPPORT_LONGJMP=wasm -sWASM_LEGACY_EXCEPTIONS=1)
  else()
    set(INN_WEB_SUSPEND "ASYNCIFY" CACHE STRING "How lifted code yields to the browser: ASYNCIFY or JSPI")
  endif()
endif()

# The machine and its services; frontends live in runtime/frontend. The native core is compiled in
# every configuration so hybrid builds keep it building; only INN_NATIVE boots through it.
file(GLOB DOSRT_SOURCES CONFIGURE_DEPENDS runtime/src/*.c runtime/src/native/*.c)
add_library(dosrt STATIC ${DOSRT_SOURCES})
target_include_directories(dosrt PUBLIC runtime/src runtime/src/native)
if(INN_NATIVE)
  target_compile_definitions(dosrt PUBLIC INN_NATIVE=1 INN_NATIVE_HW=1)
elseif(INN_NATIVE_HW)
  target_compile_definitions(dosrt PUBLIC INN_NATIVE_HW=1)
endif()

# Lifted (hand-decompiled) sources live in the private tree; the public build runs without them.
set(INN_LIFTED_DIR "${CMAKE_SOURCE_DIR}/private/lifted" CACHE PATH "Lifted sources (private tree)")
if(EXISTS "${INN_LIFTED_DIR}")
  file(GLOB_RECURSE INN_LIFTED_SOURCES FOLLOW_SYMLINKS CONFIGURE_DEPENDS "${INN_LIFTED_DIR}/*.c")
  # Matching-decompilation cases are standalone comparisons, not runtime lifts.
  list(FILTER INN_LIFTED_SOURCES EXCLUDE REGEX "/matching/")
  file(GLOB_RECURSE INN_LIFTED_MODULES FOLLOW_SYMLINKS CONFIGURE_DEPENDS "${INN_LIFTED_DIR}/*/module.txt")
  set(INN_LIFTED_TABLE "${CMAKE_BINARY_DIR}/lifted_table.c")
  set(INN_LIFTED_ENTRIES "${CMAKE_BINARY_DIR}/lifted_entries.h")
  add_custom_command(OUTPUT "${INN_LIFTED_TABLE}" "${INN_LIFTED_ENTRIES}"
    COMMAND python3 -I "${CMAKE_SOURCE_DIR}/tools/gen_lift_table.py" "${INN_LIFTED_DIR}" "${INN_LIFTED_TABLE}"
    DEPENDS ${INN_LIFTED_SOURCES} ${INN_LIFTED_MODULES} "${CMAKE_SOURCE_DIR}/tools/gen_lift_table.py")
  target_sources(dosrt PRIVATE ${INN_LIFTED_SOURCES} "${INN_LIFTED_TABLE}")
  target_include_directories(dosrt PRIVATE "${INN_LIFTED_DIR}/include" "${CMAKE_BINARY_DIR}")
  # The header is produced with the table; lifted sources include it through lift_calls.h.
  set_source_files_properties(${INN_LIFTED_SOURCES} PROPERTIES OBJECT_DEPENDS "${INN_LIFTED_ENTRIES}")
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
  # Lifted code can wait inside a frame (nested guest calls) and must yield to the browser there.
  # JSPI suspends without instrumenting code (Chromium-based browsers); Asyncify works everywhere but
  # instruments the call path. Native builds link both from the same objects; the page picks at load.
  set(INN_WEB_ASYNCIFY_JS "")
  set(INN_WEB_RUNTIME lazy)
  if(INN_WEB_PRELOAD_DIR)
    set(INN_WEB_RUNTIME eager)
  endif()
  configure_file(runtime/web/shell.html ${CMAKE_BINARY_DIR}/shell.html @ONLY)

  function(inn_web_target target suffix)
    add_executable(${target} ${INN_FRONTEND_SOURCES})
    set_target_properties(${target} PROPERTIES SUFFIX "${suffix}")
    target_link_libraries(${target} dosrt)
    target_compile_options(${target} PRIVATE -sUSE_SDL=2)
    target_compile_definitions(${target} PRIVATE INN_ASYNCIFY=1)
    target_link_options(${target} PRIVATE -sSTACK_SIZE=1048576 -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1
      -sINITIAL_MEMORY=64MB -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web
      -sEXPORTED_RUNTIME_METHODS=callMain,FS -lidbfs.js -lwebsocket.js)
    if(suffix STREQUAL ".html")
      target_link_options(${target} PRIVATE --shell-file ${CMAKE_BINARY_DIR}/shell.html)
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CMAKE_BINARY_DIR}/shell.html)
    endif()
    if(target STREQUAL "inn-asyncify")
      target_link_options(${target} PRIVATE -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144)
    elseif(INN_WEB_SUSPEND STREQUAL "JSPI" OR INN_WEB_SUSPEND STREQUAL "BOTH")
      target_link_options(${target} PRIVATE -sJSPI)
    elseif(INN_WEB_SUSPEND STREQUAL "ASYNCIFY" AND INN_NATIVE)
      target_link_options(${target} PRIVATE -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144)
    else()
      target_link_options(${target} PRIVATE -sASYNCIFY -sASYNCIFY_IGNORE_INDIRECT=1 -sASYNCIFY_STACK_SIZE=262144
        "-sASYNCIFY_ADD=['lift_run_nested','lift_dispatch','machine_step','frontend_nested_yield']")
    endif()
    if(INN_WEB_PRELOAD_DIR)
      get_filename_component(INN_WEB_PRELOAD_ABS "${INN_WEB_PRELOAD_DIR}" ABSOLUTE BASE_DIR ${CMAKE_SOURCE_DIR})
      message(STATUS "${target}: packaging ${INN_WEB_PRELOAD_ABS} as /c in inn.data (do not publish)")
      target_link_options(${target} PRIVATE --preload-file ${INN_WEB_PRELOAD_ABS}@/c)
    endif()
    if(INN_NATIVE)
      target_link_options(${target} PRIVATE --pre-js ${CMAKE_SOURCE_DIR}/runtime/web/native-pre.js)
      set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/web/native-pre.js)
    endif()
  endfunction()

  if(INN_NATIVE AND INN_WEB_SUSPEND STREQUAL "BOTH")
    inn_web_target(inn ".html")
    set(INN_WEB_ASYNCIFY_JS "inn-asyncify.js")
    configure_file(runtime/web/shell.html ${CMAKE_BINARY_DIR}/shell.html @ONLY)
    inn_web_target(inn-asyncify ".js")
  else()
    inn_web_target(inn ".html")
  endif()
  foreach(script page.js site.js site.json og.png icons/favicon.ico icons/icon.svg icons/apple-touch-icon.png icons/icon-192.png icons/icon-512.png icons/manifest.webmanifest telemetry.js audio-worklet.js legal/terms.html legal/privacy.html legal/copyright.html legal/legal.js zip.js saves.js auth.js signin.js music.js filters.js adv.js scene.js
          scenes/valley.js scenes/diner.js scenes/tower.js scenes/harbor.js scenes/town.js
          music/valley.js music/diner.js music/tower.js music/harbor.js music/town.js)
    configure_file(runtime/web/${script} ${CMAKE_BINARY_DIR}/${script} COPYONLY)
  endforeach()
  # The page font (third_party/vt323/, OFL 1.1): the pages' @font-face names fonts/VT323-Regular.ttf.
  configure_file(third_party/vt323/VT323-Regular.ttf ${CMAKE_BINARY_DIR}/fonts/VT323-Regular.ttf COPYONLY)
  # The display filters' shaders (third_party/, docs/web.md "Display filters") sit flat in <build>/shaders/;
  # the page fetches them when a filter is first chosen.
  foreach(dir xbr xbrz scalefx hqx fsr1 crt-lottes anime4k sai)
    file(GLOB shader_files CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/${dir}/*.glsl ${CMAKE_SOURCE_DIR}/third_party/${dir}/*.png)
    foreach(shader ${shader_files})
      get_filename_component(shader_name ${shader} NAME)
      configure_file(${shader} ${CMAKE_BINARY_DIR}/shaders/${shader_name} COPYONLY)
    endforeach()
  endforeach()
  # `cmake --build <dir> --target package-web` writes the deployable page to <dir>/package (docs/web.md,
  # "Packaging and deployment"). The script refuses lifted, native and preloaded builds. Pass the operator's
  # origins with -DINN_PACKAGE_ARGS="--game;wss://host:port;--auth;https://host".
  set(INN_PACKAGE_ARGS "" CACHE STRING "Extra arguments for tools/package_web.sh (--game URL, --auth URL, --connect-any)")
  add_custom_target(package-web COMMAND ${CMAKE_SOURCE_DIR}/tools/package_web.sh ${CMAKE_BINARY_DIR} ${CMAKE_BINARY_DIR}/package ${INN_PACKAGE_ARGS}
    DEPENDS inn VERBATIM USES_TERMINAL)
  if(CMAKE_CROSSCOMPILING_EMULATOR)
    set(INN_NODE ${CMAKE_CROSSCOMPILING_EMULATOR})
  else()
    find_program(INN_NODE NAMES node nodejs)
  endif()
  enable_testing()
  # The host PCM and the Sound Blaster model in wasm: test_host_audio under Node.
  add_executable(test_host_audio runtime/tests/test_host_audio.c)
  target_link_libraries(test_host_audio dosrt)
  target_link_options(test_host_audio PRIVATE -sENVIRONMENT=node -sEXIT_RUNTIME=1
    -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1048576 -lwebsocket.js)
  if(INN_NODE)
    add_test(NAME test_host_audio COMMAND ${INN_NODE} ${CMAKE_BINARY_DIR}/test_host_audio.js)
  endif()
  if(INN_NATIVE)
    # The JSPI proof (docs/native.md section 4): test_native under Node, suspending inside setjmp.
    add_executable(test_native runtime/tests/test_native.c)
    target_link_libraries(test_native dosrt)
    target_link_options(test_native PRIVATE -sJSPI -sENVIRONMENT=node -sEXIT_RUNTIME=1
      -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB -sSTACK_SIZE=1048576 -lwebsocket.js)
    if(NOT INN_NODE)
      message(FATAL_ERROR "INN_NATIVE in the browser needs node for test_native")
    endif()
    add_test(NAME test_native COMMAND ${INN_NODE} --experimental-wasm-jspi ${CMAKE_BINARY_DIR}/test_native.js)
    add_executable(test_native_asyncify runtime/tests/test_native.c)
    target_link_libraries(test_native_asyncify dosrt)
    target_compile_definitions(test_native_asyncify PRIVATE INN_TEST_ASYNCIFY=1)
    target_link_options(test_native_asyncify PRIVATE -sASYNCIFY -sASYNCIFY_STACK_SIZE=262144
      -sENVIRONMENT=node -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=64MB
      -sSTACK_SIZE=1048576 -lwebsocket.js)
    add_test(NAME test_native_asyncify COMMAND ${INN_NODE} ${CMAKE_BINARY_DIR}/test_native_asyncify.js)
  endif()
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
  target_sources(test_input_script PRIVATE runtime/frontend/input_script.c)
  target_include_directories(test_input_script PRIVATE runtime/frontend)
  # The rule parser lives in the frontend; the test supplies its own exec_stub stand-ins.
  target_sources(test_host_rules PRIVATE runtime/frontend/host_rules.c)
  target_include_directories(test_host_rules PRIVATE runtime/frontend)
endif()
