# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# ─────────────────────────────────────────────────────────────────────────────
# postprocess-plugins — built-in Predictive AI postprocessing plugins
#
# Each model-specific postprocess builds as its own standalone shared
# library, named after its source file (e.g. src/modules/postprocess-qfd.cpp
# builds postprocess-qfd.so), against the dependency-free postproc_abi ABI
# header (QAIServe/include/postproc_abi/PostprocAbi.h) — no qai_forge
# dependency. These are loaded at runtime by PostprocPlugin, exactly like a
# client-supplied plugin dropped into the POSTPROC_PLUGINS_DIR mount; nothing
# here is linked directly into the qaiserve binary.
#
# Layout:
#   include/, src/           — postprocess-utils: shared decode/NMS/label
#                               helpers, header-only surface against
#                               postproc_abi types. No qai_forge dependency.
#   include/modules/,          one .h/.cpp pair per model-specific
#   src/modules/                postprocess. Each defines a class
#                               implementing postproc_abi::IPostprocess and
#                               invokes POSTPROC_ABI_EXPORT(ClassName) once at
#                               file scope to define the CreatePostprocess/
#                               DestroyPostprocess/PostprocAbiVersion trio.
#
# Adding a new built-in postprocess requires 2 new files under */modules/ +
# one entry in POSTPROC_PLUGIN_MODULES below — nothing else.
# ─────────────────────────────────────────────────────────────────────────────

add_library(postproc_utils STATIC
    src/postprocess-utils.cpp
)
target_include_directories(postproc_utils PUBLIC
    include
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_link_libraries(postproc_utils PUBLIC
    nlohmann_json::nlohmann_json
)
# Linked into SHARED plugin targets below — must be position-independent.
set_target_properties(postproc_utils PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Each entry here is a source file base name under src/modules/ — the built
# .so is named identically (e.g. postprocess-qfd -> postprocess-qfd.so).
set(POSTPROC_PLUGIN_MODULES
    postprocess-qfd
    postprocess-qpd
    postprocess-yolov8
    postprocess-ssd-mobilenet
    postprocess-hrnet
    postprocess-deeplab-argmax
)

set(POSTPROC_PLUGIN_TARGETS "")
foreach(module ${POSTPROC_PLUGIN_MODULES})
    add_library(${module} SHARED
        src/modules/${module}.cpp
    )
    target_link_libraries(${module} PRIVATE
        postproc_utils
        nlohmann_json::nlohmann_json
    )
    set_target_properties(${module} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${module}"
    )

    list(APPEND POSTPROC_PLUGIN_TARGETS ${module})
endforeach()

install(TARGETS ${POSTPROC_PLUGIN_TARGETS}
    DESTINATION lib/postproc_plugins
)
