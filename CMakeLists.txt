# *-* Mode: cmake; *-*

cmake_minimum_required(VERSION 2.8.5)
project(rr C CXX ASM)

# "Do not add flags to export symbols from executables without the ENABLE_EXPORTS target property."
# This avoids linking executables with -rdynamic. -rdynamic has been observed
# to cause rr_exec_stub to be linked with the dynamic linker with some
# version(s) of clang (but linked to an incorrect file name, causing
# exec of rr_exec_stub to fail).
if(POLICY CMP0065)
  cmake_policy(SET CMP0065 NEW)
endif()

# On single configuration generators, make Debug the default configuration
if(NOT CMAKE_CONFIGURATION_TYPES)
  if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Whether to build in `Debug` or `Release` mode." FORCE)
  endif()
endif()

enable_testing()
set(BUILD_SHARED_LIBS ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib/rr)

set(BUILD_TESTS ON CACHE BOOL "Build tests")
set(WILL_RUN_TESTS ${BUILD_TESTS} CACHE BOOL "Run tests")

# CAREFUL!  "-" is an invalid character in RPM package names, while
# debian is happy with it.  However, "_" is illegal in debs, while RPM
# is cool with it.  Sigh.
set(rr_VERSION_MAJOR 5)
set(rr_VERSION_MINOR 0)
set(rr_VERSION_PATCH 0)

add_definitions(-DRR_VERSION="${rr_VERSION_MAJOR}.${rr_VERSION_MINOR}.${rr_VERSION_PATCH}")

set(FLAGS_COMMON "-msse2 -D__MMX__ -D__SSE__ -D__SSE2__ -D__USE_LARGEFILE64 -pthread -g3")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FLAGS_COMMON} -Wstrict-prototypes -std=gnu11")
# Define __STDC_LIMIT_MACROS so |#include <stdint.h>| works as expected.
# Define __STDC_FORMAT_MACROS so |#include <inttypes.h>| works as expected.
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FLAGS_COMMON} -D__STDC_LIMIT_MACROS -D__STDC_FORMAT_MACROS -std=c++11")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} -g3")

set(RR_FLAGS_DEBUG "-Wall -Wextra -Werror -O0 -DDEBUG -UNDEBUG")
set(RR_FLAGS_RELEASE "-Wall -Wextra -O2 -UDEBUG -DNDEBUG")
set(RR_TEST_FLAGS "${RR_FLAGS_DEBUG}")
if(CMAKE_BUILD_TYPE MATCHES "DEBUG")
  set(RR_FLAGS ${RR_FLAGS_DEBUG})
else()
  set(RR_FLAGS ${RR_FLAGS_RELEASE})
endif()

if(CMAKE_C_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-unused-command-line-argument")
endif()
if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-unused-command-line-argument")
endif()

option(force32bit "Force a 32-bit rr build, rather than both 64 and 32-bit. rr will only be able to record and replay 32-bit processes.")
option(disable32bit "On a 64-bit platform, avoid requiring a 32-bit cross-compilation toolchain by not building 32-bit components. rr will be able to record 32-bit processes but not replay them.")

if(force32bit)
  set(rr_32BIT true)
  set(rr_64BIT false)
  set(rr_MBITNESS_OPTION -m32)
else()
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    if(disable32bit)
      set(rr_32BIT false)
    else()
      set(rr_32BIT true)
    endif()
    set(rr_64BIT true)
  else()
    set(rr_32BIT true)
    set(rr_64BIT false)
  endif()
  set(rr_MBITNESS_OPTION)
endif()

option(staticlibs "Force usage of static linkage for non-standard libraries like capnproto")

# Check that compiling 32-bit code on a 64-bit target works, if required.
if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64" AND rr_32BIT)
  # try_compile won't accept LINK_FLAGS, so do this manually.
  file(WRITE "${CMAKE_BINARY_DIR}/test32.c" "int main() { return 0; }")
  execute_process(COMMAND ${CMAKE_C_COMPILER} -o ${CMAKE_BINARY_DIR}/test32 ${CMAKE_BINARY_DIR}/test32.c -m32
			RESULT_VARIABLE COMPILER_32BIT_RESULT)
  if(NOT (COMPILER_32BIT_RESULT EQUAL 0))
    message(FATAL_ERROR "Your toolchain doesn't support 32-bit cross-compilation. Install the required packages or pass -Ddisable32bit=ON to cmake.")
  endif()
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${rr_MBITNESS_OPTION}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${rr_MBITNESS_OPTION}")
set(CMAKE_ASM_FLAGS "${CMAKE_ASM_FLAGS} ${rr_MBITNESS_OPTION}")

find_package(PkgConfig REQUIRED)

# If we're cross-compiling a 32-bit rr build on a 64-bit host we need
# to ensure we're looking for the right libraries.
# This has been tested on Ubuntu and Fedora.
if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "x86_64" AND NOT rr_64BIT)
  set(LIBDIR32_CANDIDATES
    /usr/lib/i386-linux-gnu/pkgconfig/
    /usr/lib/pkgconfig/
  )
  foreach(libdir ${LIBDIR32_CANDIDATES})
    if(IS_DIRECTORY ${libdir})
      set(ENV{PKG_CONFIG_LIBDIR} ${libdir})
      break()
     endif()
   endforeach(libdir)
   if(NOT DEFINED ENV{PKG_CONFIG_LIBDIR})
     message(FATAL_ERROR "Couldn't find a suitable 32-bit pkgconfig lib dir. You probably need to install a 32-bit pkgconfig package (pkgconfig.i686 for Fedora or pkg-config:i386 for Ubuntu")
   endif()
