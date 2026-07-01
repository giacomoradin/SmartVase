"""
PlatformIO pre-build hook — sincronizza i file CONDIVISI dalla fonte canonica
(infra/) verso Hub e Mega, eliminando il rischio di copie disallineate a mano
(che in passato ha gia' causato bug: CRC seriale e cert TLS).

Sincronizza:
  - smartvase.proto / smartvase.pb.{c,h}  (infra/smartvase-proto/) -> Hub include/ + Mega src/
  - hivemq_ca_cert.h                       (infra/)                 -> Hub include/

Solo copie di file: nessuna rete, nessuna codegen (quella resta manuale via
generate_proto.bat dopo aver editato il .proto). La CAM (di Anto) NON e' toccata.
Gira a ogni build; se le copie sono gia' allineate non cambia nulla.

Nota: PlatformIO esegue gli extra_scripts senza definire __file__, quindi la
root del repo si ricava da env["PROJECT_DIR"] (Hub e Mega sono entrambi 3
livelli sotto la root).
"""
Import("env")  # noqa: F821  (iniettato da PlatformIO/SCons)
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


# Protobuf: .proto (riferimento) + .pb.{c,h} (compilati) verso Hub e Mega.
for fname in ("smartvase.proto", "smartvase.pb.c", "smartvase.pb.h"):
    src = os.path.join(PROTO, fname)
    _copy(src, HUB_INC)
    _copy(src, MEGA_SRC)

# CA cert HiveMQ: solo l'Hub lo usa (il Mega non ha rete).
_copy(os.path.join(ROOT, "infra", "hivemq_ca_cert.h"), HUB_INC)

print("[sync_shared] proto + CA cert sincronizzati dalla fonte canonica infra/")
