#ifndef MOVEMENT_H
#define MOVEMENT_H

#include <Arduino.h>
#include "smartvase_aliases.h"
#include "Persistence.h"

class Movement {
public:
    Movement();
    void init();
    void handleMovementSM(bool front_obs, bool left_obs, bool right_obs, int cached_lux, const DeviceConfig& config, CumulativeStats& stats, bool degradedModeActive);
    void stopMotors();
    void setTargetMode(CppMode mode);
    CppMovementState getCurrentState();

private:
    void moveForward(const DeviceConfig& config);
    void moveBackward(const DeviceConfig& config);
    void turnRight(const DeviceConfig& config);
    void turnLeft(const DeviceConfig& config);

    CppMovementState currentMovementState;
    CppMode targetMode;
    unsigned long motorActiveStartTime;
    unsigned long stateStartTime;
    uint8_t avoidance_attempts;
    unsigned long stuck_cooldown_start_time;
    uint32_t current_stuck_backoff;
    float pre_maneuver_dist;
    int lightThreshold;
};

#endif // MOVEMENT_H