endif()

find_program(CAPNP capnp)
if(${CAPNP} STREQUAL "CAPNP-NOTFOUND")
  message(FATAL_ERROR "Can't find 'capnp' command; install Capnproto packages? https://github.com/mozilla/rr/wiki/Building-And-Installing#tldr")
endif()

#set(REQUIRED_LIBS
#  capnp
#)

foreach(required_lib ${REQUIRED_LIBS})
  string(TOUPPER ${required_lib} PKG)
  pkg_check_modules(${PKG} REQUIRED ${required_lib})
  if(staticlibs)
    string(REPLACE ";" " " ${PKG}_STATIC_CFLAGS "${${PKG}_STATIC_CFLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${${PKG}_STATIC_CFLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${${PKG}_STATIC_CFLAGS}")
  else()
    string(REPLACE ";" " " ${PKG}_CFLAGS "${${PKG}_CFLAGS}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${${PKG}_CFLAGS}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${${PKG}_CFLAGS}")
  endif()
endforeach(required_lib)

# ==== brotli ====
set(BROTLI_FILES
  third-party/brotli/common/dictionary.c
  third-party/brotli/dec/bit_reader.c
  third-party/brotli/dec/decode.c
  third-party/brotli/dec/huffman.c
  third-party/brotli/dec/state.c
  third-party/brotli/enc/backward_references.c
  third-party/brotli/enc/backward_references.h
  third-party/brotli/enc/backward_references_hq.c
  third-party/brotli/enc/backward_references_hq.h
  third-party/brotli/enc/backward_references_inc.h
  third-party/brotli/enc/bit_cost.c
  third-party/brotli/enc/bit_cost.h
  third-party/brotli/enc/bit_cost_inc.h
  third-party/brotli/enc/block_encoder_inc.h
  third-party/brotli/enc/block_splitter.c
  third-party/brotli/enc/block_splitter.h
  third-party/brotli/enc/block_splitter_inc.h
  third-party/brotli/enc/brotli_bit_stream.c
  third-party/brotli/enc/brotli_bit_stream.h
  third-party/brotli/enc/cluster.c
  third-party/brotli/enc/cluster.h
  third-party/brotli/enc/cluster_inc.h
  third-party/brotli/enc/command.h
  third-party/brotli/enc/compress_fragment.c
  third-party/brotli/enc/compress_fragment.h
  third-party/brotli/enc/compress_fragment_two_pass.c
  third-party/brotli/enc/compress_fragment_two_pass.h
  third-party/brotli/enc/context.h
  third-party/brotli/enc/dictionary_hash.c
  third-party/brotli/enc/dictionary_hash.h
  third-party/brotli/enc/encode.c
  third-party/brotli/enc/entropy_encode.c
  third-party/brotli/enc/entropy_encode.h
  third-party/brotli/enc/entropy_encode_static.h
  third-party/brotli/enc/fast_log.h
  third-party/brotli/enc/find_match_length.h
  third-party/brotli/enc/hash_forgetful_chain_inc.h
  third-party/brotli/enc/hash.h
  third-party/brotli/enc/hash_longest_match64_inc.h
  third-party/brotli/enc/hash_longest_match_inc.h
  third-party/brotli/enc/hash_longest_match_quickly_inc.h
  third-party/brotli/enc/hash_to_binary_tree_inc.h
  third-party/brotli/enc/histogram.c
  third-party/brotli/enc/histogram.h
  third-party/brotli/enc/histogram_inc.h
  third-party/brotli/enc/literal_cost.c
  third-party/brotli/enc/literal_cost.h
  third-party/brotli/enc/memory.c
  third-party/brotli/enc/memory.h
  third-party/brotli/enc/metablock.c
  third-party/brotli/enc/metablock.h
  third-party/brotli/enc/metablock_inc.h
  third-party/brotli/enc/port.h
  third-party/brotli/enc/prefix.h
  third-party/brotli/enc/quality.h
  third-party/brotli/enc/ringbuffer.h
  third-party/brotli/enc/static_dict.c
  third-party/brotli/enc/static_dict.h
  third-party/brotli/enc/static_dict_lut.h
  third-party/brotli/enc/utf8_util.c
  third-party/brotli/enc/utf8_util.h
  third-party/brotli/enc/write_bits.h
)
add_library(brotli STATIC ${BROTLI_FILES})
set_source_files_properties(${BROTLI_FILES}
                            PROPERTIES COMPILE_FLAGS ${RR_FLAGS_RELEASE})
