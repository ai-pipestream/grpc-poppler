# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.24)
project(grpc_poppler CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)

add_library(grpc_poppler_flags INTERFACE)
target_compile_options(grpc_poppler_flags INTERFACE -Wall -Wextra)

# Build version reported by GetServiceInfo: the image tag when built in
# Docker (passed as -DGRPC_POPPLER_BUILD_VERSION), else git describe, else
# "dev".
set(GRPC_POPPLER_BUILD_VERSION "" CACHE STRING
    "build version GetServiceInfo reports")
if(NOT GRPC_POPPLER_BUILD_VERSION)
  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" describe --tags --always --dirty
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE GRPC_POPPLER_BUILD_VERSION
      OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(NOT GRPC_POPPLER_BUILD_VERSION)
    set(GRPC_POPPLER_BUILD_VERSION "dev")
  endif()
endif()
message(STATUS "grpc-poppler build version: ${GRPC_POPPLER_BUILD_VERSION}")
target_compile_definitions(grpc_poppler_flags
    INTERFACE GRPC_POPPLER_BUILD_VERSION="${GRPC_POPPLER_BUILD_VERSION}")

# ---------------------------------------------------------------------------
# gRPC + protobuf, the fleet's C++ pattern (same tag as gRParse).
# ---------------------------------------------------------------------------
set(gRPC_INSTALL OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(grpc
  GIT_REPOSITORY https://github.com/grpc/grpc.git
  GIT_TAG v1.83.1
  GIT_SHALLOW TRUE
  GIT_SUBMODULES_RECURSE TRUE)
FetchContent_MakeAvailable(grpc)

# ---------------------------------------------------------------------------
# The PdfBackendService contract (package ai.protomolt.parse.pdf.v1),
# fetched from the parser-protos repo at a pinned commit; gRParse backends/
# holds the same bytes.
# The download comes from the GitHub mirror's raw URL, not git.rokkon.com:
# GitHub-hosted runners cannot resolve the Forgejo host, and the per-file
# sha256 pins below verify the bytes either way.
# ---------------------------------------------------------------------------
set(PDF_PROTOS_COMMIT "ba226130c49683a8e629a6d3dc0f58c83c51c628"
    CACHE STRING "parser-protos commit the contract comes from")
set(PDF_PROTOS_BASE_URL
    "https://raw.githubusercontent.com/ai-pipestream/parser-protos/${PDF_PROTOS_COMMIT}/proto/ai/protomolt/parse/pdf/v1"
    CACHE STRING "base URL the contract proto files are downloaded from")
set(PDF_PROTO_TYPES_SHA256
    "27c9d9e4f822650344e609a1ca4ca79e7d7f2d867c0517f72bfa6982af8e8edd"
    CACHE STRING "sha256 of pdf_backend_types.proto")
set(PDF_PROTO_SERVICE_SHA256
    "f0d47132b48af9cf09007afac32b26ef8f8e521b16f7bc105780d6afef5e4e8a"
    CACHE STRING "sha256 of pdf_backend_service.proto")
set(PDF_PROTO_LOCAL_DIR "" CACHE PATH "directory holding the contract protos, instead of the pinned download")

set(STAGED_PROTO_DIR "${CMAKE_CURRENT_BINARY_DIR}/proto")
set(PDF_PROTO_REL "ai/protomolt/parse/pdf/v1")

if(PDF_PROTO_LOCAL_DIR)
  set(PDF_PROTO_SRC "${PDF_PROTO_LOCAL_DIR}")
else()
  set(PDF_PROTO_SRC "${CMAKE_CURRENT_BINARY_DIR}/protos-export/${PDF_PROTOS_COMMIT}")
  foreach(PROTO_SPEC IN ITEMS
      "pdf_backend_types.proto;${PDF_PROTO_TYPES_SHA256}"
      "pdf_backend_service.proto;${PDF_PROTO_SERVICE_SHA256}")
    list(GET PROTO_SPEC 0 PROTO_NAME)
    list(GET PROTO_SPEC 1 PROTO_SHA)
    if(NOT EXISTS "${PDF_PROTO_SRC}/${PROTO_NAME}")
      file(DOWNLOAD "${PDF_PROTOS_BASE_URL}/${PROTO_NAME}"
        "${PDF_PROTO_SRC}/${PROTO_NAME}"
        EXPECTED_HASH SHA256=${PROTO_SHA}
        STATUS PROTO_DL_STATUS)
      list(GET PROTO_DL_STATUS 0 PROTO_DL_CODE)
      if(NOT PROTO_DL_CODE EQUAL 0)
        file(REMOVE "${PDF_PROTO_SRC}/${PROTO_NAME}")
        message(FATAL_ERROR "contract proto download failed: ${PROTO_DL_STATUS}")
      endif()
    endif()
  endforeach()
endif()

file(MAKE_DIRECTORY "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}")
configure_file("${PDF_PROTO_SRC}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto" COPYONLY)
configure_file("${PDF_PROTO_SRC}/pdf_backend_service.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto" COPYONLY)

set(GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")

set(PDF_BACKEND_PROTOS
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_types.proto"
  "${STAGED_PROTO_DIR}/${PDF_PROTO_REL}/pdf_backend_service.proto")
set(PDF_BACKEND_GENERATED
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_types.grpc.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.pb.h"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.cc"
  "${GENERATED_DIR}/${PDF_PROTO_REL}/pdf_backend_service.grpc.pb.h")

add_custom_command(
  OUTPUT ${PDF_BACKEND_GENERATED}
  COMMAND $<TARGET_FILE:protobuf::protoc>
    --proto_path=${STAGED_PROTO_DIR}
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>
    ${PDF_BACKEND_PROTOS}
  DEPENDS ${PDF_BACKEND_PROTOS} protobuf::protoc grpc_cpp_plugin
  COMMENT "Generating C++ stubs for the PdfBackendService contract")

add_library(pdf_backend_proto STATIC ${PDF_BACKEND_GENERATED})
target_include_directories(pdf_backend_proto SYSTEM PUBLIC "${GENERATED_DIR}")
target_link_libraries(pdf_backend_proto PUBLIC grpc++ grpc++_reflection libprotobuf)

# ---------------------------------------------------------------------------
# Poppler. The GPL dependency lives in this repo alone; the wrapper source
# is GPL-3.0-or-later as a combined work.
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER_CPP REQUIRED IMPORTED_TARGET poppler-cpp)
# poppler-cpp.pc puts <includedir>/poppler/cpp on the include path, which
# only resolves the <poppler/cpp/...> includes below when includedir is a
# default search root (/usr/include). A poppler built under its own prefix
# (the image's /opt/poppler) needs the includedir itself on the path too.
target_include_directories(PkgConfig::POPPLER_CPP INTERFACE "${POPPLER_CPP_INCLUDEDIR}")
# The core library, for what poppler-cpp does not expose: the AcroForm
# widgets (src/poppler_forms.cpp) and the embedded files' raw names and
# streams (src/poppler_attachments.cpp). Its headers are installed by
# poppler's ENABLE_UNSTABLE_API_ABI_HEADERS option; the library is the one
# poppler-cpp already links, so the runtime closure does not change.
pkg_check_modules(POPPLER_CORE REQUIRED IMPORTED_TARGET poppler)

# ---------------------------------------------------------------------------
# The service binary. SHA-256 for the content-addressed cache comes from the
# boringssl crypto target gRPC already builds (no new dependency).
# ---------------------------------------------------------------------------
add_executable(grpc_poppler
  src/main.cpp
  src/poppler_service_impl.cpp
  src/poppler_forms.cpp
  src/poppler_attachments.cpp
  src/utf8.cpp
  src/document_cache.cpp
  src/sha256.cpp)
target_include_directories(grpc_poppler PRIVATE src)
target_link_libraries(grpc_poppler PRIVATE pdf_backend_proto PkgConfig::POPPLER_CPP PkgConfig::POPPLER_CORE crypto grpc_poppler_flags)

# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------
enable_testing()
add_executable(poppler_contract_test
  test/contract_roundtrip_test.cpp
  src/poppler_service_impl.cpp
  src/poppler_forms.cpp
  src/poppler_attachments.cpp
  src/utf8.cpp
  src/document_cache.cpp
  src/sha256.cpp)
target_include_directories(poppler_contract_test PRIVATE src)
target_link_libraries(poppler_contract_test PRIVATE pdf_backend_proto PkgConfig::POPPLER_CPP PkgConfig::POPPLER_CORE crypto grpc_poppler_flags)
add_test(NAME poppler_contract COMMAND poppler_contract_test
  "${CMAKE_CURRENT_SOURCE_DIR}/test/fixtures")
