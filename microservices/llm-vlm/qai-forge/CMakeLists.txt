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
#   QAI_FORGE_INSTALL        — Install targets (default ON)
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.23)
project(qai_forge VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Build options ─────────────────────────────────────────────────────────────
option(QAI_FORGE_BUILD_WORKERS  "Build genai-inference-worker binaries" ON)
option(QAI_FORGE_BUILD_GENIE    "Build GenIE generative backend + worker" OFF)
option(QAI_FORGE_BUILD_QNN       "Build QNN Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_SNPE      "Build SNPE Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_LITERT    "Build LiteRT Predictive AI engine + worker" OFF)
option(QAI_FORGE_BUILD_LITERT_LM "Build LiteRT-LM generative worker" OFF)
option(QAI_FORGE_BUILD_LLAMACPP  "Build llama.cpp backend support" OFF)
option(QAI_FORGE_INSTALL         "Install targets and CMake package config" ON)
option(QAI_FORGE_BUILD_TOPOLOGY_PROBE "Build the standalone topology-probe CLI" ON)
option(QAI_FORGE_BUILD_ADAPTER_QWEN  "Build Qwen model-adapter plugin (2.5+3+vision)" ON)
option(QAI_FORGE_BUILD_ADAPTER_GEMMA "Build Gemma model-adapter plugin" ON)

# ── Required dependencies ─────────────────────────────────────────────────────
find_package(Threads REQUIRED)

include(FetchContent)

# nlohmann/json is pinned via FetchContent (not find_package) so every
# qai-forge target links the exact same ABI — mixed versions produce
# unlinkable dlopen() "undefined symbol" errors across plugin .so files.
#
# NOT fetched during CMake configure. The source must be pre-fetched
# (pinned commit 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03, i.e. tag
# v3.11.3) by whichever build path is in use (Dockerfile RUN step or
# scripts/build.sh) and its location passed in via
# -DNLOHMANN_JSON_SOURCE_DIR=<path>. This avoids any network/git activity
# at configure time.
set(NLOHMANN_JSON_SOURCE_DIR "" CACHE PATH "Path to pre-fetched nlohmann/json source (must contain CMakeLists.txt)")
if(NOT NLOHMANN_JSON_SOURCE_DIR)
    message(FATAL_ERROR
        "[qai-forge] NLOHMANN_JSON_SOURCE_DIR is not set.\n"
        "  nlohmann/json must be pre-fetched (pinned commit 9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03,\n"
        "  tag v3.11.3) before configuring qai-forge. Pass:\n"
        "  -DNLOHMANN_JSON_SOURCE_DIR=<path-containing-CMakeLists.txt>")
endif()

set(JSON_Install ON CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
# Content-name stays "json" (unchanged from the previous live-fetch version)
# — minja's own CMakeLists.txt (see the minja block below) later does an
# identical FetchContent_Declare(json ...) + FetchContent_MakeAvailable(json)
# call. Because we've already declared+populated "json" here, FetchContent
# treats minja's call as a no-op instead of trying to fetch its own copy.
# SOURCE_DIR points at the pre-fetched local checkout — MakeAvailable() then
# only runs add_subdirectory(), with no git clone/fetch of any kind.
FetchContent_Declare(json SOURCE_DIR "${NLOHMANN_JSON_SOURCE_DIR}")
FetchContent_MakeAvailable(json)

# ── stb_image (header-only image decoder — JPEG, PNG, WEBP, BMP, GIF) ────────
# Compiled into libqai_forge (no runtime .so). stb_image.h is defined in
# exactly one .cpp file (ImageUtils.cpp).
#
# NOT fetched during CMake configure. The source must be pre-fetched
# (pinned commit 2c980bb59875b0d32144a71867fbdebb2f77cd20) by whichever build
# path is in use (Dockerfile RUN step or scripts/build.sh) and its location
# passed in via -DSTB_SOURCE_DIR=<path>. This avoids any network/git activity
# at configure time.
include(FetchContent)
set(STB_SOURCE_DIR "" CACHE PATH "Path to pre-fetched stb source (must contain stb_image.h)")
if(NOT STB_SOURCE_DIR)
    message(FATAL_ERROR
        "[qai-forge] STB_SOURCE_DIR is not set.\n"
        "  stb must be pre-fetched (pinned commit 2c980bb59875b0d32144a71867fbdebb2f77cd20)\n"
        "  before configuring qai-forge. Pass:\n"
        "  -DSTB_SOURCE_DIR=<path-containing-stb_image.h>")
endif()
set(stb_SOURCE_DIR "${STB_SOURCE_DIR}")

# ── minja (MIT — purpose-built Jinja2 subset for HuggingFace chat templates) ──
# Used by LiteRTLMOrchestrator::renderPrompt() to render each model's chat
# template (Jinja2 syntax, as provided by the LiteRT-LM worker's METADATA
# message) into a flat prompt string. Only needed when LiteRT-LM support is
# enabled — LiteRTLMOrchestrator.cpp (the only consumer) is compiled behind
# QAI_FORGE_BUILD_LITERT_LM.
# Header-only INTERFACE target — no shared library to build/install/RPATH.
#
# NOT fetched during CMake configure. The source must be pre-fetched
# (pinned commit 021c2293c187789ef13d56c6cfd89c9b134fd80f) by whichever build
# path is in use (Dockerfile RUN step or scripts/build.sh) and its location
# passed in via -DMINJA_SOURCE_DIR=<path>. This avoids any network/git
# activity at configure time.
if(QAI_FORGE_BUILD_LITERT_LM)
    set(MINJA_SOURCE_DIR "" CACHE PATH "Path to pre-fetched minja source (must contain CMakeLists.txt)")
    if(NOT MINJA_SOURCE_DIR)
        message(FATAL_ERROR
            "[qai-forge] QAI_FORGE_BUILD_LITERT_LM=ON but MINJA_SOURCE_DIR is not set.\n"
            "  minja must be pre-fetched (pinned commit 021c2293c187789ef13d56c6cfd89c9b134fd80f)\n"
            "  before configuring qai-forge. Pass:\n"
            "  -DMINJA_SOURCE_DIR=<path-containing-minja/CMakeLists.txt>")
    endif()

    # minja's own CMakeLists.txt unconditionally does its own
    # FetchContent_Declare(json ...) + FetchContent_MakeAvailable(json).
    # Since we've already FetchContent_Declare(json ...)'d and populated
    # the exact same content-name ("json") above with our pinned tag,
    # minja's identical call is a no-op — FetchContent treats it as
    # already-populated and minja links against our pinned target
    # instead of fetching its own. No stub workaround needed.
    set(MINJA_TEST_ENABLED OFF CACHE BOOL "" FORCE)
    set(MINJA_EXAMPLE_ENABLED OFF CACHE BOOL "" FORCE)
    # SOURCE_DIR points FetchContent at the pre-fetched local checkout —
    # FetchContent_MakeAvailable() then only runs add_subdirectory(), with no
    # git clone/fetch of any kind.
    FetchContent_Declare(minja SOURCE_DIR "${MINJA_SOURCE_DIR}")
    FetchContent_MakeAvailable(minja)
endif()

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

# ── llama.cpp SDK paths ───────────────────────────────────────────────────────
# Set LLAMACPP_INCLUDE_PATH on the cmake command line:
#   cmake -DLLAMACPP_INCLUDE_PATH=/install/include
# The include root must contain ggml/include/gguf.h (from llama.cpp install).
if(NOT DEFINED LLAMACPP_INCLUDE_PATH)
    set(LLAMACPP_INCLUDE_PATH "" CACHE PATH "Path to llama.cpp headers (root containing ggml/include/gguf.h)")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Layer 4: genai-lib
#
# libllmengine — LlmEngine wrapping GenieDialog (replaces libllmservice)
# libvlmengine — VlmEngine wrapping GeniePipeline (replaces libvlmservice)
#
# Nested under QAI_FORGE_BUILD_GENIE — genai-lib (and the worker binaries
# that link it, below) only exist to serve the GenIE generative backend.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_GENIE)

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

endif() # QAI_FORGE_BUILD_GENIE

# ─────────────────────────────────────────────────────────────────────────────
# Layer 2-3: libqai_forge
#
# The SDK library consumed by chatcompletions-server and responses-server.
# Does NOT link genai-lib — only the worker binaries do.
# ─────────────────────────────────────────────────────────────────────────────
set(QAI_FORGE_SOURCES
    # Public facade — unified entry point for all inference
    src/QaiForge.cpp

    # C API — Flat C FFI bridge to Layer 2 (Python/Go/Rust/C bindings)
    src/c_api/qai_forge_c_api.cpp

    # Adapters (Layer 2 — Model Adapter Pattern)
    # Qwen25Adapter.cpp/Qwen3Adapter.cpp/ToolCallJsonUtils.cpp/
    # QwenVisionPreprocessAdapter.cpp (qaiforge_plugin_adapter_qwen) and
    # GemmaAdapter.cpp (qaiforge_plugin_adapter_gemma) have moved into their
    # own plugin targets (see the "Runtime plugins" section below) —
    # ModelAdapterFactory/VisionPreprocessAdapterFactory now resolve
    # adapters via PluginRegistry instead of a compile-time type list.
    # DefaultAdapter.h (header-only) stays in core as the always-available
    # fallback.
    src/adapters/ModelAdapterFactory.cpp

    # Shared helper used by multiple adapter plugins (Qwen25Adapter,
    # Qwen3Adapter, GemmaAdapter) — kept in core so each plugin resolves it
    # via the qai_forge.so link rather than depending on another plugin.
    src/adapters/ToolCallJsonUtils.cpp

    # Vision Preprocessing Adapters (Layer 2 — VLM Patch Ordering)
    src/adapters/VisionPreprocessAdapterFactory.cpp
    src/adapters/DefaultVisionPreprocessAdapter.cpp

    # Managers (Layer 2)
    src/managers/MetricsManager.cpp
    src/managers/ModelConfigManager.cpp
    src/managers/SystemResourceManager.cpp

    # Scheduler (Layer 2 — model residency, job queuing, tool-chain continuity)
    src/scheduler/PriorityModelQueue.cpp
    src/scheduler/ConversationMemoryCoordinator.cpp
    src/scheduler/ModelLoadCoordinator.cpp
    src/scheduler/ToolChainTable.cpp
    src/scheduler/ToolChainLeaseManager.cpp
    src/scheduler/ActiveOperationRegistry.cpp
    src/scheduler/EvictionPolicy.cpp
    src/scheduler/ModelRuntime.cpp
    src/scheduler/PostTurnWorker.cpp
    src/scheduler/WarmModelPool.cpp
    src/scheduler/InferenceScheduler.cpp
    src/scheduler/PlacementAdmissionGateway.cpp

    # Predictive AI scheduler (Phase 4 — pool + runtime for tensor inference)
    src/scheduler/PredictiveModelRuntime.cpp
    src/scheduler/PredictiveModelPool.cpp

    # Resource/topology layer (device placement)
    src/resources/ResourceTypes.cpp
    src/resources/TopologyRegistry.cpp
    src/resources/StandardTopologies.cpp
    src/resources/ResourceManager.cpp
    $<IF:$<BOOL:${QAI_FORGE_BUILD_QNN}>,${CMAKE_CURRENT_SOURCE_DIR}/src/resources/QnnTopologyProbe.cpp,${CMAKE_CURRENT_SOURCE_DIR}/src/resources/QnnTopologyProbeStub.cpp>

    # Execution/placement layer (device placement)
    src/execution/ExecutionPlan.cpp
    src/execution/ProviderRegistry.cpp
    src/execution/ExecutionPlanResolver.cpp
    src/execution/RuntimeArtifacts.cpp
    src/execution/QnnLaunchOptions.cpp
    src/execution/SnpeLaunchOptions.cpp
    src/execution/LiteRtLaunchOptions.cpp
    src/execution/LiteRtLmLaunchOptions.cpp
    src/execution/LlamaCppLaunchOptions.cpp

    # Orchestration (Layer 2)
    src/orchestration/PredictiveOrchestrator.cpp
    # OrchestratorAdapter — internal glue between the public
    # qai_forge::IGenerativeOrchestrator ABI (implemented by each runtime
    # plugin's concrete orchestrator) and the scheduler (ModelRuntime/
    # WarmModelPool/PostTurnWorker). Core, runtime-agnostic — stays in
    # qai_forge.so regardless of which generative runtime plugins are built.
    src/orchestration/OrchestratorAdapter.cpp

    # BackendAdapter / PredictiveBackendAdapter — internal glue between the
    # public qai_forge::IGenerativeBackend / qai_forge::IInferenceBackend
    # ABIs (implemented by each runtime plugin's concrete backend) and the
    # scheduler (ModelRuntime/PostTurnWorker for generative,
    # PredictiveModelRuntime for predictive). Core, runtime-agnostic — stay
    # in qai_forge.so regardless of which backend plugins are built. Mirror
    # OrchestratorAdapter.cpp's role on the backend side.
    src/backend/BackendAdapter.cpp
    src/backend/PredictiveBackendAdapter.cpp

    # Reasoning (Layer 2)
    src/reasoning/ReasoningRouter.cpp
    src/reasoning/ReasoningBudgetCalculator.cpp

    # Utils (Layer 2)
    src/utils/ImageUtils.cpp
    src/utils/PrivilegeDrop.cpp
    src/utils/UseLock.cpp

    # Backend abstraction (Layer 3 — IGenerativeBackend implementations)
    # BackendFactory now delegates entirely to PluginRegistry — it no
    # longer needs compile-time knowledge of any concrete backend type.
    src/backend/BackendFactory.cpp
    src/plugin/PluginRegistry.cpp
    src/plugin/PluginLoader.cpp

    # Predictive AI backend shared infra (Layer 3) — PredictiveSharedMemory
    # and the PredictiveWorkerChannel/ProcessPool below are used by all
    # predictive plugins (QNN/SNPE/LiteRT); QNNBackend.cpp/SNPEBackend.cpp/
    # LiteRTBackend.cpp themselves have moved into their own plugin targets
    # (see the qaiforge_plugin_qnn/snpe/litert blocks below).
    src/backend/PredictiveSharedMemory.cpp

    # Worker management (Layer 3)
    # NOTE: InferenceWorkerManager.cpp is the shared base class used by
    # VlmInferenceWorkerManager (GenIE plugin), LiteRTLMWorkerManager
    # (LiteRT-LM plugin), and LlamaCppWorkerManager (llama.cpp plugin) — it
    # must remain in core so any subset of those plugins can be dlopen'd
    # independently.
    src/worker/InferenceWorkerManager.cpp
    src/worker/PredictiveWorkerChannel.cpp
    src/worker/PredictiveWorkerProcessPool.cpp
)

# ── Conditionally add GenIE generative backend sources ───────────────────────
# GenIE is Qualcomm's own SDK — the one runtime historically always built in.
# Gated the same way as the other optional runtimes so a shared-rootfs
# libqai_forge.so's compiled-in capability set (as reported by
# BackendFactory::supportedRuntimes()) always matches reality, and so a
# from-source build with no GenIE SDK available can still configure cleanly.
if(QAI_FORGE_BUILD_GENIE)
    # GenIEBackend.cpp, GenieOrchestrator.cpp, GenieConfigMaterializer.cpp,
    # and VlmInferenceWorkerManager.cpp have moved into the qaiforge_plugin_genie
    # target (see the "Runtime plugins" section below) — they no longer
    # compile into the core qai_forge.so.

    # Define preprocessor macro for conditional compilation. Still needed —
    # GenIEBackend.h/GenieOrchestrator.h are guarded by #ifdef
    # QAI_FORGE_BUILD_GENIE, and this directory-scoped definition applies to
    # the qaiforge_plugin_genie target as well (CMake directory-level compile
    # definitions apply to every target in the same CMakeLists.txt file,
    # regardless of definition order).
    add_compile_definitions(QAI_FORGE_BUILD_GENIE)

    message(STATUS "[qai-forge] GenIE generative backend support enabled")
endif()

# ── Conditionally add LiteRT-LM generative backend sources ───────────────────
# LiteRTLMOrchestrator.cpp depends on minja (see the minja FetchContent
# block above, itself gated on this same flag) — gating these together
# ensures a non-LiteRT-LM build never pulls in the minja dependency.
if(QAI_FORGE_BUILD_LITERT_LM)
    # LiteRTLMBackend.cpp, LiteRTLMOrchestrator.cpp, and
    # LiteRTLMWorkerManager.cpp have moved into the qaiforge_plugin_litertlm
    # target (see the "Runtime plugins" section below).

    # Define preprocessor macro for conditional compilation. Still needed —
    # applies directory-wide, including to the qaiforge_plugin_litertlm target.
    add_compile_definitions(QAI_FORGE_BUILD_LITERT_LM)

    message(STATUS "[qai-forge] LiteRT-LM generative backend support enabled")
endif()

# ── Conditionally add llama.cpp backend sources ──────────────────────────────
if(QAI_FORGE_BUILD_LLAMACPP)
    # LlamaCppBackend.cpp, LlamaCppOrchestrator.cpp, and
    # LlamaCppWorkerManager.cpp have moved into the qaiforge_plugin_llamacpp
    # target (see the "Runtime plugins" section below). GgufMetadataReader.cpp
    # stays in core — it's a standalone zero-copy header parser with no
    # dependency on the plugin split, and other code may want to use it
    # independently of whether the llamacpp plugin itself loads.
    list(APPEND QAI_FORGE_SOURCES
        # GGUF metadata reader (Layer 2 — zero-copy header parser)
        src/utils/GgufMetadataReader.cpp
    )

    # Define preprocessor macro for conditional compilation. Still needed —
    # applies directory-wide, including to the qaiforge_plugin_llamacpp target.
    add_compile_definitions(QAI_FORGE_BUILD_LLAMACPP)

    message(STATUS "[qai-forge] llama.cpp backend support enabled")
endif()

add_library(qai_forge SHARED ${QAI_FORGE_SOURCES})

target_include_directories(qai_forge
    PUBLIC
        # SDK public headers — resolves #include "qai_forge/..." for the
        # stable facade surface (QaiForge.h, InternalDTOs.h, c_api/, etc.)
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Internal SDK headers (backend/, orchestration/, scheduler/, worker/,
        # execution/, resources/, reasoning/, session/, and the internal-only
        # managers/utils/adapters split off from the public tree above).
        # Build-time only — never added to INSTALL_INTERFACE, so it is never
        # exported via qai_forgeConfig.cmake/qai_forgeTargets.cmake. External
        # consumers (including future standalone processes on a shared
        # rootfs) can only ever see and link against the public facade;
        # they cannot #include these headers or depend on symbols whose
        # existence varies by QAI_FORGE_BUILD_* flag combination.
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
        # genai-lib headers needed by SummarizationMiddleware and GenieOrchestrator
        # (they include InferenceProtocol.h which uses nlohmann/json)
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
        # stb_image.h — header-only image decoder used by ImageUtils.cpp
        ${stb_SOURCE_DIR}
)

# ── Add QNN include path if enabled (QnnTopologyProbe.cpp needs QnnInterface.h) ─
if(QAI_FORGE_BUILD_QNN)
    target_include_directories(qai_forge PRIVATE ${QNN_INCLUDE_PATH})
    message(STATUS "[qai-forge] Added QNN include path to qai_forge: ${QNN_INCLUDE_PATH}")

    # Define preprocessor macro for conditional compilation. Mirrors the
    # QAI_FORGE_BUILD_LITERT_LM/_LLAMACPP pattern above — this lets
    # BackendFactory::supportedRuntimes() (and any other compile-time check)
    # accurately report whether QNN predictive-runtime support was actually
    # built into this .so, since QNNBackend.cpp is otherwise unconditionally
    # compiled in regardless of this flag (only the qnn-inference-worker
    # binary itself is gated on QAI_FORGE_BUILD_QNN).
    add_compile_definitions(QAI_FORGE_BUILD_QNN)
endif()

# ── Add llama.cpp include path if enabled ────────────────────────────────────
if(QAI_FORGE_BUILD_LLAMACPP)
    if(NOT LLAMACPP_INCLUDE_PATH)
        message(FATAL_ERROR
            "[qai-forge] QAI_FORGE_BUILD_LLAMACPP=ON but LLAMACPP_INCLUDE_PATH is not set.\n"
            "  Pass -DLLAMACPP_INCLUDE_PATH=<path> where <path> contains ggml/include/gguf.h.\n"
            "  Example: -DLLAMACPP_INCLUDE_PATH=/install/include")
    endif()

    target_include_directories(qai_forge PRIVATE
            ${LLAMACPP_INCLUDE_PATH}
	    ${LLAMACPP_INCLUDE_PATH}/ggml/include
    )
    message(STATUS "[qai-forge] Added llama.cpp include path: ${LLAMACPP_INCLUDE_PATH}")

    # Locate libllama, libggml, and its modular dependencies (base, cpu, common)
    find_library(LLAMACPP_LLAMA_LIB
        NAMES llama libllama
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )
    find_library(LLAMACPP_GGML_LIB
        NAMES ggml libggml
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )
    find_library(LLAMACPP_GGML_BASE_LIB
        NAMES ggml-base libggml-base
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )
    find_library(LLAMACPP_GGML_CPU_LIB
        NAMES ggml-cpu libggml-cpu
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )
    find_library(LLAMACPP_COMMON_LIB
        NAMES llama-common libllama-common common libcommon
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )
    # mtmd: multimodal (vision/--mmproj) support, only linked into
    # llamacpp-inference-worker (see its target block below) — llama.cpp
    # builds this by default alongside libllama/libggml.
    find_library(LLAMACPP_MTMD_LIB
        NAMES mtmd libmtmd
        HINTS
            ${LLAMACPP_INCLUDE_PATH}/../lib
            ${CMAKE_PREFIX_PATH}/lib
            ${CMAKE_PREFIX_PATH}
        PATHS
            /usr/lib
            /usr/local/lib
            /usr/lib/aarch64-linux-gnu
    )

    if (NOT LLAMACPP_LLAMA_LIB OR NOT LLAMACPP_GGML_LIB)
        message(WARNING "[qai-forge] Core llama.cpp libraries (libllama/libggml) not found. Build may fail during linking.")
    else()
        message(STATUS "[qai-forge] Found libllama: ${LLAMACPP_LLAMA_LIB}")
        message(STATUS "[qai-forge] Found libggml: ${LLAMACPP_GGML_LIB}")

        if (LLAMACPP_GGML_BASE_LIB)
            message(STATUS "[qai-forge] Found libggml-base: ${LLAMACPP_GGML_BASE_LIB}")
        endif()
        if (LLAMACPP_GGML_CPU_LIB)
            message(STATUS "[qai-forge] Found libggml-cpu: ${LLAMACPP_GGML_CPU_LIB}")
        endif()
        if (LLAMACPP_COMMON_LIB)
            message(STATUS "[qai-forge] Found libllama-common: ${LLAMACPP_COMMON_LIB}")
        endif()
        if (LLAMACPP_MTMD_LIB)
            message(STATUS "[qai-forge] Found libmtmd: ${LLAMACPP_MTMD_LIB}")
        else()
            message(WARNING "[qai-forge] libmtmd not found — llamacpp-inference-worker will build without vision/mmproj support.")
        endif()
    endif()
endif()

# minja is a header-only INTERFACE target — target_link_libraries() alone
# propagates its include directories automatically. Only needed when
# LiteRTLMOrchestrator.cpp (the sole consumer) is actually compiled.
if(QAI_FORGE_BUILD_LITERT_LM)
    target_link_libraries(qai_forge PRIVATE
        # minja template engine — used by LiteRTLMOrchestrator::renderPrompt()
        minja
    )
endif()

target_link_libraries(qai_forge
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        Threads::Threads
        # dlopen/dlsym/dlclose/dlerror — used by PluginLoader.cpp to discover
        # and load qaiforge_plugin_*.so files at startup.
        dl
)

if(QAI_FORGE_BUILD_LLAMACPP AND LLAMACPP_LLAMA_LIB AND LLAMACPP_GGML_LIB)
    # Important: llama-common must come before llama/ggml in the link order
    # to resolve GGUF symbols properly when statically or modularly linked
    if(LLAMACPP_COMMON_LIB)
        target_link_libraries(qai_forge PUBLIC ${LLAMACPP_COMMON_LIB})
    endif()

    target_link_libraries(qai_forge PUBLIC ${LLAMACPP_LLAMA_LIB} ${LLAMACPP_GGML_LIB})

    # Link modular libraries if they exist on the target system
    if(LLAMACPP_GGML_BASE_LIB)
        target_link_libraries(qai_forge PUBLIC ${LLAMACPP_GGML_BASE_LIB})
    endif()
    if(LLAMACPP_GGML_CPU_LIB)
        target_link_libraries(qai_forge PUBLIC ${LLAMACPP_GGML_CPU_LIB})
    endif()
endif()

set_target_properties(qai_forge PROPERTIES
    OUTPUT_NAME "qai_forge"
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    # Ensure the library exports its symbols
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
    # $ORIGIN kept for any future co-installed runtime .so dependencies
    # (no longer needed for jinja2cpp — minja is header-only).
    BUILD_RPATH "$ORIGIN"
    INSTALL_RPATH "$ORIGIN"
)

# ─────────────────────────────────────────────────────────────────────────────
# Runtime plugins — dlopen-loaded backend/orchestrator implementations
#
# Each plugin is a standalone shared library exporting one extern "C" entry
# point (qai_forge_register_plugin) that registers its runtime(s) against
# PluginRegistry. PluginLoader (core qai_forge.so) discovers and
# dlopen()'s every *.so in the plugin directory at process startup —
# BackendFactory never needs compile-time knowledge of these types.
#
# None of these plugins link the underlying native SDK directly (libGenie,
# libllama, QNN/SNPE/LiteRT libs) — that linkage lives entirely in the
# separate worker subprocess binaries (genai-inference-worker,
# llamacpp-inference-worker, qnn-inference-worker, etc., built further
# below), which the plugin's backend class talks to over IPC. Plugins only
# need qai_forge's own headers/symbols (InferenceWorkerManager,
# PredictiveWorkerProcessPool, ModelConfigManager, DTOs, Logger, etc.).
#
# Each plugin is still gated by its corresponding QAI_FORGE_BUILD_* option —
# unbuilt SDK support ⇒ the plugin target simply isn't configured, exactly
# as the old #ifdef'd source lists behaved, just at the CMake-target level.
# ─────────────────────────────────────────────────────────────────────────────

set(QAI_FORGE_PLUGIN_INSTALL_SUBDIR "lib/qai-forge/plugins" CACHE STRING
    "Relative install destination for runtime plugin shared libraries")

# ── qaiforge_plugin_genie ─────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_GENIE)
    add_library(qaiforge_plugin_genie SHARED
        src/backend/GenIEBackend.cpp
        src/orchestration/GenieOrchestrator.cpp
        src/execution/GenieConfigMaterializer.cpp
        src/worker/VlmInferenceWorkerManager.cpp
        src/backend/plugins/GeniePluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_genie PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
    )
    target_link_libraries(qaiforge_plugin_genie PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_genie PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_genie"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_genie
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_genie")
endif()

# ── qaiforge_plugin_litertlm ──────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LITERT_LM)
    add_library(qaiforge_plugin_litertlm SHARED
        src/backend/LiteRTLMBackend.cpp
        src/orchestration/LiteRTLMOrchestrator.cpp
        src/worker/LiteRTLMWorkerManager.cpp
        src/backend/plugins/LiteRTLMPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_litertlm PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
    )
    target_link_libraries(qaiforge_plugin_litertlm PRIVATE
        qai_forge
        minja
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_litertlm PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_litertlm"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_litertlm
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_litertlm")
endif()

# ── qaiforge_plugin_llamacpp ──────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LLAMACPP)
    add_library(qaiforge_plugin_llamacpp SHARED
        src/backend/LlamaCppBackend.cpp
        src/orchestration/LlamaCppOrchestrator.cpp
        src/worker/LlamaCppWorkerManager.cpp
        src/backend/plugins/LlamaCppPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_llamacpp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/include
    )
    target_link_libraries(qaiforge_plugin_llamacpp PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_llamacpp PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_llamacpp"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_llamacpp
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_llamacpp")
endif()

# ── qaiforge_plugin_qnn ───────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_QNN)
    add_library(qaiforge_plugin_qnn SHARED
        src/backend/QNNBackend.cpp
        src/backend/plugins/QnnPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_qnn PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(qaiforge_plugin_qnn PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_qnn PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_qnn"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_qnn
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_qnn")
endif()

# ── qaiforge_plugin_snpe ──────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_SNPE)
    add_library(qaiforge_plugin_snpe SHARED
        src/backend/SNPEBackend.cpp
        src/backend/plugins/SnpePluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_snpe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(qaiforge_plugin_snpe PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_snpe PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_snpe"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_snpe
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_snpe")
endif()

# ── qaiforge_plugin_litert ────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LITERT)
    add_library(qaiforge_plugin_litert SHARED
        src/backend/LiteRTBackend.cpp
        src/backend/plugins/LiteRTPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_litert PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(qaiforge_plugin_litert PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
        Threads::Threads
    )
    set_target_properties(qaiforge_plugin_litert PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_litert"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_litert
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_litert")
endif()

# ── qaiforge_plugin_adapter_qwen ──────────────────────────────────────────────
# Model-adapter plugin: Qwen2.5/Qwen3 tool-calling adapters + Qwen vision
# preprocessing adapter. Registers against PluginRegistry's model/vision
# adapter extension points (the public, out-of-tree Level-B ABI) rather
# than being wired into BackendFactory. No native SDK dependency.
if(QAI_FORGE_BUILD_ADAPTER_QWEN)
    add_library(qaiforge_plugin_adapter_qwen SHARED
        src/adapters/Qwen25Adapter.cpp
        src/adapters/Qwen3Adapter.cpp
        src/adapters/QwenVisionPreprocessAdapter.cpp
        src/adapters/plugins/QwenAdapterPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_adapter_qwen PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(qaiforge_plugin_adapter_qwen PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
    )
    set_target_properties(qaiforge_plugin_adapter_qwen PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_adapter_qwen"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_adapter_qwen
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_adapter_qwen")
endif()

# ── qaiforge_plugin_adapter_gemma ─────────────────────────────────────────────
# Model-adapter plugin: Gemma tool-calling adapter (LiteRT-LM FC grammar).
if(QAI_FORGE_BUILD_ADAPTER_GEMMA)
    add_library(qaiforge_plugin_adapter_gemma SHARED
        src/adapters/GemmaAdapter.cpp
        src/adapters/plugins/GemmaAdapterPluginEntry.cpp
    )
    target_include_directories(qaiforge_plugin_adapter_gemma PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(qaiforge_plugin_adapter_gemma PRIVATE
        qai_forge
        nlohmann_json::nlohmann_json
    )
    set_target_properties(qaiforge_plugin_adapter_gemma PROPERTIES
        OUTPUT_NAME "qaiforge_plugin_adapter_gemma"
        BUILD_RPATH "$ORIGIN/.."
        INSTALL_RPATH "$ORIGIN/.."
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS qaiforge_plugin_adapter_gemma
            LIBRARY DESTINATION ${QAI_FORGE_PLUGIN_INSTALL_SUBDIR}
        )
    endif()

    message(STATUS "[qai-forge] Built plugin: qaiforge_plugin_adapter_gemma")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Worker binaries (Layer 3 → Layer 4)
#
# These are the subprocess binaries spawned by InferenceWorkerManager.
# They link genai-lib directly — no ctypes/CFFI overhead.
#
# Gated on QAI_FORGE_BUILD_GENIE (in addition to the pre-existing
# QAI_FORGE_BUILD_WORKERS check) — these workers only exist to serve the
# GenIE generative backend.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_GENIE AND QAI_FORGE_BUILD_WORKERS)

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
# topology-probe CLI — standalone hardware topology discovery tool.
# Links against libqai_forge to reuse TopologyRegistry/StandardTopologies/
# QnnTopologyProbe. Does not participate in any inference path.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_TOPOLOGY_PROBE)
    add_executable(topology-probe
        src/tools/topology-probe.cpp
    )
    target_include_directories(topology-probe PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # topology-probe.cpp includes qai_forge/resources/{QnnTopologyProbe,
        # StandardTopologies,TopologyRegistry}.h directly — these are internal
        # SDK headers (see the internal/ include note on the qai_forge target
        # above), so this build-only tool needs the same private search path.
        ${CMAKE_CURRENT_SOURCE_DIR}/internal
    )
    target_link_libraries(topology-probe PRIVATE
        qai_forge
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS topology-probe
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif() # QAI_FORGE_BUILD_TOPOLOGY_PROBE

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

# Define preprocessor macro for conditional compilation, gated only on
# QAI_FORGE_BUILD_SNPE (not QAI_FORGE_BUILD_WORKERS) — SNPEBackend.cpp is
# unconditionally compiled into qai_forge regardless of whether the
# snpe-inference-worker binary itself gets built, so the macro must track
# the SDK-support flag alone for BackendFactory::supportedRuntimes() to be
# accurate even in a QAI_FORGE_BUILD_WORKERS=OFF configuration. Mirrors the
# QAI_FORGE_BUILD_QNN pattern above.
if(QAI_FORGE_BUILD_SNPE)
    add_compile_definitions(QAI_FORGE_BUILD_SNPE)
endif()

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

# Define preprocessor macro for conditional compilation, gated only on
# QAI_FORGE_BUILD_LITERT (not QAI_FORGE_BUILD_WORKERS) — LiteRTBackend.cpp is
# unconditionally compiled into qai_forge regardless of whether the
# litert-inference-worker binary itself gets built. Mirrors the
# QAI_FORGE_BUILD_QNN/_SNPE pattern above so BackendFactory::
# supportedRuntimes() accurately reports LiteRT predictive-runtime support.
if(QAI_FORGE_BUILD_LITERT)
    add_compile_definitions(QAI_FORGE_BUILD_LITERT)
endif()

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
    target_compile_definitions(litert-lm-inference-worker PRIVATE
        LITERT_LM_AVAILABLE
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

# ─────────────────────────────────────────────────────────────────────────────
# llama.cpp worker (generative AI — LLM inference via direct libllama API)
#
# llamacpp-inference-worker links libllama/libggml/libllama-common directly
# (no llama-server, no HTTP/SSE) — mirrors litert-lm-inference-worker's
# structure. Reuses the libllama/libggml libraries already located above for
# qai_forge's own GgufMetadataReader.
# ─────────────────────────────────────────────────────────────────────────────
if(QAI_FORGE_BUILD_LLAMACPP AND LLAMACPP_LLAMA_LIB AND LLAMACPP_GGML_LIB)
    add_executable(llamacpp-inference-worker
        src/worker/llamacpp/main.cpp
    )
    target_include_directories(llamacpp-inference-worker PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${LLAMACPP_INCLUDE_PATH}
        ${LLAMACPP_INCLUDE_PATH}/ggml/include
    )
    target_compile_definitions(llamacpp-inference-worker PRIVATE
        QAI_FORGE_BUILD_LLAMACPP
    )

    # Important: llama-common must come before llama/ggml in the link order
    # to resolve GGUF symbols properly (matches qai_forge's own link order above).
    if(LLAMACPP_COMMON_LIB)
        target_link_libraries(llamacpp-inference-worker PRIVATE ${LLAMACPP_COMMON_LIB})
    endif()

    target_link_libraries(llamacpp-inference-worker PRIVATE
        ${LLAMACPP_LLAMA_LIB}
        ${LLAMACPP_GGML_LIB}
    )

    if(LLAMACPP_GGML_BASE_LIB)
        target_link_libraries(llamacpp-inference-worker PRIVATE ${LLAMACPP_GGML_BASE_LIB})
    endif()
    if(LLAMACPP_GGML_CPU_LIB)
        target_link_libraries(llamacpp-inference-worker PRIVATE ${LLAMACPP_GGML_CPU_LIB})
    endif()

    # mtmd: vision/--mmproj support. Guarded (not a hard requirement) so a
    # build missing libmtmd still produces a text-only worker instead of
    # failing the whole llamacpp backend.
    if(LLAMACPP_MTMD_LIB)
        target_compile_definitions(llamacpp-inference-worker PRIVATE
            QAI_FORGE_LLAMACPP_MTMD
        )
        target_link_libraries(llamacpp-inference-worker PRIVATE ${LLAMACPP_MTMD_LIB})
    endif()

    target_link_libraries(llamacpp-inference-worker PRIVATE
        nlohmann_json::nlohmann_json
        Threads::Threads
    )

    if(QAI_FORGE_INSTALL)
        install(TARGETS llamacpp-inference-worker
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endif() # QAI_FORGE_BUILD_LLAMACPP

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
    if(QAI_FORGE_BUILD_GENIE)
        install(TARGETS llmengine vlmengine
            EXPORT qai_forgeTargets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    # ── Install worker binaries ────────────────────────────────────────────────
    if(QAI_FORGE_BUILD_WORKERS AND QAI_FORGE_BUILD_GENIE)
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
