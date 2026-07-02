/*!
 * @file Movement.cpp
 * @ingroup MegaMovement
 * @brief Implementation of the Movement class: VNH5019 motor driver, navigation FSM,
 *        light/shadow seeking with anti-circling, rotating light scan ("solar compass"),
 *        obstacle avoidance and "stuck" state with backoff.
 * @date 2026-04-29
 * @author Giacomo Radin
 */

#include "Movement.h"
#include "SensorPolicy.h"
#include "NavPolicy.h"
#include <avr/wdt.h>

// =================================================================
// Motor driver — Pololu Dual VNH5019 Shield (PIN map: docs/PINS - Sheet1.csv)
// =================================================================
// VNH5019 interface per motor (4 signals + 1 optional diagnostic):
//   INA, INB  -> direction (digitalWrite HIGH/LOW)
//   PWM       -> speed (analogWrite); must be a PWM pin
//   EN/DIAG   -> enable + fault diagnostics. Open-drain with a pull-up on the
//                shield (R1/R10 4.7k -> VDD): at rest HIGH = driver ENABLED;
//                the chip pulls it LOW on a fault (overtemp/overcurrent/under-
//                voltage). Must NOT be driven by the Mega (would disable it): we
//                only read it, as INPUT, to report the fault in 'diag'.
// The shield ties the chip's ENA and ENB together per motor ("EN A=B" jumper):
// hence a single EN line per motor.
//
// Pin mapping confirmed 2026-06-30 via multimeter continuity test (Mega pin ->
// shield connector), superseding the earlier best-guess mapping which had
// PWM/INA/INB from different shield channels paired together (the actual
// cause of the 0V-outputs bug seen on the bench):
//   D7  -> M1PWM   D41 -> M1INA   D43 -> M1INB
//   D6  -> M2PWM   D45 -> M2INA   D47 -> M2INB
#define MOTOR_LEFT_PWM   7   ///< Left motor -> M1PWM (speed, PWM pin). Continuity-confirmed 2026-06-30.
#define MOTOR_LEFT_INA   41  ///< Left motor -> M1INA (direction). Continuity-confirmed 2026-06-30.
#define MOTOR_LEFT_INB   43  ///< Left motor -> M1INB (direction). Continuity-confirmed 2026-06-30.
#define MOTOR_RIGHT_PWM  6   ///< Right motor -> M2PWM (speed, PWM pin). Continuity-confirmed 2026-06-30.
// INA/INB swapped vs. the physical M2INA/M2INB continuity mapping: bench test
// on 2026-06-30 showed the right motor spinning backward on 'motor f' with the
// direct mapping, so direction is inverted here in firmware instead of
// re-wiring. If the motor is later re-wired, swap these back and re-test.
#define MOTOR_RIGHT_INA  47  ///< Right motor direction pin A (wired to shield's M2INB; swapped for correct 'forward').
#define MOTOR_RIGHT_INB  45  ///< Right motor direction pin B (wired to shield's M2INA; swapped for correct 'forward').

// EN/DIAG (M1EN/M2EN) are NOT wired to the Mega yet (confirmed 2026-06-30):
// fault reading is disabled below instead of guessing pins. The driver still
// works fine without it (the shield enables it via its own pull-up); this
// only disables the optional fault diagnostics in 'diag'. Flip to 1 and set
// the real pins once EN/DIAG is wired.
#define MOTOR_EN_DIAG_WIRED 0  ///< 0 = EN/DIAG not wired to the Mega: faultLeft()/faultRight() always report "no fault".
#if MOTOR_EN_DIAG_WIRED
#define MOTOR_LEFT_EN    -1  ///< TODO: set once M1EN/DIAG is wired to the Mega.
#define MOTOR_RIGHT_EN   -1  ///< TODO: set once M2EN/DIAG is wired to the Mega.
#endif

#define FRONT_OBSTACLE_CM  20.0f    ///< "Nearby obstacle" threshold on the front probes (cm).
#define SIDE_OBSTACLE_CM   12.0f    ///< "Nearby obstacle" threshold on the side probes (cm), less restrictive.
#define MOTOR_SAFETY_TIMEOUT_MS  20000UL  ///< Maximum uninterrupted movement time before the safety stop (ms).
#define SEEK_BIAS_PWM      60.0f    ///< Gentle steering bias (PWM) added while light/shadow seeking wants to turn.

/*! @brief Duration of the full light-scan rotation (ms). Time-based (no encoders):
           roughly one in-place turn at calibration PWM. BENCH-TUNE: measure a real
           360° with `motor l/r` and adjust so the scan covers about one full turn.
           Must stay well under MOTOR_SAFETY_TIMEOUT_MS together with the align phase. */