# ==== brotli ====

#find_path(SECCOMP NAMES "linux/seccomp.h")
#if(NOT SECCOMP)
#  message(FATAL_ERROR "Couldn't find linux/seccomp.h. You may need to upgrade your kernel.")
#endif()

# Check for Python >=2.7 but not Python 3.
find_package(PythonInterp 2.7 REQUIRED)
if(PYTHON_VERSION_MAJOR GREATER 2)
  message(FATAL_ERROR "Python 3 is not supported, please use Python 2.7.")
endif()

execute_process(COMMAND "${PYTHON_EXECUTABLE}" "-c" "# nothing"
                RESULT_VARIABLE python_status)
if(python_status)
  message(FATAL_ERROR "Couldn't run python interpreter ${PYTHON_EXECUTABLE}.")
endif()

# Check for required Python modules
if(WILL_RUN_TESTS)
  if(NOT BUILD_TESTS)
    message(FATAL_ERROR "Running tests requires building them")
  endif()

  set(REQUIRED_PYTHON_MODULES
    pexpect
  )
else()
  set(REQUIRED_PYTHON_MODULES)
endif()

foreach(py_module ${REQUIRED_PYTHON_MODULES})
  execute_process(COMMAND "${PYTHON_EXECUTABLE}" "-c"
			"import ${py_module}"
			RESULT_VARIABLE module_status)
  if(module_status)
    message(FATAL_ERROR "Couldn't find required Python module ${py_module}.")
  endif()
endforeach(py_module)

if(WILL_RUN_TESTS)
  # Check for gdb
  execute_process(COMMAND "gdb" "--version" RESULT_VARIABLE module_status OUTPUT_QUIET)
  if(module_status)
    message(FATAL_ERROR "Couldn't find gdb.")
  endif()
endif()

set(PRELOAD_COMPILE_FLAGS "-fno-stack-protector")

if (DEBUG_PRELOAD_LIB)
  set(PRELOAD_COMPILE_FLAGS "${PRELOAD_COMPILE_FLAGS} ${RR_FLAGS_DEBUG}")
else()
  set(PRELOAD_COMPILE_FLAGS "${PRELOAD_COMPILE_FLAGS} ${RR_FLAGS_RELEASE}")
endif()

set_source_files_properties(src/preload/syscallbuf.c
                            src/preload/overrides.c
                            PROPERTIES COMPILE_FLAGS ${PRELOAD_COMPILE_FLAGS})

include_directories("${PROJECT_SOURCE_DIR}/include")
include_directories("${PROJECT_SOURCE_DIR}/third-party/proc-service")
include_directories("${PROJECT_SOURCE_DIR}/third-party/brotli/include")
# We need to know where our generated files are.
include_directories("${CMAKE_CURRENT_BINARY_DIR}")

# Order matters here! syscall_hook.S must be immediately before syscallbuf.c,
# breakpoint_table.S must be before overrides.c, which must be last.
add_library(rrpreload
  src/preload/syscall_hook.S
  src/preload/syscallbuf.c
  src/preload/raw_syscall.S
  src/preload/breakpoint_table.S
  src/preload/overrides.c
)
set_target_properties(rrpreload PROPERTIES LINK_FLAGS "-nostartfiles")

# Ensure that CMake knows about our generated files.
#
# Alphabetical, please.
set(GENERATED_FILES
  AssemblyTemplates.generated
  CheckSyscallNumbers.generated
  SyscallEnumsX64.generated
  SyscallEnumsX86.generated
  SyscallEnumsForTestsX64.generated
  SyscallEnumsForTestsX86.generated
  SyscallHelperFunctions.generated
  SyscallnameArch.generated
  SyscallRecordCase.generated
)

foreach(generated_file ${GENERATED_FILES})
  set_source_files_properties(${generated_file}
                              PROPERTIES GENERATED true HEADER_FILE_ONLY true)
  add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${generated_file}"
                     COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_syscalls.py"
		               "${CMAKE_CURRENT_BINARY_DIR}/${generated_file}"
		     DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_syscalls.py"
		       "${CMAKE_CURRENT_SOURCE_DIR}/src/syscalls.py"
		       "${CMAKE_CURRENT_SOURCE_DIR}/src/assembly_templates.py")
endforeach(generated_file)

