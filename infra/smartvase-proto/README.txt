SmartVase - canonical Protocol Buffers schema (nanopb)
=======================================================

This folder is the single source of truth for the Hub <-> Mega serial
wire format:

  smartvase.proto        the CANONICAL schema (edit ONLY this file)
  smartvase.pb.c / .pb.h nanopb-generated sources (do not edit by hand,
                         except the mandatory include patch below)
  generate_proto.bat     regenerates the .pb.{c,h} using the local venv
  sync_to_firmware.py    PlatformIO pre-build hook: copies the .proto,
                         the generated sources and the HiveMQ CA cert
                         into the Hub and Mega projects on every build
  nanopb-nanopb-0.4.9.1/ vendored nanopb runtime (third-party code)

Workflow to change the protocol:
  1. Edit smartvase.proto (append new fields with NEW tags only).
  2. Run generate_proto.bat.
  3. Patch the generated smartvase.pb.h:
       #include <pb.h>   ->   #include "pb.h"
  4. Build normally: the pre-build hook syncs the copies into
     firmware/1_esp32-hub/.../include/ and
     firmware/2_platform-controller_mega/.../src/ automatically.
  5. Update smartvase_aliases.h in the firmwares if message or field
     names changed.