#define LIGHT_SCAN_TOTAL_MS  6000UL

/*! @brief Minimum spacing between two LDR samples during the scan rotation (ms).
           The main loop runs orders of magnitude faster than the LDR EMA evolves:
           sampling every iteration would only accumulate thousands of duplicated
           values per sector (risking counter/precision issues) without adding
           information. 20 ms gives ~25 samples per 500 ms sector. */
#define LIGHT_SCAN_SAMPLE_MS 20UL

// Initializes the state machine's default values (see Movement.h for field details).
Movement::Movement() :
    currentMovementState(CPP_M_IDLE),
    targetMode(CPP_IDLE),
    motorActiveStartTime(0),
    stateStartTime(0),
    avoidance_attempts(0),
    stuck_cooldown_start_time(0),
    current_stuck_backoff(30000UL),
    seekTurnStartMs(0),
    seekRelocateUntilMs(0),
    wallFollowMode(WALL_OFF),
    scanSeekLight(true),
    scanStartMs(0),
    scanAlignUntilMs(0),
    scanLastSampleMs(0)
{
    for (uint8_t i = 0; i < CARE_SCAN_SECTORS; ++i) {
        scanSectorSum[i] = 0.0f;
        scanSectorCnt[i] = 0;
    }
}

void Movement::init() {
    pinMode(MOTOR_LEFT_PWM, OUTPUT);
    pinMode(MOTOR_LEFT_INA, OUTPUT);
    pinMode(MOTOR_LEFT_INB, OUTPUT);
    pinMode(MOTOR_RIGHT_PWM, OUTPUT);
    pinMode(MOTOR_RIGHT_INA, OUTPUT);
    pinMode(MOTOR_RIGHT_INB, OUTPUT);
#if MOTOR_EN_DIAG_WIRED
    // EN/DIAG as INPUT_PULLUP: we NEVER drive these pins (the shield enables
    // the driver via its own external pull-up to VDD; driving them LOW
    // would disable the driver). The internal pull-up avoids false faults if the pin
    // is not wired (reads HIGH = "ok"). Read by faultLeft()/faultRight().
    pinMode(MOTOR_LEFT_EN, INPUT_PULLUP);
    pinMode(MOTOR_RIGHT_EN, INPUT_PULLUP);
#endif
    analogWrite(MOTOR_LEFT_PWM, 0);
    analogWrite(MOTOR_RIGHT_PWM, 0);
    digitalWrite(MOTOR_LEFT_INA, LOW);
    digitalWrite(MOTOR_LEFT_INB, LOW);
    digitalWrite(MOTOR_RIGHT_INA, LOW);
    digitalWrite(MOTOR_RIGHT_INB, LOW);
}

void Movement::stopMotors(CumulativeStats& stats) {
    analogWrite(MOTOR_LEFT_PWM, 0);
    analogWrite(MOTOR_RIGHT_PWM, 0);
    digitalWrite(MOTOR_LEFT_INA, LOW);
    digitalWrite(MOTOR_LEFT_INB, LOW);
    digitalWrite(MOTOR_RIGHT_INA, LOW);
    digitalWrite(MOTOR_RIGHT_INB, LOW);
    if (motorActiveStartTime > 0) {
        stats.total_motor_active_time_s += (millis() - motorActiveStartTime) / 1000UL;
        motorActiveStartTime = 0;
    }
}

bool Movement::faultLeft() const {
#if MOTOR_EN_DIAG_WIRED
    // EN/DIAG low while the driver should be enabled = VNH5019 fault.
    return digitalRead(MOTOR_LEFT_EN) == LOW;
#else
    // EN/DIAG not wired to the Mega yet (confirmed 2026-06-30): no fault
    // reporting available. The driver still runs fine (enabled via the
    // shield's own pull-up); this only disables the diagnostic in 'diag'.
    return false;
#endif
}

bool Movement::faultRight() const {
#if MOTOR_EN_DIAG_WIRED
    return digitalRead(MOTOR_RIGHT_EN) == LOW;
#else
    return false;
#endif
}

