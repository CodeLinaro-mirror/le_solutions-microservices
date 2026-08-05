// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/utils/PrivilegeDrop.h"
#include "qai_forge/utils/Logger.h"

#include <sys/stat.h>
#include <unistd.h>
#include <grp.h>
#include <glob.h>
#include <filesystem>
#include <vector>
#include <cstdlib>

namespace qai_forge {
namespace utils {

void drop_privileges_and_bind_devices() {
    if (getuid() != 0) {
        LOG_INFO("[startup] Not running as root, skipping privilege drop.");
        return;
    }

    struct stat st;
    std::vector<gid_t> supp_gids;

    // ── 1. Discover host GIDs and chmod DSP/DMA devices ──────────────────────
    const char* dev_patterns[] = { "/dev/fastrpc*", "/dev/dma_heap/*" };
    for (const char* pattern : dev_patterns) {
        glob_t g;
        if (glob(pattern, GLOB_NOSORT, nullptr, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; ++i) {
                const char* dev = g.gl_pathv[i];
                if (stat(dev, &st) == 0) {
                    LOG_DEBUG("[startup] Found " << dev
                              << " owned by host GID " << st.st_gid);
                    supp_gids.push_back(st.st_gid);
                }
                if (chmod(dev, 0666) == 0) {
                    LOG_DEBUG("[startup] Set permissions 0666 on " << dev);
                } else {
                    LOG_WARN("[startup] chmod 0666 failed for " << dev);
                }
            }
        }
        globfree(&g);
    }

    // ── 2. Discover GID of /usr/share/qcom (bind-mounted from host) ──────────
    const char* qcom_share = "/usr/share/qcom";
    if (stat(qcom_share, &st) == 0) {
        LOG_DEBUG("[startup] Found " << qcom_share
                  << " owned by host GID " << st.st_gid);
        supp_gids.push_back(st.st_gid);
    }

    // ── 3. Deploy baked fastrpc DSP config into the bind-mounted share ────────
    // /etc/fastrpc/hexagon-dsp-binaries.yaml is baked into the image.
    // /usr/share/qcom is bind-mounted from the host at runtime.
    // We copy the config now (as root) so fastrpc can find it.
    const char* fastrpc_src = "/etc/fastrpc/hexagon-dsp-binaries.yaml";
    const char* fastrpc_dst_dir = "/usr/share/qcom/conf.d";
    std::error_code ec;
    if (std::filesystem::exists(fastrpc_src, ec) &&
        std::filesystem::exists(qcom_share, ec)) {
        std::filesystem::create_directories(fastrpc_dst_dir, ec);
        std::filesystem::copy_file(
            fastrpc_src,
            std::string(fastrpc_dst_dir) + "/hexagon-dsp-binaries.yaml",
            std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            LOG_INFO("[startup] Deployed " << fastrpc_src
                     << " -> " << fastrpc_dst_dir << "/");
        } else {
            LOG_WARN("[startup] Could not deploy fastrpc config: " << ec.message());
        }
    }

    // ── 4. Detect target UID/GID from mounted models volume ──────────────────
    uid_t target_uid = 10000;
    gid_t target_gid = 10000;
    const char* models_dir = "/mnt/work/models";
    if (stat(models_dir, &st) == 0) {
        target_uid = st.st_uid;
        target_gid = st.st_gid;
        LOG_INFO("[startup] Detected " << models_dir
                 << " owned by UID " << target_uid
                 << ", GID " << target_gid
                 << ". Adopting these IDs.");
    } else {
        LOG_INFO("[startup] Directory " << models_dir
                 << " not found. Defaulting to UID " << target_uid
                 << ", GID " << target_gid << ".");
    }
    supp_gids.push_back(target_gid);

    // ── 5. Apply supplementary groups and drop privileges ────────────────────
    if (setgroups(supp_gids.size(), supp_gids.data()) != 0) {
        LOG_ERROR("[startup] FATAL: setgroups() failed.");
        std::exit(1);
    }
    if (setresgid(target_gid, target_gid, target_gid) != 0) {
        LOG_ERROR("[startup] FATAL: setresgid() failed.");
        std::exit(1);
    }
    if (setresuid(target_uid, target_uid, target_uid) != 0) {
        LOG_ERROR("[startup] FATAL: setresuid() failed.");
        std::exit(1);
    }

    LOG_INFO("[startup] Privileges dropped to UID " << target_uid
             << " / GID " << target_gid);
}

} // namespace utils
} // namespace qai_forge