add_custom_target(Generated DEPENDS ${GENERATED_FILES})

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64"
                   COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py"
                   "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32"
                   COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py"
                   "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64_replay"
                   COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py"
                   "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64_replay"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py")
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32_replay"
                   COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py"
                   "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32_replay"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/generate_rr_page.py")

add_custom_target(Pages DEPENDS
                  "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32"
                  "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64"
                  "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32_replay"
                  "${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64_replay")

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rr_trace.capnp.c++"
                          "${CMAKE_CURRENT_BINARY_DIR}/rr_trace.capnp.h"
                   COMMAND capnp compile
                           "--src-prefix=${CMAKE_CURRENT_SOURCE_DIR}/src"
                           "-oc++:${CMAKE_CURRENT_BINARY_DIR}"
                           "${CMAKE_CURRENT_SOURCE_DIR}/src/rr_trace.capnp"
                   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/rr_trace.capnp")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/rr_trace.capnp.c++"
                            PROPERTIES GENERATED true)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/rr_trace.capnp.h"
                            PROPERTIES GENERATED true HEADER_FILE_ONLY true)

set(RR_SOURCES
  src/test/cpuid_loop.S
  src/AddressSpace.cc
  src/AutoRemoteSyscalls.cc
  src/Command.cc
  src/CompressedReader.cc
  src/CompressedWriter.cc
  src/CPUIDBugDetector.cc
  src/DiversionSession.cc
  src/DumpCommand.cc
  src/ElfReader.cc
  src/EmuFs.cc
  src/Event.cc
  src/ExtraRegisters.cc
  src/fast_forward.cc
  src/FdTable.cc
  src/FileMonitor.cc
  src/Flags.cc
  src/ftrace.cc
  src/GdbCommand.cc
  src/GdbCommandHandler.cc
  src/GdbConnection.cc
  src/GdbExpression.cc
  src/GdbInitCommand.cc
  src/GdbServer.cc
  src/HasTaskSet.cc
  src/HelpCommand.cc
  src/kernel_abi.cc
  src/kernel_metadata.cc
  src/log.cc
  src/MagicSaveDataMonitor.cc
  src/MmappedFileMonitor.cc
  src/MonitoredSharedMemory.cc
  src/Monkeypatcher.cc
  src/PackCommand.cc
  src/PerfCounters.cc
  src/ProcFdDirMonitor.cc
  src/ProcMemMonitor.cc
  src/PsCommand.cc
  src/RecordCommand.cc
  src/RecordSession.cc
  src/record_signal.cc
  src/record_syscall.cc
  src/RecordTask.cc
  src/Registers.cc
  src/remote_code_ptr.cc
  src/ReplayCommand.cc
  src/ReplaySession.cc
  src/replay_syscall.cc
  src/ReplayTask.cc
  src/ReplayTimeline.cc
  src/RerunCommand.cc
  src/ReturnAddressList.cc
  src/Scheduler.cc
  src/SeccompFilterRewriter.cc
  src/Session.cc
  src/StdioMonitor.cc
  src/Task.cc
  src/ThreadGroup.cc
  src/ThreadDb.cc
  src/TraceFrame.cc
  src/TraceStream.cc
  src/VirtualPerfCounterMonitor.cc
  src/util.cc
  src/WaitStatus.cc
  ${CMAKE_CURRENT_BINARY_DIR}/rr_trace.capnp.c++
  third-party/blake2/blake2b.c
)

set_source_files_properties(${RR_SOURCES}
                            PROPERTIES COMPILE_FLAGS ${RR_FLAGS})

function(post_build_executable target)
# grsecurity needs these. But if we add them ourselves, they may conflict
# with other flags added in other ways, and they all have to match :-(. So
# don't do this until a better solution presents itself
#  add_custom_command(TARGET ${target}
#                     POST_BUILD
#                     COMMAND setfattr ARGS -n user.pax.flags -v m $<TARGET_FILE:${target}>)
endfunction(post_build_executable)

option(RR_BUILD_SHARED "Build the rr shared library as well as the binary (experimental).")