void Movement::driveMotors(int16_t left, int16_t right) {
    // Left wheel.
    if (left == 0) {
        digitalWrite(MOTOR_LEFT_INA, LOW);
        digitalWrite(MOTOR_LEFT_INB, LOW);
        analogWrite(MOTOR_LEFT_PWM, 0);
    } else {
        const bool fwd = (left > 0);
        int16_t mag = fwd ? left : (int16_t)-left;
        if (mag > 255) mag = 255;
        digitalWrite(MOTOR_LEFT_INA, fwd ? HIGH : LOW);
        digitalWrite(MOTOR_LEFT_INB, fwd ? LOW  : HIGH);
        analogWrite(MOTOR_LEFT_PWM, (uint8_t)mag);
    }
    // Right wheel.
    if (right == 0) {
        digitalWrite(MOTOR_RIGHT_INA, LOW);
        digitalWrite(MOTOR_RIGHT_INB, LOW);
        analogWrite(MOTOR_RIGHT_PWM, 0);
    } else {
        const bool fwd = (right > 0);
        int16_t mag = fwd ? right : (int16_t)-right;
        if (mag > 255) mag = 255;
        digitalWrite(MOTOR_RIGHT_INA, fwd ? HIGH : LOW);
        digitalWrite(MOTOR_RIGHT_INB, fwd ? LOW  : HIGH);
        analogWrite(MOTOR_RIGHT_PWM, (uint8_t)mag);
    }
    if ((left != 0 || right != 0) && motorActiveStartTime == 0) {
        motorActiveStartTime = millis();
    }
}

// The four fixed maneuvers are thin wrappers over driveMotors (kept for the CLI
// `motor`/`motortest` and the emergency-recovery FSM). motorCalibLeft/Right are
// the per-wheel PWM trims for straight driving.
void Movement::moveForward(const DeviceConfig& config) {
    driveMotors((int16_t)config.motorCalibLeft, (int16_t)config.motorCalibRight);
}

void Movement::moveBackward(const DeviceConfig& config) {
    driveMotors(-(int16_t)config.motorCalibLeft, -(int16_t)config.motorCalibRight);
}

void Movement::turnRight(const DeviceConfig& config) {
    driveMotors((int16_t)config.motorCalibLeft, -(int16_t)config.motorCalibRight);
}

void Movement::turnLeft(const DeviceConfig& config) {
    driveMotors(-(int16_t)config.motorCalibLeft, (int16_t)config.motorCalibRight);
}

bool Movement::frontBlocked(const ObstacleView& v) const {
    auto near = [](float d) { return !isnan(d) && d < FRONT_OBSTACLE_CM; };
    return near(v.top) || near(v.front_right) || near(v.front_left);
}

