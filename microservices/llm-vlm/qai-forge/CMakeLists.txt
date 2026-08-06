# ─────────────────────────────────────────────────────────────────────────────
# qai-forge — Qualcomm AI LLM Runtime SDK
#
# Builds three targets:
#
#   libqai_forge          — SDK library
#                            Linked by chatcompletions-server and responses-server.
#                            Exports qai_forge::qai_forge CMake target.
#
#   genai-inference-worker  — LLM worker subprocess binary
#                            Links libllmengine.so.
#                            Spawned by InferenceWorkerManager via fork/exec.
#
#   genai-vlm-inference-worker — VLM worker subprocess binary
#                            Links libvlmengine.so.
#                            Spawned by VlmInferenceWorkerManager via fork/exec.
#
# Also builds (as part of genai-lib):
#   libllmengine            — C++ engine wrapping GenieDialog (LLM)
#   libvlmengine            — C++ engine wrapping GeniePipeline (VLM)
#
# Required CMake variables:
#   GENIE_INCLUDE_PATH      — Path to Qualcomm GenIE SDK headers (GeniePipeline.h etc.)
#   GENIE_LIB_PATH          — Path to directory containing libGenie.so
#
# Optional CMake variables:
#   QAI_FORGE_BUILD_WORKERS  — Build worker binaries (default ON)
#   QAI_FORGE_BUILD_GENAILIB — Build genai-lib (default ON; set OFF if pre-built)
#   QAI_FORGE_INSTALL        — Install targets (default ON)
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)
project(qai_forge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ─────────────────────────────────────────────────────────────
option(QAI_FORGE_BUILD_WORKERS  "Build genai-inference-worker binaries" ON)
option(QAI_FORGE_BUILD_GENAILIB "Build genai-lib (Layer 4)" ON)
option(QAI_FORGE_BUILD_QNN       "Build QNN Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_SNPE      "Build SNPE Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_LITERT    "Build LiteRT Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_LITERT_LM "Build LiteRT-LM generative worker" OFF)
option(QAI_FORGE_INSTALL         "Install targets and CMake package config" ON)

# ── Required dependencies ─────────────────────────────────────────────────────
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

# ── stb_image (header-only image decoder — JPEG, PNG, WEBP, BMP, GIF) ────────
# Downloaded at configure time; compiled into libqai_forge (no runtime .so).
# stb_image.h is defined in exactly one .cpp file (ImageUtils.cpp).
include(FetchContent)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(stb)

# ── GenIE SDK paths ───────────────────────────────────────────────────────────
# Set GENIE_INCLUDE_PATH and GENIE_LIB_PATH on the cmake command line:
#   cmake -DGENIE_INCLUDE_PATH=/opt/genie/include -DGENIE_LIB_PATH=/opt/genie/lib
if(NOT DEFINED GENIE_INCLUDE_PATH)
    set(GENIE_INCLUDE_PATH "" CACHE PATH "Path to Qualcomm GenIE SDK headers")
endif()
if(NOT DEFINED GENIE_LIB_PATH)
    set(GENIE_LIB_PATH "" CACHE PATH "Path to directory containing libGenie.so")
endif()

# ── QNN SDK paths ─────────────────────────────────────────────────────────────
# Set QNN_INCLUDE_PATH on the cmake command line:
#   cmake -DQNN_INCLUDE_PATH=/opt/qnn/include
if(NOT DEFINED QNN_INCLUDE_PATH)
    set(QNN_INCLUDE_PATH "" CACHE PATH "Path to QNN SDK headers (QnnInterface.h etc.)")
endif()

# ── SNPE SDK paths ────────────────────────────────────────────────────────────
# Set SNPE_INCLUDE_PATH on the cmake command line:
#   cmake -DSNPE_INCLUDE_PATH=/opt/snpe/include
if(NOT DEFINED SNPE_INCLUDE_PATH)
    set(SNPE_INCLUDE_PATH "" CACHE PATH "Path to SNPE SDK headers (SNPE/SNPE.h etc.)")
endif()

# ── LiteRT SDK paths ──────────────────────────────────────────────────────────
# Set LITERT_INCLUDE_PATH on the cmake command line:
#   cmake -DLITERT_INCLUDE_PATH=/usr/include
# The include root must contain the litert/c/ subdirectory with all C API headers.
if(NOT DEFINED LITERT_INCLUDE_PATH)
    set(LITERT_INCLUDE_PATH "" CACHE PATH "Path to LiteRT C API headers (root containing litert/c/)")
endif()

# ── LiteRT-LM SDK paths ───────────────────────────────────────────────────────
# Set LITERT_LM_INCLUDE_PATH on the cmake command line:
#   cmake -DLITERT_LM_INCLUDE_PATH=/usr/include
# The include root must contain litert_lm/c/engine.h (staged by LiteRTLM_builder).
if(NOT DEFINED LITERT_LM_INCLUDE_PATH)
    set(LITERT_LM_INCLUDE_PATH "" CACHE PATH "Path to LiteRT-LM C API headers (root containing litert_lm/c/engine.h)")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Layer 4: genai-lib
#
# libllmengine — LlmEngine wrapping GenieDialog (replaces libllmservice)
# libvlmengine — VlmEngine wrapping GeniePipeline (replaces libvlmservice)
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_GENAILIB)

    # Locate libGenie.so
    find_library(GENIE_LIB
        NAMES Genie libGenie
        PATHS ${GENIE_LIB_PATH}
        NO_DEFAULT_PATH
        REQUIRED
    )
    message(STATUS "[qai-forge] Found libGenie: ${GENIE_LIB}")

    # ── libllmengine ───────────────────────────────────────────────────────────
    # Replaces libllmservice. Uses LlmEngine (Pimpl) — no C interface.
    add_library(llmengine SHARED
        lib/src/llm-engine.cpp
    )
    target_include_directories(llmengine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
            $<INSTALL_INTERFACE:include/genai-lib>
        PRIVATE
            ${GENIE_INCLUDE_PATH}
    )
    target_link_libraries(llmengine PRIVATE ${GENIE_LIB})
    set_target_properties(llmengine PROPERTIES
        OUTPUT_NAME "llmengine"
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )

    # ── libvlmengine ───────────────────────────────────────────────────────────
    # Replaces libvlmservice. Uses VlmEngine (Pimpl) — no C interface.
    add_library(vlmengine SHARED
        lib/src/vlm-engine.cpp
    )
    target_include_directories(vlmengine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
            $<INSTALL_INTERFACE:include/genai-lib>
        PRIVATE
            ${GENIE_INCLUDE_PATH}
    )
    target_link_libraries(vlmengine PRIVATE ${GENIE_LIB} nlohmann_json::nlohmann_json)
    set_target_properties(vlmengine PROPERTIES
        OUTPUT_NAME "vlmengine"
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )

endif() # QAI_FORGE_BUILD_GENAILIB

# ─────────────────────────────────────────────────────────────────────────────
# Layer 2-3: libqai_forge
#
# The SDK library consumed by chatcompletions-server and responses-server.
# Does NOT link genai-lib — only the worker binaries do.
# ─────────────────────────────────────────────────────────────────────────────
set(QAI_FORGE_SOURCES
    # C API — Flat C FFI bridge to Layer 2 (Python/Go/Rust/C bindings)
    src/c_api/qai_forge_c_api.cpp

    # Adapters (Layer 2 — Model Adapter Pattern)
    src/adapters/ModelAdapterFactory.cpp
    src/adapters/Qwen25Adapter.cpp
    src/adapters/Qwen3Adapter.cpp

    # Vision Preprocessing Adapters (Layer 2 — VLM Patch Ordering)
    src/adapters/VisionPreprocessAdapterFactory.cpp
    src/adapters/DefaultVisionPreprocessAdapter.cpp
    src/adapters/QwenVisionPreprocessAdapter.cpp

    # Managers (Layer 2)
    src/managers/MetricsManager.cpp
    src/managers/ModelConfigManager.cpp
    src/managers/SystemResourceManager.cpp

    # Orchestration (Layer 2)
    src/orchestration/ChatOrchestratorImpl.cpp
    src/orchestration/ConcurrencyMiddleware.cpp
    src/orchestration/SummarizationMiddleware.cpp

    # Reasoning (Layer 2)
    src/reasoning/ReasoningRouter.cpp
    src/reasoning/ReasoningBudgetCalculator.cpp

    # Session (Layer 2)
    src/session/SessionManager.cpp

    # Utils (Layer 2)
    src/utils/ImageUtils.cpp
    src/utils/PrivilegeDrop.cpp

    # Backend abstraction (Layer 3 — IGenerativeBackend implementations)
    src/backend/GenIEBackend.cpp
    src/backend/BackendFactory.cpp

    # Predictive AI backends (Layer 3 — IInferenceBackend implementations)
    src/backend/QNNBackend.cpp
    src/backend/SNPEBackend.cpp
    src/backend/LiteRTBackend.cpp

    # Generative AI backends (Layer 3 — IGenerativeBackend implementations)
    src/backend/LiteRTLMBackend.cpp

    # Routing (Layer 2 — InferenceRouter)
    src/routing/InferenceRouter.cpp

    # Middleware (Layer 2 — pluggable middleware pipeline)
    src/orchestration/ExclusiveLockMiddleware.cpp
    src/orchestration/ContextCompactionMiddleware.cpp

    # Predictive AI orchestration (Layer 2)
    src/orchestration/PredictiveAIOrchestrator.cpp

    # Worker management (Layer 3)
    src/worker/InferenceWorkerManager.cpp
    src/worker/VlmInferenceWorkerManager.cpp
    src/worker/PredictiveWorkerManager.cpp
)

add_library(qai_forge SHARED ${QAI_FORGE_SOURCES})

target_include_directories(qai_forge
    PUBLIC
        # SDK public headers — resolves #include "qai_forge/..."
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # genai-lib headers needed by SummarizationMiddleware and ChatOrchestratorImpl
        # (they include InferenceProtocol.h which uses nlohmann/json)
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        # stb_image.h — header-only image decoder used by ImageUtils.cpp
        ${stb_SOURCE_DIR}
)

target_link_libraries(qai_forge
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        # POSIX: fork/exec, sockets, signals (implicit on Linux, explicit for clarity)
        # pthread is needed for std::mutex, std::condition_variable
        Threads::Threads
)

set_target_properties(qai_forge PROPERTIES
    OUTPUT_NAME "qai_forge"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    # Ensure the library exports its symbols
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ─────────────────────────────────────────────────────────────────────────────
# Worker binaries (Layer 3 → Layer 4)
#
# These are the subprocess binaries spawned by InferenceWorkerManager.
# They link genai-lib directly — no ctypes/CFFI overhead.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_WORKERS AND QAI_FORGE_BUILD_GENAILIB)

    # ── genai-inference-worker (LLM) ───────────────────────────────────────────
    add_executable(genai-inference-worker
        src/worker/llm/main.cpp
    )
    target_include_directories(genai-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        # Logger.h lives in the SDK public headers; workers inherit LOG_LEVEL from parent
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(genai-inference-worker PRIVATE
        llmengine
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    # ── genai-vlm-inference-worker (VLM) ──────────────────────────────────────
    add_executable(genai-vlm-inference-worker
        src/worker/vlm/main.cpp
    )
    target_include_directories(genai-vlm-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        # Logger.h lives in the SDK public headers; workers inherit LOG_LEVEL from parent
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(genai-vlm-inference-worker PRIVATE
        vlmengine
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

endif() # QAI_FORGE_BUILD_WORKERS

# ─────────────────────────────────────────────────────────────────────────────
# QNN engine + worker (Predictive AI — QNN context binary)
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_QNN AND QAI_FORGE_BUILD_WORKERS)

    # ── libqnnengine ───────────────────────────────────────────────────────────
    # Wraps QNN C API (dlopen-based). Links only dl — QNN backend loaded at runtime.
    add_library(qnnengine SHARED
        lib/src/qnn-engine.cpp
    )
    target_include_directories(qnnengine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
            $<INSTALL_INTERFACE:include/genai-lib>
        PRIVATE
            ${QNN_INCLUDE_PATH}
    )
    target_link_libraries(qnnengine PRIVATE dl)
    set_target_properties(qnnengine PROPERTIES
        OUTPUT_NAME "qnnengine"
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )

    # ── qnn-inference-worker ───────────────────────────────────────────────────
    add_executable(qnn-inference-worker
        src/worker/qnn/main.cpp
    )
    target_include_directories(qnn-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${QNN_INCLUDE_PATH}
    )
    target_link_libraries(qnn-inference-worker PRIVATE
        qnnengine
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qnnengine
            EXPORT qai_forgeTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
        install(TARGETS qnn-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

endif() # QAI_FORGE_BUILD_QNN

# ─────────────────────────────────────────────────────────────────────────────
# SNPE engine + worker (Predictive AI — SNPE DLC container)
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_SNPE AND QAI_FORGE_BUILD_WORKERS)

    # ── libsnpeengine ──────────────────────────────────────────────────────────
    # Wraps SNPE C API (dlopen-based). Links only dl — libSNPE.so loaded at runtime.
    add_library(snpeengine SHARED
        lib/src/snpe-engine.cpp
    )
    target_include_directories(snpeengine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
            $<INSTALL_INTERFACE:include/genai-lib>
        PRIVATE
            ${SNPE_INCLUDE_PATH}
    )
    target_link_libraries(snpeengine PRIVATE dl)
    set_target_properties(snpeengine PROPERTIES
        OUTPUT_NAME "snpeengine"
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )

    # ── snpe-inference-worker ──────────────────────────────────────────────────
    add_executable(snpe-inference-worker
        src/worker/snpe/main.cpp
    )
    target_include_directories(snpe-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${SNPE_INCLUDE_PATH}
    )
    target_link_libraries(snpe-inference-worker PRIVATE
        snpeengine
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS snpeengine
            EXPORT qai_forgeTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
        install(TARGETS snpe-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

endif() # QAI_FORGE_BUILD_SNPE

# ─────────────────────────────────────────────────────────────────────────────
# LiteRT engine + worker (Predictive AI — .tflite with NPU dispatch)
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LITERT AND QAI_FORGE_BUILD_WORKERS)

    if(NOT LITERT_INCLUDE_PATH)
        message(FATAL_ERROR
            "[qai-forge] QAI_FORGE_BUILD_LITERT=ON but LITERT_INCLUDE_PATH is not set.\n"
            "  Pass -DLITERT_INCLUDE_PATH=<path> where <path> contains litert/c/ headers.\n"
            "  Example: -DLITERT_INCLUDE_PATH=/usr/include")
    endif()

    # Locate liblitert_runtime_c_api.so — built by the LiteRT_builder Docker stage
    # and installed to /usr/lib inside the container.
    find_library(LITERT_RUNTIME_LIB
        NAMES litert_runtime_c_api LiteRt litert
        PATHS /usr/lib ${LITERT_INCLUDE_PATH}/../lib
        NO_DEFAULT_PATH
        REQUIRED
    )
    message(STATUS "[qai-forge] Found LiteRT runtime: ${LITERT_RUNTIME_LIB}")

    # ── liblitertengine ────────────────────────────────────────────────────────
    # Wraps the LiteRT C API (CompiledModel path). Links liblitert_runtime_c_api.so.
    add_library(litertengine SHARED
        lib/src/litert-engine.cpp
    )
    target_include_directories(litertengine
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/lib/include>
            $<INSTALL_INTERFACE:include/genai-lib>
        PRIVATE
            ${LITERT_INCLUDE_PATH}
    )
    target_link_libraries(litertengine PRIVATE ${LITERT_RUNTIME_LIB})
    set_target_properties(litertengine PROPERTIES
        OUTPUT_NAME "litertengine"
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
    )

    # ── litert-inference-worker ────────────────────────────────────────────────
    add_executable(litert-inference-worker
        src/worker/litert/main.cpp
    )
    target_include_directories(litert-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LITERT_INCLUDE_PATH}
    )
    target_link_libraries(litert-inference-worker PRIVATE
        litertengine
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    # ── Install LiteRT engine + worker ─────────────────────────────────────────
    if(QAI_FORGE_INSTALL)
        install(TARGETS litertengine
            EXPORT qai_forgeTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
        install(TARGETS litert-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

endif() # QAI_FORGE_BUILD_LITERT

# ─────────────────────────────────────────────────────────────────────────────
# LiteRT-LM worker (generative AI — LLM inference via LiteRT-LM)
#
# litert-lm-inference-worker uses LiteRT-LM's LlmInference C++ API.
# LiteRT-LM is built in the LiteRTLM_builder Docker stage with
# --override_repository=litert=<our LiteRT source> so it reuses the same
# LiteRT version as the rest of the stack.
#
# The worker currently links against llmengine as a placeholder.
# Replace with a dedicated litert-lm engine library once the LiteRT-LM
# C++ API headers are available at build time.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LITERT_LM)

    if(NOT LITERT_LM_INCLUDE_PATH)
        message(FATAL_ERROR
            "[qai-forge] QAI_FORGE_BUILD_LITERT_LM=ON but LITERT_LM_INCLUDE_PATH is not set.\n"
            "  Pass -DLITERT_LM_INCLUDE_PATH=<path> where <path> contains litert_lm/c/engine.h.\n"
            "  Example: -DLITERT_LM_INCLUDE_PATH=/usr/include")
    endif()

    # Locate the LiteRT-LM C API shared library — built by LiteRTLM_builder stage.
    # The library name depends on the Bazel target; try common names.
    # Not REQUIRED — if LiteRT-LM hasn't been built yet, skip the worker gracefully.
    find_library(LITERT_LM_LIB
        NAMES litert_lm_c_api litert_lm LiteRtLm
        PATHS /usr/lib ${LITERT_LM_INCLUDE_PATH}/../lib
        NO_DEFAULT_PATH
    )

    if(NOT LITERT_LM_LIB)
        message(WARNING
            "[qai-forge] LiteRT-LM library not found in /usr/lib — "
            "litert-lm-inference-worker will NOT be built. "
            "Ensure LiteRTLM_builder stage completed successfully.")
    else()
        message(STATUS "[qai-forge] Found LiteRT-LM library: ${LITERT_LM_LIB}")
    endif()

    if(LITERT_LM_LIB)
    add_executable(litert-lm-inference-worker
        src/worker/litert-lm/main.cpp
    )
    target_include_directories(litert-lm-inference-worker PRIVATE
        ${LITERT_LM_INCLUDE_PATH}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(litert-lm-inference-worker PRIVATE
        ${LITERT_LM_LIB}
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    # liblitert_lm_c_api.so has transitive Rust/prebuilt deps that are resolved
    # at runtime via the dynamic linker (RPATH or LD_LIBRARY_PATH). Allow the
    # static linker to proceed even if those symbols appear unresolved at link time.
    target_link_options(litert-lm-inference-worker PRIVATE
        -Wl,--allow-shlib-undefined
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS litert-lm-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
    endif() # LITERT_LM_LIB

endif() # QAI_FORGE_BUILD_LITERT_LM

# Note: Transport servers (gRPC, D-Bus, HTTP) are built as standalone
# applications in their own top-level directories (grpc-server/, dbus-server/,
# chatcompletions/, responses/). They link against libqai_forge via
# find_package(qai_forge). This keeps the SDK free of transport-specific
# dependencies (Drogon, gRPC, sdbus-c++).

# ─────────────────────────────────────────────────────────────────────────────
# Install & CMake package export
#
# After install, downstream projects can use:
#   find_package(qai_forge REQUIRED)
#   target_link_libraries(my_target PRIVATE qai_forge::qai_forge)
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    # ── Install SDK library ────────────────────────────────────────────────────
    install(TARGETS qai_forge
        EXPORT qai_forgeTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # ── Install public headers ─────────────────────────────────────────────────
    install(DIRECTORY include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )

    # ── Install genai-lib headers (needed by worker binaries) ─────────────────
    install(DIRECTORY lib/include/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/genai-lib
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp"
    )

    # ── Install genai-lib shared libraries ────────────────────────────────────
    if(QAI_FORGE_BUILD_GENAILIB)
        install(TARGETS llmengine vlmengine
            EXPORT qai_forgeTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    # ── Install worker binaries ────────────────────────────────────────────────
    if(QAI_FORGE_BUILD_WORKERS AND QAI_FORGE_BUILD_GENAILIB)
        install(TARGETS genai-inference-worker genai-vlm-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    # ── CMake package config ───────────────────────────────────────────────────
    install(EXPORT qai_forgeTargets
        FILE qai_forgeTargets.cmake
        NAMESPACE qai_forge::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qai_forge
    )

    configure_package_config_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/qai_forgeConfig.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/qai_forgeConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qai_forge
    )

    write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/qai_forgeConfigVersion.cmake"
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )

    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/qai_forgeConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/qai_forgeConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/qai_forge
    )

endif() # QAI_FORGE_INSTALL