if(RR_BUILD_SHARED)
  add_library(rr ${RR_SOURCES})
  set_target_properties(rr PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/lib)
  add_executable(rrbin src/main.cc)
  set(RR_BIN rrbin)
  post_build_executable(rrbin)
  set_target_properties(rrbin PROPERTIES ENABLE_EXPORTS true OUTPUT_NAME rr)
  set_target_properties(rrbin PROPERTIES INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib")
  set_target_properties(rrbin PROPERTIES INSTALL_RPATH_USE_LINK_PATH true)
  set_target_properties(brotli PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(rrbin rr)
  install(TARGETS rr
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib)
else()
  add_executable(rr ${RR_SOURCES} src/main.cc)
  set_target_properties(rr PROPERTIES ENABLE_EXPORTS true)
  post_build_executable(rr)
  set(RR_BIN rr)
endif()
add_dependencies(rr Generated Pages)

option(strip "Strip debug info from rr binary")

if(strip)
  set_target_properties(rr PROPERTIES LINK_FLAGS "-s")
endif()

target_link_libraries(rr
  ${CMAKE_DL_LIBS}
  -lrt
  brotli
)

if(staticlibs)
  # Urgh ... this might not work for everyone, but there doesn't seem to be
  # a way to persuade pkg-confing/pkg_check_modules to produce the right flags
  target_link_libraries(rr -l:libcapnp.a -l:libkj.a)
else()
  target_link_libraries(rr ${CAPNP_LDFLAGS})
endif()

target_link_libraries(rrpreload
  ${CMAKE_DL_LIBS}
)

add_executable(rr_exec_stub src/exec_stub.c)
post_build_executable(rr_exec_stub)
set_target_properties(rr_exec_stub
                      PROPERTIES LINK_FLAGS "-static -nostartfiles -nodefaultlibs")
set_source_files_properties(src/exec_stub.c
                            COMPILE_FLAGS "-fno-stack-protector")

set(RR_GDB_RESOURCES
  32bit-avx.xml
  32bit-core.xml
  32bit-linux.xml
  32bit-sse.xml
  64bit-avx.xml
  64bit-core.xml
  64bit-linux.xml
  64bit-seg.xml
  64bit-sse.xml
  amd64-avx-linux.xml
  amd64-linux.xml
  i386-avx-linux.xml
  i386-linux.xml
)
foreach(file ${RR_GDB_RESOURCES})
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/third-party/gdb/${file}"
                 "${CMAKE_CURRENT_BINARY_DIR}/share/rr/${file}"
                 COPYONLY)
  install(FILES third-party/gdb/${file}
          DESTINATION share/rr)
endforeach(file)

install(PROGRAMS scripts/signal-rr-recording.sh
                  ${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64
                  ${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_64_replay
                  ${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32
                  ${CMAKE_CURRENT_BINARY_DIR}/bin/rr_page_32_replay
  DESTINATION bin)

install(TARGETS ${RR_BIN} rrpreload rr_exec_stub
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION lib/rr
  ARCHIVE DESTINATION lib/rr)

# Build 32-bit librrpreload on 64-bit builds.
# We copy the source files into '32' subdirectories in the output
# directory, so we can set different compile options on them.
# This sucks but I can't find a better way to get CMake to build
# the same source file in two different ways.
if(rr_32BIT AND rr_64BIT)
  foreach(file preload_interface.h syscallbuf.h)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/preload/${file}"
                   "${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                   COPYONLY)
  endforeach(file)

  foreach(file syscallbuf.c overrides.c raw_syscall.S syscall_hook.S breakpoint_table.S)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/preload/${file}"
                   "${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                   COPYONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                                PROPERTIES COMPILE_FLAGS "-m32 ${RR_FLAGS_RELEASE}")
  endforeach(file)

  # Order matters here! syscall_hook.S must be immediately before syscallbuf.c.
  # breakpoint_table.S must be before overrides.c, which must be last.
  add_library(rrpreload_32
    32/syscall_hook.S
    32/syscallbuf.c
    32/raw_syscall.S
    32/breakpoint_table.S
    32/overrides.c
  )
  set_target_properties(rrpreload_32 PROPERTIES LINK_FLAGS "-m32 -nostartfiles")
  target_link_libraries(rrpreload_32
    ${CMAKE_DL_LIBS}
  )

  foreach(file exec_stub.c)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/${file}"
                   "${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                   COPYONLY)
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                                PROPERTIES COMPILE_FLAGS "-m32 -fno-stack-protector")
  endforeach(file)

  add_executable(rr_exec_stub_32 32/exec_stub.c)
  post_build_executable(rr_exec_stub_32)
  set_target_properties(rr_exec_stub_32
                        PROPERTIES LINK_FLAGS "-static -nostartfiles -nodefaultlibs -m32")

  install(TARGETS rrpreload_32 rr_exec_stub_32
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib/rr
    ARCHIVE DESTINATION lib/rr)
endif()

##--------------------------------------------------
## Testing

# A "basic test" consists of a foo.c source file. All basic tests use the
# same basic_test.run driver script. The test name is passed as an additional
# parameter to the driver script. This script just does
# "compare_test EXIT-SUCCESS", i.e. records and replays the program and verifies
# that the output of both runs is identical and contains EXIT-SUCCESS.
#
# NB: you must update this variable when adding a new test source
# file.  The list is not generated automatically.
#
# Alphabetical, please.
set(BASIC_TESTS
)

set(BASIC_CPP_TESTS
  std_random
)

# A "test with program" consists of a foo.c source file and a foo.run driver
# script.  See src/test/util.sh to learn how the .run files work.
#
# NB: you must update this variable when adding a new test source
# file.  The list is not generated automatically.
#
# Alphabetical, please.
set(TESTS_WITH_PROGRAM
)

# A "test without program" is a foo.run driver script only, which does
# something with one of the test executables above (or has special rules
# to build its own executable).
#
# NB: you must update this variable when adding a new test source
# file.  The list is not generated automatically.
#
# Alphabetical, please.
set(TESTS_WITHOUT_PROGRAM
  async_signal_syscalls_100
  async_signal_syscalls_1000
  bad_breakpoint
  break_block
  break_clock
  break_clone
  break_exec
  break_int3
  break_mmap_private
  break_msg
  break_rdtsc
  break_sigreturn
  break_sync_signal
  break_thread
  break_time_slice
  breakpoint_consistent
  call_exit
  check_patched_pthread
  checkpoint_async_signal_syscalls_1000
  checkpoint_mmap_shared
  checkpoint_prctl_name
  checkpoint_simple
  checksum_sanity_noclone
  cont_signal
  cpuid
  dead_thread_target
  desched_ticks
  deliver_async_signal_during_syscalls
  env_newline
  exec_deleted
  exec_stop
  execp
  explicit_checkpoint_clone
  file_name_newline
  final_sigkill
  first_instruction
  fork_exec_info_thr
  gcrypt_rdrand
  get_thread_list
  hardlink_mmapped_files
  mprotect_step
  parent_no_break_child_bkpt
  parent_no_stop_child_crash
  post_exec_fpu_regs
  proc_maps
  read_bad_mem
  record_replay
  remove_watchpoint
  replay_overlarge_event_number
  restart_invalid_checkpoint
  restart_unstable
  restart_diversion
  reverse_alarm
  reverse_continue_exec_subprocess
  reverse_continue_fork_subprocess
  reverse_continue_start
  reverse_finish
  reverse_step_breakpoint
  reverse_step_signal
  reverse_step_threads2
  reverse_watchpoint
  reverse_watchpoint_syscall
  run_end
  run_in_function
  sanity
  shm_checkpoint
  siginfo
  sigreturn_checksum
  signal_stop
  signal_checkpoint
  simple_script
  simple_script_debug
  simple_winch
  stack_overflow_debug
  step1
  step_rdtsc
  step_signal
  string_instructions_break
  string_instructions_replay_quirk
  subprocess_exit_ends_session
  switch_processes
  syscallbuf_timeslice_250
  trace_version
  term_trace_cpu
  unwind_on_signal
  vfork_exec
  watch_code
  watchpoint_cond
  when
)

if(BUILD_TESTS)
  add_test(check_environment
      bash ${CMAKE_SOURCE_DIR}/src/test/check_environment_test.run)
  set_tests_properties(check_environment
      PROPERTIES FAIL_REGULAR_EXPRESSION "rr needs /proc/sys/kernel/perf_event_paranoid <= 1")

  foreach(test ${BASIC_TESTS} ${TESTS_WITH_PROGRAM})
    add_executable(${test} src/test/${test}.c)
    post_build_executable(${test})
    set_source_files_properties(src/test/${test}.c
                                PROPERTIES COMPILE_FLAGS ${RR_TEST_FLAGS})
    add_dependencies(${test} Generated)
    # FreeBSD has no -ldl.
    target_link_libraries(${test} -lrt)
  endforeach(test)

  # Test disabled because it requires libuvc to be built and installed, and a
  # working USB camera
  # add_executable(usb src/test/usb.c)
  # post_build_executable(usb)
  # add_dependencies(usb Generated)
  # target_link_libraries(usb -lrt -L/usr/local/lib -luvc -lusb-1.0)

  foreach(test ${BASIC_CPP_TESTS})
    add_executable(${test} src/test/${test}.cc)
    post_build_executable(${test})
    set_source_files_properties(src/test/${test}.cc
                                PROPERTIES COMPILE_FLAGS ${RR_TEST_FLAGS})
    add_dependencies(${test} Generated)
    target_link_libraries(${test} -lrt)
  endforeach(test)

  add_library(test_lib
    src/test/test_lib.c
    )
  add_dependencies(test_lib Generated)
  #set_source_files_properties(src/test/test_lib.c
  #                            PROPERTIES COMPILE_FLAGS ${RR_TEST_FLAGS})
  #target_link_libraries(constructor -lrt test_lib)

  # cpuid test needs to link with cpuid_loop.S
  add_executable(cpuid src/test/cpuid.c src/test/cpuid_loop.S)
  post_build_executable(cpuid)
  set_source_files_properties(src/test/cpuid.c
                              PROPERTIES COMPILE_FLAGS ${RR_TEST_FLAGS})
  add_dependencies(cpuid Generated)
  target_link_libraries(cpuid -lrt)

  # Check if we're running on KNL. If so, we allot more time to tests, due to
  # reduced single-core performance.
  exec_program(cat ARGS "/proc/cpuinfo" OUTPUT_VARIABLE CPUINFO)
  string(REGEX MATCH "^.*(Xeon Phi).*$" CPU_MODEL_PHI ${CPUINFO})
  if(NOT "${CPU_MODEL_PHI}" STREQUAL "")
    set(TEST_MONITOR_DEFAULT_TIMEOUT 480)
  else()
    set(TEST_MONITOR_DEFAULT_TIMEOUT 120)
  endif()

  function(configure_test test)
    # The real timeouts are handled by test-monitor
    set_tests_properties(${test}
      PROPERTIES FAIL_REGULAR_EXPRESSION "FAILED" TIMEOUT 1000)
  endfunction(configure_test)

  foreach(test ${BASIC_TESTS} ${BASIC_CPP_TESTS} ${OTHER_TESTS})
    add_test(${test}
      bash ${CMAKE_SOURCE_DIR}/src/test/basic_test.run ${test} -b ${PROJECT_BINARY_DIR} ${TEST_MONITOR_DEFAULT_TIMEOUT})
    configure_test(${test})
    add_test(${test}-no-syscallbuf
      bash ${CMAKE_SOURCE_DIR}/src/test/basic_test.run ${test} -n ${PROJECT_BINARY_DIR} ${TEST_MONITOR_DEFAULT_TIMEOUT})
    configure_test(${test}-no-syscallbuf)
  endforeach(test)

  foreach(test ${TESTS_WITH_PROGRAM} ${TESTS_WITHOUT_PROGRAM})
    add_test(${test}
      bash ${CMAKE_SOURCE_DIR}/src/test/${test}.run ${test} -b ${PROJECT_BINARY_DIR} ${TEST_MONITOR_DEFAULT_TIMEOUT})
    configure_test(${test})
    add_test(${test}-no-syscallbuf
      bash ${CMAKE_SOURCE_DIR}/src/test/${test}.run ${test} -n ${PROJECT_BINARY_DIR} ${TEST_MONITOR_DEFAULT_TIMEOUT})
    configure_test(${test}-no-syscallbuf)
  endforeach(test)

  # Run 32-bit tests on 64-bit builds.
  # We copy the test files into '32' subdirectories in the output
  # directory, so we can set different compile options on them.
  # This sucks but I can't find a better way to get CMake to build
  # the same source file in two different ways.
  if(rr_32BIT AND rr_64BIT)
    foreach(header util.h nsutils.h)
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/test/${header}"
                    "${CMAKE_CURRENT_BINARY_DIR}/32/${header}"
                    COPYONLY)
    endforeach(header)

    foreach(test ${BASIC_TESTS} ${TESTS_WITH_PROGRAM} cpuid test_lib)
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/test/${test}.c"
                     "${CMAKE_CURRENT_BINARY_DIR}/32/${test}.c"
                     COPYONLY)
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/32/${test}.c"
                                  PROPERTIES COMPILE_FLAGS "-m32 ${RR_TEST_FLAGS}")
    endforeach(test)

    foreach(test ${BASIC_CPP_TESTS})
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/test/${test}.cc"
                     "${CMAKE_CURRENT_BINARY_DIR}/32/${test}.cc"
                     COPYONLY)
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/32/${test}.cc"
                                  PROPERTIES COMPILE_FLAGS "-m32 ${RR_TEST_FLAGS}")
    endforeach(test)

    foreach(file cpuid_loop.S)
      configure_file("${CMAKE_CURRENT_SOURCE_DIR}/src/test/${file}"
                     "${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                     COPYONLY)
      set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/32/${file}"
                                  PROPERTIES COMPILE_FLAGS "-m32 ${RR_TEST_FLAGS}")
    endforeach(file)

    foreach(test ${BASIC_TESTS} ${TESTS_WITH_PROGRAM})
      add_executable(${test}_32 "${CMAKE_CURRENT_BINARY_DIR}/32/${test}.c")
      post_build_executable(${test}_32)
      add_dependencies(${test}_32 Generated)
      set_target_properties(${test}_32 PROPERTIES LINK_FLAGS "-m32 ${RR_TEST_FLAGS}")
      target_link_libraries(${test}_32 -lrt -ldl)
    endforeach(test)

    foreach(test ${BASIC_CPP_TESTS})
      add_executable(${test}_32 "${CMAKE_CURRENT_BINARY_DIR}/32/${test}.cc")
      post_build_executable(${test}_32)
      add_dependencies(${test}_32 Generated)
      set_target_properties(${test}_32 PROPERTIES LINK_FLAGS "-m32 ${RR_TEST_FLAGS}")
      target_link_libraries(${test}_32 -lrt)
    endforeach(test)

    add_library(test_lib_32
      "${CMAKE_CURRENT_BINARY_DIR}/32/test_lib.c"
    )
    add_dependencies(test_lib_32 Generated)
    set_target_properties(test_lib_32 PROPERTIES LINK_FLAGS -m32)

    target_link_libraries(constructor_32 -lrt test_lib_32)

    # cpuid test needs to link with cpuid_loop.S
    add_executable(cpuid_32 32/cpuid.c 32/cpuid_loop.S)
    post_build_executable(cpuid_32)
    add_dependencies(cpuid_32 Generated)
    set_target_properties(cpuid_32 PROPERTIES LINK_FLAGS -m32)
    target_link_libraries(cpuid_32 -lrt)

    foreach(test ${BASIC_TESTS} ${BASIC_CPP_TESTS} ${OTHER_TESTS})
      add_test(${test}-32
        bash ${CMAKE_SOURCE_DIR}/src/test/basic_test.run ${test}_32 -b ${PROJECT_BINARY_DIR})
      configure_test(${test}-32)
      add_test(${test}-32-no-syscallbuf
        bash ${CMAKE_SOURCE_DIR}/src/test/basic_test.run ${test}_32 -n ${PROJECT_BINARY_DIR})
      configure_test(${test}-32-no-syscallbuf)
    endforeach(test)

    foreach(test ${TESTS_WITH_PROGRAM} ${TESTS_WITHOUT_PROGRAM})
      add_test(${test}-32
        bash ${CMAKE_SOURCE_DIR}/src/test/${test}.run ${test}_32 -b ${PROJECT_BINARY_DIR})
      configure_test(${test}-32)
      add_test(${test}-32-no-syscallbuf
	    bash ${CMAKE_SOURCE_DIR}/src/test/${test}.run ${test}_32 -n ${PROJECT_BINARY_DIR})
      configure_test(${test}-32-no-syscallbuf)
    endforeach(test)
  endif()

  set(CHAOS_TESTS
    core_count
    mmap_adjacent
    mmap_bits
    starvation_multithreaded
    starvation_singlethreaded
  )

  foreach(test ${CHAOS_TESTS})
    add_executable(${test} src/chaos-test/${test}.c)
    post_build_executable(${test})
    target_link_libraries(${test} -lrt)
  endforeach(test)