void Movement::handleMovementSM(const ObstacleView& v, int cached_lux,
                                const DeviceConfig& config, CumulativeStats& stats,
                                bool degradedModeActive) {
    // In degraded mode the motors must stay off and the SM remains in IDLE.
    if (degradedModeActive) {
        if (currentMovementState != CPP_M_IDLE) {
            stopMotors(stats);
            currentMovementState = CPP_M_IDLE;
        }
        return;
    }

    // Safety: no movement can last uninterrupted for > MOTOR_SAFETY_TIMEOUT_MS.
    if (motorActiveStartTime > 0 && millis() - motorActiveStartTime > MOTOR_SAFETY_TIMEOUT_MS) {
        stopMotors(stats);
        currentMovementState = CPP_M_IDLE;
        return;
    }

    const bool front_obs = frontBlocked(v);
    const bool left_obs  = !isnan(v.left)  && v.left  < SIDE_OBSTACLE_CM;
    const bool right_obs = !isnan(v.right) && v.right < SIDE_OBSTACLE_CM;
    const int  lightThr  = config.light_threshold;

    switch (currentMovementState) {
        case CPP_M_IDLE:
            stopMotors(stats);
            if (targetMode != CPP_IDLE) {
                currentMovementState      = CPP_M_MOVING;
                motorActiveStartTime      = millis();
                stateStartTime            = millis();
                avoidance_attempts        = 0;
                current_stuck_backoff     = 30000UL;
                if (targetMode == CPP_LIGHT)  stats.light_seeking_sessions++;
                if (targetMode == CPP_SHADOW) stats.shadow_seeking_sessions++;
            }
            break;

        case CPP_M_MOVING: {
            // Target back to IDLE (care layer settling, CLI/Hub 'mode idle'):
            // stop immediately instead of waiting for the 20 s safety timeout.
            if (targetMode == CPP_IDLE) {
                stopMotors(stats);
                currentMovementState = CPP_M_IDLE;
                break;
            }

            // After 60 s of driving without obstacles, the anti-stuck backoff
            // resets to the base value: past stuck events must not permanently
            // penalize future recoveries.
            if (millis() - stateStartTime > 60000UL) {
                stateStartTime        = millis();
                avoidance_attempts    = 0;
                current_stuck_backoff = 30000UL;
            }

            // Proportional differential drive (NavPolicy.h): continuous steering
            // away from obstacles instead of the old bang-bang stop-and-turn. The
            // hard emergency case (a front sensor < emergency threshold) is still
            // handled by the reverse/turn recovery FSM below.
            NavDistances nd;
            nd.top = v.top; nd.front_right = v.front_right; nd.front_left = v.front_left;
            nd.left = v.left; nd.right = v.right;
            const NavParams np = navDefaultParams();

            WheelCmd cmd;
            if (wallFollowMode != WALL_OFF) {
                // Wall-following overrides seeking: keep a constant distance from
                // the chosen wall using the side sensor (US5/US6).
                cmd = wallFollowDrive(nd, (int16_t)config.motorCalibLeft,
                                      (int16_t)config.motorCalibRight, np,
                                      wallFollowMode == WALL_LEFT);
            } else {
                // Light/shadow seeking as a gentle steering bias: with a single
                // LDR there is no left/right light gradient, so while below/above
                // threshold we add a fixed bias and let obstacle avoidance shape
                // the rest (virtual-force-field style composition).
                float seekBias = 0.0f;
                if (seekWantsTurn(targetMode == CPP_LIGHT, targetMode == CPP_SHADOW,
                                  cached_lux, lightThr)) {
                    seekBias = (targetMode == CPP_LIGHT) ? SEEK_BIAS_PWM : -SEEK_BIAS_PWM;
                }
                cmd = proportionalDrive(nd, (int16_t)config.motorCalibLeft,
                                        (int16_t)config.motorCalibRight, np, seekBias);
            }

            if (cmd.emergency) {
                stats.obstacles_avoided++;
                currentMovementState = CPP_M_AVOID_START;
            } else {
                driveMotors(cmd.left, cmd.right);
            }
            break;
        }

        case CPP_M_AVOID_START:
            stopMotors(stats);
            stateStartTime       = millis();
            currentMovementState = CPP_M_AVOID_REVERSING;
            stats.escape_attempts++;
            break;

        case CPP_M_AVOID_REVERSING:
            moveBackward(config);
            if (millis() - stateStartTime > config.avoid_reverse_ms) {
                stateStartTime       = millis();
                currentMovementState = CPP_M_AVOID_TURNING;
            }
            break;

        case CPP_M_AVOID_TURNING: {
            if (millis() - stateStartTime > config.avoid_turn_ms) {
                // Check whether we managed to clear the front.
                if (front_obs) {
                    avoidance_attempts++;
                    if (avoidance_attempts >= 3) {
                        stats.stuck_events++;
                        currentMovementState      = CPP_M_STUCK;
                        stuck_cooldown_start_time = millis();
                    } else {
                        currentMovementState = CPP_M_AVOID_START;
                    }
                } else {
                    currentMovementState = CPP_M_MOVING;
                    stateStartTime       = millis(); // restarts the 60 s window
                }
                break;
            }
            // Rotation direction: prefer the free side.
            // If both are equally free/occupied, pick randomly.
            if (!right_obs && left_obs)       turnRight(config);
            else if (!left_obs && right_obs)  turnLeft(config);
            else if (random(0, 2) == 0)       turnLeft(config);
            else                              turnRight(config);
            break;
        }

        case CPP_M_STUCK:
            stopMotors(stats);
            if (millis() - stuck_cooldown_start_time > current_stuck_backoff) {
                currentMovementState   = CPP_M_IDLE;
                current_stuck_backoff += 10000UL; // exponential backoff (linear 10 s increment)
            }
            break;

        case CPP_M_SCAN_ROTATE: {
            // Light scan, phase 1: rotate in place for ~one full turn while
            // accumulating the LDR ADC per time sector ("solar compass" with a
            // single fixed sensor). Obstacles are not evaluated here: the
            // rotation is in place (no translation), and the global motor
            // safety timeout still bounds the whole maneuver.
            if (targetMode == CPP_IDLE) {           // scan aborted (mode change)
                stopMotors(stats);
                currentMovementState = CPP_M_IDLE;
                break;
            }
            turnRight(config);
            const unsigned long elapsed  = millis() - scanStartMs;
            const unsigned long sectorMs = LIGHT_SCAN_TOTAL_MS / CARE_SCAN_SECTORS;
            if (cached_lux >= 0 &&
                millis() - scanLastSampleMs >= LIGHT_SCAN_SAMPLE_MS) {
                scanLastSampleMs = millis();
                uint8_t idx = (uint8_t)(elapsed / sectorMs);
                if (idx >= CARE_SCAN_SECTORS) idx = CARE_SCAN_SECTORS - 1;
                scanSectorSum[idx] += (float)cached_lux;
                scanSectorCnt[idx]++;
            }
            if (elapsed >= LIGHT_SCAN_TOTAL_MS) {
                float mean[CARE_SCAN_SECTORS];
                for (uint8_t i = 0; i < CARE_SCAN_SECTORS; ++i) {
                    mean[i] = scanSectorCnt[i] ? (scanSectorSum[i] / (float)scanSectorCnt[i])
                                               : NAN;
                }
                const int8_t best = careBestScanSector(mean, CARE_SCAN_SECTORS, scanSeekLight);
                if (best < 0) {
                    // No valid light data collected (LDR blind): give up on the
                    // scan and fall back to plain seeking-biased driving.
                    currentMovementState = CPP_M_MOVING;
                    stateStartTime       = millis();
                } else {
                    // Phase 2: rotate again (same direction, time-based dead
                    // reckoning) up to the middle of the best sector.
                    scanAlignUntilMs = millis() + (unsigned long)best * sectorMs + sectorMs / 2;
                    currentMovementState = CPP_M_SCAN_ALIGN;
                }
            }
            break;
        }

        case CPP_M_SCAN_ALIGN:
            if (targetMode == CPP_IDLE) {           // scan aborted (mode change)
                stopMotors(stats);
                currentMovementState = CPP_M_IDLE;
                break;
            }
            turnRight(config);
            if ((long)(millis() - scanAlignUntilMs) >= 0) {
                // Aligned with the chosen sector: resume normal driving. The
                // residual heading error of the time-based alignment is
                // corrected naturally by the gradient climb that follows.
                currentMovementState = CPP_M_MOVING;
                stateStartTime       = millis();
            }
            break;
    }
}

