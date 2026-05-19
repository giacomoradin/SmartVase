#ifndef MOVEMENT_H
#define MOVEMENT_H

#include <Arduino.h>
#include "smartvase_aliases.h"

class Sensors;

// Snapshot delle distanze passato alla state machine (cm; NAN se non valida)
struct ObstacleView {
    float top;          // US1
    float front_right;  // US2
    float front_left;   // US3
    float left;         // US5
    float right;        // US6
};

class Movement {
public:
    Movement();
    void init();

    // Da chiamare ad alta frequenza nel main loop.
    // Il logger e' un callback per emettere log strutturati senza dipendere
    // dalla classe Communication (rompe il ciclo include).
    void handleMovementSM(const ObstacleView& v, int cached_lux,
                          const DeviceConfig& config, CumulativeStats& stats,
                          bool degradedModeActive);

    void stopMotors(CumulativeStats& stats);
    void setTargetMode(CppMode mode);
    CppMode getTargetMode() const { return targetMode; }
    CppMovementState getCurrentState() const { return currentMovementState; }

    // Test diretti per CLI/debug
    void testMove(char dir, uint16_t ms, const DeviceConfig& config);

private:
    void moveForward(const DeviceConfig& config);
    void moveBackward(const DeviceConfig& config);
    void turnRight(const DeviceConfig& config);
    void turnLeft(const DeviceConfig& config);

    // Decide se il path frontale e' libero usando le 3 sonde frontali.
    bool frontBlocked(const ObstacleView& v) const;

    CppMovementState currentMovementState;
    CppMode          targetMode;
    unsigned long    motorActiveStartTime;
    unsigned long    stateStartTime;
    uint8_t          avoidance_attempts;
    unsigned long    stuck_cooldown_start_time;
    uint32_t         current_stuck_backoff;
};

#endif // MOVEMENT_H