endif()

add_executable(ftrace_helper src/ftrace/ftrace_helper.c)

add_executable(test-monitor src/test-monitor/test-monitor.cc)

include(ProcessorCount)
ProcessorCount(N)
if(NOT N EQUAL 0)
  set(JFLAG -j${N})
endif()

add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --verbose ${JFLAG})
# Run only syscallbuf-enabled and native-bitness tests
add_custom_target(fastcheck COMMAND ${CMAKE_CTEST_COMMAND} --verbose --exclude-regex '[-]' ${JFLAG})

##--------------------------------------------------
## Package configuration

include (InstallRequiredSystemLibraries)

set(CPACK_PACKAGE_NAME "rr")
set(CPACK_PACKAGE_VERSION_MAJOR "${rr_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${rr_VERSION_MINOR}")
set(CPACK_PACKAGE_VERSION_PATCH "${rr_VERSION_PATCH}")
set(CPACK_SYSTEM_NAME "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

set(CPACK_OUTPUT_FILE_PREFIX dist)
set(CPACK_GENERATOR "TGZ;RPM;DEB")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_BINARY_DIR "${PROJECT_BINARY_DIR}")
# Don't strip binaries. It's important/useful for librrpreload at least to
# have debug symbols. For package releases, pass -Dstrip=TRUE to strip symbols
# from the rr binary at build time.
set(CPACK_STRIP_FILES FALSE)

