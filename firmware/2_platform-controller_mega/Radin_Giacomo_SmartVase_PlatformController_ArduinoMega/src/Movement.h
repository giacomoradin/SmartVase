/*!
 * @file Movement.h
 * @ingroup MegaMovement
 * @brief Interface of the autonomous movement FSM (light/shadow seeking, rotating light scan, obstacle avoidance, stuck recovery).
 * @date 2026-04-29
 * @author Giacomo Radin
 */

/**
 * @defgroup MegaMovement Movement and navigation (Mega)
 * @brief Motor state machine, light/shadow seeking, rotating light scan ("solar compass"),
 *        obstacle avoidance, anti-circling and "stuck" state handling.
 * @{
 */

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include <Arduino.h>
#include "smartvase_aliases.h"
#include "CarePolicy.h"   // CARE_SCAN_SECTORS (light scan sector buffers)

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
     * (M_IDLE, M_MOVING, M_AVOID_*, M_STUCK, M_SCAN_*) based on the sensor readings, the
     * ambient light and the desired target mode. In M_MOVING a target mode back to IDLE
     * (care layer settling, CLI/Hub `mode idle`) stops the motors immediately, instead of
     * letting them run until the 20 s safety timeout.
     *
     * @param v Structure holding the current obstacle distances.
     * @param cached_lux Current filtered LDR ADC reading, 0..1023 (negative = not valid yet).
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

    /*! @brief Wall-following sub-mode (local, bench-only for now — not a remote protobuf mode). */
    enum WallFollow : uint8_t {
        WALL_OFF   = 0,  /**< No wall-following: normal proportional/seeking drive. */
        WALL_LEFT  = 1,  /**< Follow the left wall (side sensor US5). */
        WALL_RIGHT = 2   /**< Follow the right wall (side sensor US6). */
    };

    /**
     * @brief Enables/disables local wall-following, which overrides light/shadow seeking while active.
     * @details Uses the two side ultrasonic sensors (US5/US6) via `wallFollowDrive` (NavPolicy.h) to keep
     *          a constant distance from the chosen wall. Bench/CLI only for now (`wall` command); to make
     *          it remotely commandable a new protobuf `SetModeCommand` value would be needed.
     * @param[in] mode WALL_OFF, WALL_LEFT or WALL_RIGHT.
     */
    void setWallFollow(WallFollow mode) { wallFollowMode = mode; }

    /*! @brief Returns the current wall-following sub-mode. */
    WallFollow getWallFollow() const { return (WallFollow)wallFollowMode; }

    /**
     * @brief Starts a rotating light scan ("solar compass" primitive).
     *
     * @details With a single fixed LDR the direction of the light can only be
     *          obtained by moving: the robot rotates in place for a full turn
     *          (LIGHT_SCAN_TOTAL_MS, time-based — no encoders), accumulating the
     *          mean LDR ADC over CARE_SCAN_SECTORS angular sectors, then rotates
     *          again up to the best sector (brightest when seeking light,
     *          darkest when seeking shade, see careBestScanSector in
     *          CarePolicy.h) and resumes normal driving (CPP_M_MOVING) with the
     *          existing seeking bias. Called by the care layer (Care.cpp) on
     *          every transition into a seeking state.
     *
     *          Accepted only from CPP_M_IDLE, CPP_M_MOVING or an already running
     *          scan (which simply restarts the rotation): the avoidance and
     *          stuck-recovery states keep priority. Harmless in degraded mode:
     *          handleMovementSM() forces the FSM back to IDLE with motors off
     *          before any scan rotation can start.
     *
     * @param seekLight true = seek the brightest sector, false = the darkest.
     * @param stats     Cumulative statistics (counts the seeking session).
     */
    void startLightScan(bool seekLight, CumulativeStats& stats);

    /*! @brief true while a light scan (rotate or align phase) is in progress. */
    bool scanInProgress() const {
        return currentMovementState == CPP_M_SCAN_ROTATE ||
               currentMovementState == CPP_M_SCAN_ALIGN;
    }

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

    /**
     * @brief Activates the motors to move the robot forward (public for mfp0 test).
     */
    void moveForward(const DeviceConfig& config);

private:

    /**
     * @brief Low-level differential-drive primitive: sets direction + PWM for each wheel.
     * @details Signed speeds: sign = direction (positive = forward), magnitude 0..255 = PWM.
     *          0 = brake (both direction pins low, PWM off). All the higher-level maneuvers
     *          (moveForward/Backward/turnLeft/turnRight) and the proportional/wall-follow
     *          navigation route through this. Magnitudes are clamped to 255.
     * @param[in] left  Left wheel signed speed (−255..255).
     * @param[in] right Right wheel signed speed (−255..255).
     */
    void driveMotors(int16_t left, int16_t right);

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
    uint8_t          wallFollowMode;          /**< Wall-following sub-mode (WallFollow: 0=off, 1=left, 2=right) */

    // --- Light scan (see startLightScan) ---
    bool             scanSeekLight;                     /**< true = current scan seeks light, false = shade. */
    unsigned long    scanStartMs;                       /**< Timestamp of the scan rotation start. */
    unsigned long    scanAlignUntilMs;                  /**< End timestamp of the align rotation toward the best sector. */
    unsigned long    scanLastSampleMs;                  /**< Timestamp of the last accepted LDR sample (throttles the sampling: the main loop runs at kHz rates, far faster than the LDR EMA changes). */
    float            scanSectorSum[CARE_SCAN_SECTORS];  /**< Per-sector accumulated LDR ADC during the rotation. */
    uint16_t         scanSectorCnt[CARE_SCAN_SECTORS];  /**< Per-sector number of valid samples (16-bit: at kHz loop rates an 8-bit counter would overflow within one 500 ms sector and corrupt the mean). */
};

#endif // MOVEMENT_H

/** @} */ // end of MegaMovement
