# Firestore - Flutter Communication Convention

## Flutter --> Firestore

### 1. Initial Config
Plant information for SmartVase setup.

**Firestore Path:**
```
smartvase/HUB_123456/command/config
```

**JSON Payload Example:**
```
{
    "soil_dry_threshold": 450,
    "light_threshold": 400
}
```

### 2. Commands
There are many possible commands that the Flutter App can send, all with the following **Firestore Path** structure:
```
smartvase/HUB_123456/command/{action}
```

- Water (Start Irrigation):
    - Path:  smartvase/HUB_123456/command/water 
    - JSON:  {"cmd_id": 101, "type": "water", "duration_ms": 5000} 
- Set Mode:
    - Path:  smartvase/HUB_123456/command/setMode 
    - JSON:  {"cmd_id": 102, "type": "setMode", "mode": "LIGHT"}  (mode può essere "LIGHT" , "SHADOW" o "IDLE" )
- Emergency Stop:
    - Path:  smartvase/HUB_123456/command/stop 
    - JSON:  {"cmd_id": 103, "type": "stop"} 
- Request Diagnostics:
    - Path:  smartvase/HUB_123456/command/requestDiagnostics 
    - JSON:  {"cmd_id": 104, "type": "requestDiagnostics"} 
- Cambia parametri di sterzata/inversione:
    - Path:  smartvase/HUB_123456/command/setMotionParams 
    - JSON:  {"cmd_id": 105, "type": "setMotionParams", "reverse_ms": 1000, "turn_ms": 1200} 
- Read Soil Humidity:
    - Path:  smartvase/HUB_123456/command/readSoil 
    - JSON:  {"cmd_id": 106, "type": "readSoil"} 
- Soft Reset Arduino Mega :
    - Path:  smartvase/HUB_123456/command/softReset 
    - JSON:  {"cmd_id": 107, "type": "softReset"}

## Firestore --> Flutter

### 3. Telemetry
Telemetry from the SmartVase.

**Firestore Path:**
smartvase/HUB_123456/telemetry/telemetry

**JSON Payload Example:**
```
{
    "timestamp_utc":31288,
    "uptime_s":2475,
    "device_id":"HUB_123456",
    "fw_version":"1.3.0",
    "movement_state":"IDLE",
    "lux":529,
    "soil_moisture":0,
    "water_level_cm":24.50690269,
    "distances_cm": {
        "top":10.88708973,
        "front_right":125.5802612,
        "front_left":102.0237427,
        "left":0,
        "right":66.40615845
    },
    "free_ram_bytes":2727,
    "counters": {
        "watchdog_resets":808,
        "total_irrigations":0,
        "total_irrigation_duration_s":0,
        "total_motor_active_time_s":3786,
        "obstacles_avoided":11,
        "stuck_events":11,
        "bme_read_errors":0,
        "log_overflows":0,
        "pb_decode_failures":1,
        "light_seeking_sessions":139,
        "shadow_seeking_sessions":0,
        "escape_attempts":33
    }
}
```

### 4. Image Ready Notification
This gets updated when a new image has been uploaded to Firebase Cloud Storage AND the leaf health analysis has been completed (by the lightweight vision algorithm on the edge).

**Firestore Path:**
smartvase/CAM_123456/vision/image_ready

**JSON Payload Example:**
```
{
    "image_url": "gs://smartvase-7cfd9.appspot.com/images/VASE_01_1678886405.jpg",
    "timestamp_utc": 31288,
    "resolution": "640x480",
    "size_bytes": 397847,
    "crc32": 1239082,
    "capture_time_ms": 254,
    "plant_healthy": true,
    "message": "Plant is ok, no need to worry!"
}
```


