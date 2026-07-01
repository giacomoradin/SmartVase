/*!
 * @file Movement.h
 * @ingroup MegaMovement
 * @brief Interface of the autonomous movement FSM (light/shadow seeking, obstacle avoidance, stuck recovery).
 * @date 2026-04-29
 * @author Giacomo Radin
 */

/**
 * @defgroup MegaMovement Movement and navigation (Mega)
 * @brief Motor state machine, light/shadow seeking, obstacle avoidance, anti-circling and "stuck" state handling.
 * @{
 */

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include <Arduino.h>
#include "smartvase_aliases.h"

class Sensors;

/**
 * @struct ObstacleView
 * @brief Snapshot of the distances measured by the ultrasonic sensors (in cm).
 *
 * Values are set to NAN if the corresponding sensor reading is invalid or faulty.
 */
struct ObstacleView {
    float top;          /**< US1: Front top (upper anti-collision) */
    float front_right;  /**< US2: Front right */
    float front_left;   /**< US3: Front left */
    float left;         /**< US5: Side left */
    float right;        /**< US6: Side right */
};

/**
 * @class Movement
 * @brief FSM (Finite State Machine) for autonomous movement and seeking optimal conditions.
 *
 * The class drives the motors to follow light or shadow (seeking) and to avoid
 * obstacles detected by the ultrasonic sensors (obstacle avoidance). It also manages the
 * "stuck" state with exponential backoff.
 *
 * @note Motor driver: **Pololu Dual VNH5019 Shield**. Per-motor interface: `INA`/`INB`
 *       (direction), `PWM` (speed), `EN/DIAG` (enable + fault reporting, read via
 *       faultLeft()/faultRight()). Pin mapping in Movement.cpp and in `docs/PINS - Sheet1.csv`.
 */
class Movement {
public:
    /**
     * @brief Constructor of the Movement class.
     */
    Movement();

    /**
     * @brief Initializes the motor driver pins.
     */
    void init();

    /**
     * @brief Handles the movement state and logic on every cycle.
     *
     * To be called at high frequency in the main loop. Drives the movement state machine
     * (M_IDLE, M_MOVING, M_AVOID_*, M_STUCK) based on the sensor readings, the ambient light
     * and the desired target mode.
     *
     * @param v Structure holding the current obstacle distances.
     * @param cached_lux Current filtered light level (lux).
     * @param config Current configuration stored in EEPROM.
     * @param stats Cumulative usage statistics (used to count seeking sessions and avoid circling).
     * @param degradedModeActive Flag indicating whether the system is in degraded mode (insufficient RAM, etc.).
     */
    void handleMovementSM(const ObstacleView& v, int cached_lux,
                          const DeviceConfig& config, CumulativeStats& stats,
                          bool degradedModeActive);

    /**
     * @brief Immediately stops the robot's motors.
     *
     * @param stats Reference to the statistics, to update the activity state.
     */
    void stopMotors(CumulativeStats& stats);

    /**
     * @brief Sets the robot's target mode.
     *
     * @param mode New operating mode (LIGHT, SHADOW, IDLE).
     */
    void setTargetMode(CppMode mode);

    /**
     * @brief Returns the currently set target mode.
     * @return CppMode Current target mode.
     */
    CppMode getTargetMode() const { return targetMode; }

    /**
     * @brief Returns the state machine's current movement state.
     * @return CppMovementState Current movement state.
     */
    CppMovementState getCurrentState() const { return currentMovementState; }

    /**
     * @brief Indicates whether the left motor's VNH5019 driver is reporting a fault.
     * @details Reads the EN/DIAG pin (VNH5019 open-drain line): at rest it is held high by
     *          the shield's pull-up (driver enabled), and the chip pulls it low on a fault
     *          (overtemperature, overcurrent, undervoltage). EN/DIAG is **not wired** to the
     *          Mega as of 2026-06-30 (`MOTOR_EN_DIAG_WIRED 0` in Movement.cpp): this always
     *          returns false until it is physically wired and the flag/pins are updated.
     * @return true if a fault is currently active on the left motor.
     */
    bool faultLeft() const;

    /**
     * @brief Indicates whether the right motor's VNH5019 driver is reporting a fault. See faultLeft().
     * @return true if a fault is currently active on the right motor.
     */
    bool faultRight() const;

    /**
     * @brief Performs a test movement (forward, backward, left, right) for a given duration.
     *
     * Mainly used for debugging from the serial CLI interface (`motor` command). Blocking
     * (with periodic `wdt_reset()`), meant for bench testing with the wheels lifted off the ground.
     *
     * @param dir Character indicating the direction ('F'=Forward, 'B'=Backward, 'L'=Left, 'R'=Right).
     * @param ms Duration of the test movement in milliseconds, clamped to 60000 ms (60 s).
     * @param config Configuration holding the motor speed/power parameters.
     */
    void testMove(char dir, uint16_t ms, const DeviceConfig& config);

private:
    /**
     * @brief Activates the motors to move the robot forward.
     */
    void moveForward(const DeviceConfig& config);

    /**
     * @brief Activates the motors to move the robot backward.
     */
    void moveBackward(const DeviceConfig& config);

    /**
     * @brief Rotates the robot to the right in place.
     */
    void turnRight(const DeviceConfig& config);

    /**
     * @brief Rotates the robot to the left in place.
     */
    void turnLeft(const DeviceConfig& config);

    /**
     * @brief Checks whether the front trajectory is blocked by obstacles.
     *
     * Combines the data from sensors US1 (front top), US2 (front right) and US3 (front left).
     *
     * @param v Current obstacle view.
     * @return true if there is a nearby obstacle in front, false otherwise.
     */
    bool frontBlocked(const ObstacleView& v) const;

    CppMovementState currentMovementState;    /**< Current state of the motor FSM */
    CppMode          targetMode;              /**< Target mode set by the user or the cloud */
    unsigned long    motorActiveStartTime;    /**< Timestamp when motor activity started */
    unsigned long    stateStartTime;           /**< Timestamp of entry into the current FSM state */
    uint8_t          avoidance_attempts;      /**< Number of consecutive failed obstacle-avoidance attempts */
    unsigned long    stuck_cooldown_start_time;/**< Start of the wait cooldown in the M_STUCK state */
    uint32_t         current_stuck_backoff;   /**< Current backoff duration for the stuck state (in ms) */
    unsigned long    seekTurnStartMs;         /**< Timestamp of the start of seeking rotation (to avoid circling) */
    unsigned long    seekRelocateUntilMs;     /**< Time limit within which to complete the relocation */
};

#endif // MOVEMENT_H

/** @} */ // end of MegaMovement
