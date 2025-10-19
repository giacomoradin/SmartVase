🌱🤖 SmartVase: IoT Automated Greenhouse System 🤖🪴

A highly reliable, resilient, and observable IoT system for a mobile, fully automated greenhouse, controlled via a native Android application.

This document provides a comprehensive technical overview of the SmartVase project, covering its architectural design, core principles, communication protocols, and development workflow. It is intended to be the single source of truth for the development team.

Table of Contents

Project Vision & Core Principles

System Architecture Deep Dive

2.1 Firmware Subsystems

2.2 Android Application

2.3 Cloud & Communication Backbone

Communication Protocol: Protobuf over MQTT & Serial

3.1 Why Protocol Buffers?

3.2 The smartvase.proto Contract

Key Firmware Concepts Explained

4.1 Failsafe Mechanisms

4.2 Resilient Data Persistence

4.3 Non-Blocking Cooperative Scheduler

Team Roles & Responsibilities

Getting Started: Developer Workflow

6.1 Prerequisites

6.2 The Core Development Workflow

License

1. Project Vision & Core Principles

The mission of SmartVase is to engineer an enterprise-ready IoT product that automates plant care with high reliability. The system is architected around four fundamental principles:

Resilience & Robustness: The system must operate autonomously for extended periods and be capable of recovering gracefully from software hangs, hardware failures, and unexpected environmental states.

Observability & Diagnostics: "If you can't measure it, you can't improve it." Every component is designed to produce structured logs, detailed telemetry, and health status reports, enabling deep performance analysis and rapid debugging.

Performance & Efficiency: In a resource-constrained embedded environment, every CPU cycle, byte of RAM, and bit transmitted matters. The architecture prioritizes efficient serialization, non-blocking code, and minimal resource consumption.

Modularity & Maintainability: The system employs a clean separation of concerns, enabling parallel development, independent testing of components, and easier long-term maintenance.

2. System Architecture Deep Dive

The system utilizes a distributed architecture, decoupling responsibilities across specialized hardware components and a central communication backbone.

2.1 Firmware Subsystems

The firmware is a triumvirate of microcontrollers, each with a distinct, non-overlapping role.

🤖 Arduino Mega - Platform Controller ("The Brawn")

The lowest-level component, responsible for direct hardware interfacing and physical world interaction.

Hardware Interface:

Motors (Left & Right) via H-Bridge

Water Pump & UVA Light (Relay Control)

Ultrasonic Sensors (Front, Left, Right, Water Tank)

Soil Moisture Sensor (Fork)

Ambient Light Sensor (Photoresistor)

BME680 Environmental Sensor (I2C)

Core Responsibilities:

Executes high-level commands (e.g., move, water).

Manages a real-time, non-blocking Finite State Machine (FSM) for autonomous movement and obstacle avoidance.

Continuously samples, filters (EMA), and caches sensor data.

Implements critical failsafe mechanisms (see Section 4).

Communication: Communicates exclusively with the ESP32 Hub over a Serial link using the Protocol Buffers (Protobuf) serialization format.

🧠 ESP32 Standard - Logic & Web Hub ("The Brain")

The system's central nervous system. It coordinates operations, aggregates data, and manages all external communication.

Hardware Interface:

Serial Port 1: Connection to ESP32-CAM.

Serial Port 2: Connection to Arduino Mega.

Core Responsibilities:

Manages Wi-Fi connectivity.

Acts as the primary MQTT Client, connecting to the central broker to send/receive data from the Android app.

Functions as a message router: receives Protobuf messages from the co-processors via Serial, relays them to MQTT, and vice-versa.

Aggregates logs and telemetry from all subsystems into a unified stream.

Hosts a local Web Server for low-level diagnostics (e.g., /health endpoint) accessible on the local network.

Communication: Bilingual; speaks Protobuf over Serial to its co-processors and Protobuf over MQTT to the outside world.

👁️ ESP32-CAM - Vision Co-Processor ("The Eye")

A specialized co-processor for all vision-related tasks.

Hardware Interface:

Camera Module.

Core Responsibilities:

Captures high-resolution images of the plant on command.

Performs basic, on-device image analysis (e.g., calculating average leaf color in HSV space for health-state screening).

Transmits image data or analysis results back to the Hub.

Communication: Communicates exclusively with the ESP32 Hub over a Serial link. It uses Protobuf for commands and metadata, followed by a raw binary stream for JPEG image data.

2.2 Android Application

The user-facing control center, built on a modern, robust, and scalable native Android stack.

Technology Stack:

Language: Kotlin

UI: Jetpack Compose (declarative, reactive UI)

Architecture: MVVM (Model-View-ViewModel)

Dependency Injection: Hilt

Communication: Paho MQTT Client

Local Persistence: Room (for telemetry history) & Jetpack DataStore (for secure settings).

Core Responsibilities:

Provides a real-time dashboard displaying telemetry data.

Allows users to send commands (e.g., manual watering, mode changes).

Displays a unified, filterable log stream from all devices.

Securely configures connection settings for the MQTT broker.

2.3 Cloud & Communication Backbone

A central MQTT Broker serves as the simple, reliable, and scalable backbone for all communication between the IoT device (via the Hub) and the Android application.

Responsibilities:

Authenticates clients.

Relays messages between publishers and subscribers based on topic filters.

Topic Structure: The system uses a clean, hierarchical topic structure. Example:

