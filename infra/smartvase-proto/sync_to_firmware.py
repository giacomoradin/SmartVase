"""
PlatformIO pre-build hook — syncs the SHARED files from the canonical source
(infra/) to the Hub and the Mega, removing the risk of manually misaligned
copies (which already caused bugs in the past: serial CRC and TLS cert).

Syncs:
  - smartvase.proto / smartvase.pb.{c,h}  (infra/smartvase-proto/) -> Hub include/ + Mega src/
  - hivemq_ca_cert.h                       (infra/)                 -> Hub include/

File copies only: no network, no codegen (that stays manual via
generate_proto.bat after editing the .proto). The CAM is NOT touched.
Runs on every build; if the copies are already aligned, nothing changes.

Note: PlatformIO runs extra_scripts without defining __file__, so the repo
root is derived from env["PROJECT_DIR"] (Hub and Mega are both 3 levels
below the root).
"""
Import("env")  # noqa: F821  (injected by PlatformIO/SCons)
import os
import shutil

PROJECT_DIR = env["PROJECT_DIR"]
ROOT  = os.path.abspath(os.path.join(PROJECT_DIR, "..", "..", ".."))
PROTO = os.path.join(ROOT, "infra", "smartvase-proto")

HUB_INC = os.path.join(
    ROOT, "firmware", "1_esp32-hub",
    "Radin_Giacomo_SmartVase_LogicWebHub_ESP32Standard", "include")
MEGA_SRC = os.path.join(
    ROOT, "firmware", "2_platform-controller_mega",
    "Radin_Giacomo_SmartVase_PlatformController_ArduinoMega", "src")


def _copy(src, dst_dir):
    if os.path.isfile(src) and os.path.isdir(dst_dir):
        shutil.copy2(src, os.path.join(dst_dir, os.path.basename(src)))


# Protobuf: .proto (reference) + .pb.{c,h} (generated) to Hub and Mega.
for fname in ("smartvase.proto", "smartvase.pb.c", "smartvase.pb.h"):
    src = os.path.join(PROTO, fname)
    _copy(src, HUB_INC)
    _copy(src, MEGA_SRC)

# HiveMQ CA cert: only the Hub uses it (the Mega has no network).
_copy(os.path.join(ROOT, "infra", "hivemq_ca_cert.h"), HUB_INC)

print("[sync_shared] proto + CA cert synced from the canonical source infra/")
