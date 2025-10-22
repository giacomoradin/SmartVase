<div align="center">
  <img src="https://placehold.co/1200x400/1a2a1a/c2f0c2?text=SmartVase%0A%0AEnterprise-Grade%20IoT%20Greenhouse&font=raleway" alt="SmartVase Project Banner">
</div>

<div align="center">
  <h3>A highly reliable, resilient, and observable IoT system for a mobile, fully automated greenhouse, controlled via a native Android application.</h3>
</div>

---

This document presents a comprehensive technical specification for the **SmartVase** project.  
It is designed to serve as the definitive reference for the engineering team, detailing the system's architecture, foundational design principles, communication protocols, and development workflow.

---

## 🗺️ Table of Contents
1. [Project Vision & Foundational Principles](#-1-project-vision--foundational-principles)  
2. [System Architecture Analysis](#-2-system-architecture-analysis)  
3. [Communication Protocol: Protobuf](#-3-communication-protocol-protobuf-over-mqtt--serial)  
4. [Key Firmware Concepts](#-4-elucidation-of-key-firmware-concepts)  
5. [Team Roles & Responsibilities](#-5-team-roles--responsibilities)  
6. [Developer Onboarding & Workflow](#-6-developer-onboarding--workflow)  
7. [License](#-7-license)  

---

## 🎯 1. Project Vision & Foundational Principles

The objective of the SmartVase project is to engineer an enterprise-ready IoT product that automates plant cultivation with a high degree of reliability.  
The system's architecture is predicated on four foundational principles:

- **Resilience & Robustness**: Operate autonomously for extended durations and recover gracefully from software stalls, hardware faults, and anomalous conditions.  
- **Observability & Diagnostics**: Structured logs, granular telemetry, and comprehensive health status reports.  
- **Performance & Efficiency**: Efficient data serialization and non-blocking execution models in resource-constrained environments.  
- **Modularity & Maintainability**: Rigorous separation of concerns, enabling parallel development and long-term maintainability.  

---

## 🏗️ 2. System Architecture Analysis

The system utilizes a distributed architecture that decouples responsibilities across specialized hardware components and a central communication backbone.

### Firmware Subsystems

| Component       | Codename | Core Function                                        |
|-----------------|----------|------------------------------------------------------|
| Arduino Mega    | The Brawn | Direct hardware control, sensor reading, physical I/O |
| ESP32 Standard  | The Brain | Logic coordination, data aggregation, comms hub      |
| ESP32-CAM       | The Eye   | Vision processing and image analysis                 |
| Android App     | —        | User-facing control center (Kotlin, Compose, MVVM)   |

### Cloud & Communication Backbone
- Central **MQTT Broker** as the reliable, scalable backbone for all communication between IoT devices and the Android application.

---

## ⚡ 3. Communication Protocol: Protobuf over MQTT & Serial

To satisfy efficiency and robustness requirements, **Protocol Buffers (Protobuf)** are used instead of JSON.

Advantages:
- Smaller payloads  
- Faster parsing  
- Minimal RAM overhead  
- Strongly-typed schema eliminating runtime errors  

All message structures are defined in a single canonical file: **`smartvase.proto`**.

---

## 🛠️ 4. Elucidation of Key Firmware Concepts

- **Hardware Watchdog (WDT)**: Ensures recovery from software faults.  
- **Low Memory Detection**: Activates degraded mode to prevent crashes.  
- **Hub Deadman Timer**: Safe state if hub communication is lost.  
- **Resilient Data Persistence**: Dual-slot, CRC-validated storage for config and stats.  

---

## 👨‍💻 5. Team Roles & Responsibilities

| Member   | Role                                | Key Responsibilities |
|----------|-------------------------------------|----------------------|
| Giacomo  | PM & Lead Firmware Eng | Supervision, system architecture, firmware (ESP32 Hub & CAM) |
| Antonio  | Computer Vision Specialist          | Image processing & analysis on ESP32-CAM |
| Fia      | Backend & Cloud Architect           | MQTT broker design, setup, maintenance |
| Francesco  | Android Application Developer       | Native Android app (MVVM, Compose, MQTT integration) |

---

## 🚀 6. Developer Onboarding & Workflow

### Development Prerequisites
- Protocol Buffer Compiler (`protoc`)  
- Nanopb (C implementation for embedded)  
- PlatformIO IDE  
- Android Studio  

### Core Workflow (Schema-First)
1. **✍️ Define in `.proto`**: Edit `smartvase.proto`.  
2. **⚙️ Generate Code**: Run `protoc` to generate code for all platforms.  
3. **💡 Implement Logic**: Use generated, type-safe structures in firmware/app.  

---

## 📜 7. License

This project is licensed under the **MIT License**.  
See the [LICENSE](LICENSE) file for full details.