smartvase/01/telemetry/fast

smartvase/01/logs

smartvase/01/cmd/water

3. Communication Protocol: Protobuf over MQTT & Serial

To meet the project's goals of efficiency and robustness, JSON is explicitly not used for data exchange. The entire system relies on Protocol Buffers.

3.1 Why Protocol Buffers?

Protocol Buffers (Protobuf) is a binary serialization format developed by Google. It was chosen over JSON for several critical reasons in an embedded context:

Feature

Protocol Buffers (with nanopb)

JSON (with ArduinoJson)

Advantage for SmartVase

Payload Size

Highly compact binary format.

Verbose text format with repeated keys.

Smaller Footprint. Up to 60% smaller payloads reduce network traffic and transmission time.

Parsing Speed

Extremely fast binary parsing.

Slower, CPU-intensive string parsing.

Higher Performance. Frees up critical CPU cycles on the microcontrollers.

Memory Usage

Minimal RAM overhead; can encode/decode on-the-fly from streams.

Requires significant RAM for a DOM-like JsonDocument.

Essential for Arduino Mega. Allows the system to operate reliably without exhausting the limited SRAM.

Schema & Safety

Strict, strongly-typed schema defined in a .proto file.

Schemaless; prone to runtime errors from typos or type mismatches.

Extreme Reliability. The schema acts as a contract, eliminating an entire class of common IoT bugs at compile time.

3.2 The smartvase.proto Contract

All message structures for the entire system are defined in a single file: smartvase.proto. This file is the Single Source of Truth for all data communication.

Workflow: Any change to data structures must begin by editing this file.

Code Generation: The protoc compiler and associated plugins (nanopb for C, protobuf-java for Android) use this file to automatically generate the necessary serialization/deserialization code for all platforms. This ensures all components are always in sync.

4. Key Firmware Concepts Explained

The Platform Controller's firmware contains several critical mechanisms to ensure resilience.

4.1 Failsafe Mechanisms on the Platform Controller

Hardware Watchdog (WDT): The WDT is always active. If the main loop freezes for any reason, the WDT will automatically reset the microcontroller, ensuring recovery from software stalls. The number of WDT resets is a persisted cumulative metric.

Low Memory Detection: The firmware constantly monitors free SRAM. If it drops below a critical threshold, a "degraded mode" is activated, disabling non-essential functions (like autonomous movement) to prevent a crash.

Hub Deadman Timer: The Mega expects a periodic message (or command) from the Hub. If no communication is received within a defined timeout (e.g., 2 minutes), it assumes the Hub is offline, enters a safe state (motors off), and logs a critical event.

Sensor Reliability Tracking: The system tracks consecutive read failures for each critical sensor. If a sensor fails more than a set number of times, it is marked as unreliable, and its data is ignored to prevent the system from making decisions based on faulty readings.

4.2 Resilient Data Persistence

Configuration and cumulative statistics (e.g., total_irrigations) must survive reboots. To prevent data corruption from sudden power loss during a write and to manage flash/EEPROM wear:

Dual-Slot Storage: Two memory slots are reserved for any piece of persistent data.

CRC Validation: Each stored structure includes a CRC16 checksum of its data.

Write/Read Logic: On saving, data is written to the other slot. On loading, the system reads both slots, validates their CRC, and loads the one that is valid. This makes data persistence an atomic and recoverable operation.

4.3 Non-Blocking Cooperative Scheduler

The main loop() on the Arduino Mega is a simple, fast-running cooperative scheduler.

millis()-based Timing: No delay() calls are used. All tasks (e.g., sendFastTelemetry, flushLogQueue) are scheduled based on the elapsed time since their last run.

Task Miss Tracking: The scheduler monitors if a task is executed significantly later than its intended interval. This "task miss" count is a key performance indicator that reveals if the main loop is becoming overloaded.

5. Team Roles & Responsibilities

Member

Role

Key Responsibilities

Giacomo

Project Manager & Lead Firmware Engineer

Overall project supervision, system architecture, and firmware development for the ESP32 Hub and ESP32-CAM.

Antonio

Computer Vision Specialist

Development of all image processing and analysis logic on the ESP32-CAM.

Fia

Backend & Cloud Architect

Design, setup, and maintenance of the MQTT broker infrastructure. Defines and documents topic structures.

Gabriel

Android Application Developer

End-to-end development of the native Android control application.

6. Getting Started: Developer Workflow

6.1 Prerequisites

Protocol Buffer Compiler (protoc): Installation Guide

Nanopb: The C implementation of Protobuf for embedded systems. GitHub Repository

PlatformIO IDE: The recommended environment for all firmware development.

Android Studio: The official IDE for Android application development.

6.2 The Core Development Workflow

This schema-first workflow is mandatory for any changes affecting communication.

Define the Change in .proto: The process begins by editing the smartvase.proto file. Add or modify the required message or field definitions. This is the design phase.

Generate Code for All Platforms:

Firmware: Run the nanopb_generator.py script on the updated .proto file to produce the new smartvase.pb.c and smartvase.pb.h.

Android: Rebuild the Android project. The Gradle protobuf plugin will automatically detect the change and generate the corresponding Java/Kotlin classes.

Implement the Logic: With the new code generated, you can now use the new message structures in your firmware or Android code to implement the desired feature. This final step is now type-safe and protected by the schema.

7. License

This project is licensed under the MIT License. See the LICENSE file for details.