set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
  "Lightweight tool for recording and replaying execution of applications (trees of processes and threads)")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_PACKAGE_VENDOR "Mozilla Foundation")

set(CPACK_DEBIAN_PACKAGE_MAINTAINER "Mozilla Foundation")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
if(${CMAKE_SYSTEM_PROCESSOR} STREQUAL "x86_64")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "i.86")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "i386")
elseif(${CMAKE_SYSTEM_PROCESSOR} MATCHES "arm.*")
  set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "arm")
endif()

# XXX Cmake 2.8.7 doesn't know how to avoid specifying /usr,
# /usr/bin, etc, as files to be installed, but distros are finicky
# about their specification.  We want to manually filter those paths
# out of our install list but 2.8.7 also isn't capable of that.
set(CPACK_RPM_USER_BINARY_SPECFILE "${CMAKE_SOURCE_DIR}/rr.spec")
set(CPACK_RPM_PACKAGE_RELEASE 1)
set(CPACK_RPM_PACKAGE_GROUP "Development/Debuggers")
set(CPACK_RPM_PACKAGE_LICENSE "MIT and BSD")
# Prevent binaries from being stripped
set(CPACK_RPM_SPEC_INSTALL_POST "/bin/true")

include (CPack)

##--------------------------------------------------
## Misc

add_custom_target(setup-travis COMMAND src/script/setup_travis.sh)