void Movement::startLightScan(bool seekLight, CumulativeStats& stats) {
    // Only from benign states: the avoidance/stuck recovery keeps priority,
    // and a scan over a scan simply restarts the rotation.
    if (currentMovementState != CPP_M_IDLE &&
        currentMovementState != CPP_M_MOVING &&
        !scanInProgress()) {
        return;
    }
    scanSeekLight    = seekLight;
    scanStartMs      = millis();
    scanLastSampleMs = 0;   // accept the first sample immediately
    for (uint8_t i = 0; i < CARE_SCAN_SECTORS; ++i) {
        scanSectorSum[i] = 0.0f;
        scanSectorCnt[i] = 0;
    }
    // Count the seeking session here: the care-driven path enters MOVING via
    // the scan states, skipping the M_IDLE bookkeeping.
    if (currentMovementState == CPP_M_IDLE) {
        if (seekLight) stats.light_seeking_sessions++;
        else           stats.shadow_seeking_sessions++;
        motorActiveStartTime  = millis();
        avoidance_attempts    = 0;
        current_stuck_backoff = 30000UL;
    }
    stateStartTime       = millis();
    currentMovementState = CPP_M_SCAN_ROTATE;
}

void Movement::setTargetMode(CppMode mode) {
    targetMode = mode;
}

void Movement::testMove(char dir, uint16_t ms, const DeviceConfig& config) {
    // Blocking helper for manual CLI use (wheels lifted off the ground). Hard cap
    // at MAX_TEST_MS (aligned with the pump's cap, Pump.cpp) to allow an
    // extended continuous test without being unbounded; periodic wdt_reset()
    // in the loop below prevents the watchdog (4 s) from tripping during
    // the blocking wait.
    const uint16_t MAX_TEST_MS = 60000;
    if (ms > MAX_TEST_MS) ms = MAX_TEST_MS;

    switch (dir) {
        case 'f': moveForward(config);  break;
        case 'b': moveBackward(config); break;
        case 'l': turnLeft(config);     break;
        case 'r': turnRight(config);    break;
        default: return;
    }

    unsigned long t0 = millis();
    while (millis() - t0 < ms) {
        wdt_reset();
        delay(50);
    }

    analogWrite(MOTOR_LEFT_PWM, 0);
    analogWrite(MOTOR_RIGHT_PWM, 0);
    digitalWrite(MOTOR_LEFT_INA, LOW);
    digitalWrite(MOTOR_LEFT_INB, LOW);
    digitalWrite(MOTOR_RIGHT_INA, LOW);
    digitalWrite(MOTOR_RIGHT_INB, LOW);
}
