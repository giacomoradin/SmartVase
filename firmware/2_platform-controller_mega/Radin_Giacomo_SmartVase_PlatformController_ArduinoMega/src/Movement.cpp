#include "Movement.h"
#include "smartvase_aliases.h" // Required for CppMovementState and CppMode enums

// --- Pinout ---
const int enA = 7, in1 = 43, in2 = 45;
const int enB = 6, in3 = 47, in4 = 49;

Movement::Movement() {
    this->currentMovementState = CPP_M_IDLE;
    this->targetMode = CPP_IDLE;
    this->motorActiveStartTime = 0;
    this->stateStartTime = 0;
    this->avoidance_attempts = 0;
    this->stuck_cooldown_start_time = 0;
    this->current_stuck_backoff = 30000;
    this->pre_maneuver_dist = 0;
    this->lightThreshold = 600;
}

void Movement::init() {
    pinMode(enA, OUTPUT);
    pinMode(in1, OUTPUT);
    pinMode(in2, OUTPUT);
    pinMode(enB, OUTPUT);
    pinMode(in3, OUTPUT);
    pinMode(in4, OUTPUT);
    stopMotors();
}

void Movement::stopMotors() {
    analogWrite(enA, 0);
    analogWrite(enB, 0);
    digitalWrite(in1, LOW);
    digitalWrite(in2, LOW);
    digitalWrite(in3, LOW);
    digitalWrite(in4, LOW);
    if (this->motorActiveStartTime > 0) {
        // stats.total_motor_active_time_s += (millis() - this->motorActiveStartTime) / 1000;
        this->motorActiveStartTime = 0;
    }
}

void Movement::moveForward(const DeviceConfig& config) { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }
void Movement::moveBackward(const DeviceConfig& config) { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void Movement::turnRight(const DeviceConfig& config) { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, HIGH); digitalWrite(in2, LOW); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, LOW); digitalWrite(in4, HIGH); }
void Movement::turnLeft(const DeviceConfig& config) { analogWrite(enA, config.motorCalibLeft); digitalWrite(in1, LOW); digitalWrite(in2, HIGH); analogWrite(enB, config.motorCalibRight); digitalWrite(in3, HIGH); digitalWrite(in4, LOW); }

void Movement::handleMovementSM(bool front_obs, bool left_obs, bool right_obs, int cached_lux, const DeviceConfig& config, CumulativeStats& stats, bool degradedModeActive) {
    if (degradedModeActive) {
        if (this->currentMovementState != CPP_M_IDLE) { stopMotors(); this->currentMovementState = CPP_M_IDLE; }
        return;
    }
    if (this->motorActiveStartTime > 0 && millis() - this->motorActiveStartTime > 20000) {
        // LOG_ERROR("motor_timeout", "Safety stop");
        stopMotors();
        this->currentMovementState = CPP_M_IDLE;
        return;
    }

    switch (this->currentMovementState) {
        case CPP_M_IDLE:
            stopMotors();
            if (this->targetMode != CPP_IDLE) {
                this->currentMovementState = M_MOVING;
                this->motorActiveStartTime = millis();
                this->avoidance_attempts = 0;
                this->current_stuck_backoff = 30000;
            }
            break;
        case M_MOVING:
            if (front_obs) {
                stats.obstacles_avoided++;
                this->currentMovementState = M_AVOID_START;
            } else {
                if (this->targetMode == CPP_LIGHT && cached_lux < this->lightThreshold) turnRight(config);
                else if (this->targetMode == CPP_SHADOW && cached_lux > this->lightThreshold) turnLeft(config);
                else moveForward(config);
            }
            break;
        case M_AVOID_START:
            stopMotors();
            // pre_maneuver_dist = cached_front_dist_cm;
            this->stateStartTime = millis();
            this->currentMovementState = M_AVOID_REVERSING;
            stats.escape_attempts++;
            break;
        case M_AVOID_REVERSING:
            moveBackward(config);
            if (millis() - this->stateStartTime > config.avoid_reverse_ms) {
                this->stateStartTime = millis();
                this->currentMovementState = M_AVOID_TURNING;
            }
            break;
        case M_AVOID_TURNING:
            if (millis() - this->stateStartTime > config.avoid_turn_ms) {
                // if (!isnan(pre_maneuver_dist) && !isnan(cached_front_dist_cm) && abs(cached_front_dist_cm - pre_maneuver_dist) < 2.0) {
                //     stats.no_progress_events++;
                // }
                if (front_obs) {
                    this->avoidance_attempts++;
                    if (this->avoidance_attempts >= 3) {
                        stats.stuck_events++;
                        // LOG_CRITICAL("stuck_detected", "Entering STUCK state");
                        this->currentMovementState = M_STUCK;
                        this->stuck_cooldown_start_time = millis();
                    } else { this->currentMovementState = M_AVOID_START; }
                } else { this->currentMovementState = M_MOVING; }
                break;
            }
            if (!right_obs && right_obs != left_obs) turnRight(config);
            else if (!left_obs) turnLeft(config);
            else { if (random(0, 2) == 0) turnLeft(config); else turnRight(config); }
            break;
        case M_STUCK:
            stopMotors();
            if (millis() - this->stuck_cooldown_start_time > this->current_stuck_backoff) {
                // LOG_INFO("stuck_cooldown_end", "Attempting recovery");
                this->currentMovementState = M_IDLE;
                this->current_stuck_backoff += 10000;
            }
            break;
    }
}

void Movement::setTargetMode(CppMode mode) {
    this->targetMode = mode;
}

CppMovementState Movement::getCurrentState() {
    return this->currentMovementState;
}
