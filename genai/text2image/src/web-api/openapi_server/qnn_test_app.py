# Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
# SPDX-License-Identifier: BSD-3-Clause-Clear

# coding: utf-8

import sys

from qnn_lib_provider import QnnLibrary, QnnProvider, SystemProvider

# =========================================================================================
# 1) App orchestrator (simple main)
# =========================================================================================
class QnnApp:
    def __init__(self):
        self.libs = QnnLibrary()

    def run(self) -> int:
        # Libraries
        try:
            self.libs.load()
        except RuntimeError as e:
            print("[FATAL]", e)
            return 3

        # --- Core providers ---
        try:
            pp, n = self.libs.get_qnn_providers()
        except RuntimeError as e:
            print("[FATAL]", e)
            return 4

        print(f"[INFO] QNN n_providers={n}")
        p0 = QnnProvider(pp[0])
        info = p0.info()
        print(f"[QNN 0] provider=0x{info['addr']:016x} "
              f"name_ptr=0x{info['name_ptr']:016x} "
              f"coreApi={info['coreApi']} backendApi={info['backendApi']} "
              f"backendId={info['backendId']} providerName={info['providerName']}")

        # Now using logCreate before backendCreate
        rc_core = p0.backend_create_free(log_level=3)  # DEBUG by default
        if rc_core == 0:
            print("[INFO] Core backendCreate with logger succeeded.")
        else:
            print("[WARN] Core backendCreate returned non-zero; see error lines above.")

        # --- System providers ---
        if self.libs.system is not None:
            try:
                spp, sn = self.libs.get_system_providers()
            except RuntimeError as e:
                print("[WARN]", e)
                sn = 0
                spp = None

            if sn > 0 and spp is not None:
                print(f"[INFO] QNN System n_providers={sn}")
                s0 = SystemProvider(spp[0])
                sinfo = s0.info()
                print(f"[SYS 0] provider=0x{sinfo['addr']:016x} "
                      f"name_ptr=0x{sinfo['name_ptr']:016x} "
                      f"systemApi={sinfo['systemApi']} backendId={sinfo['backendId']} "
                      f"providerName={sinfo['providerName']}")
                s0.smoke_test(log_level=5)
            else:
                print("[WARN] No System providers found or system lib not loaded.")

        return rc_core


# =========================================================================================
# 2) Super-simple main
# =========================================================================================
if __name__ == "__main__":
    sys.exit(QnnApp().run())
