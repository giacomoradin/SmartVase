/*!
 * @file Movement.cpp
 * @ingroup MegaMovement
 * @brief Implementation of the Movement class: H-bridge motor driver, navigation FSM,
 *        light/shadow seeking with anti-circling, obstacle avoidance and "stuck" state with backoff.
 * @date 2026-04-29
 * @author Giacomo Radin
 */

#include "Movement.h"
#include "SensorPolicy.h"
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
#define MOTOR_RIGHT_INA  45  ///< Right motor -> M2INA (direction). Continuity-confirmed 2026-06-30.
#define MOTOR_RIGHT_INB  47  ///< Right motor -> M2INB (direction). Continuity-confirmed 2026-06-30.

// EN/DIAG (M1EN/M2EN) are NOT wired to the Mega yet (confirmed 2026-06-30):
// fault reading is disabled below instead of guessing pins. The driver still
// works fine without it (the shield enables it via its own pull-up); this
// only disables the optional fault diagnostics in 'diag'. Flip to 1 and set
// the real pins once EN/DIAG is wired.
#define MOTOR_EN_DIAG_WIRED 0
#if MOTOR_EN_DIAG_WIRED
#define MOTOR_LEFT_EN    -1  ///< TODO: set once M1EN/DIAG is wired to the Mega.
#define MOTOR_RIGHT_EN   -1  ///< TODO: set once M2EN/DIAG is wired to the Mega.
#endif

#define FRONT_OBSTACLE_CM  20.0f    ///< "Nearby obstacle" threshold on the front probes (cm).
#define SIDE_OBSTACLE_CM   12.0f    ///< "Nearby obstacle" threshold on the side probes (cm), less restrictive.
#define MOTOR_SAFETY_TIMEOUT_MS  20000UL  ///< Maximum uninterrupted movement time before the safety stop (ms).

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
    seekRelocateUntilMs(0)
{
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

void Movement::moveForward(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_PWM,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_INA, HIGH);
    digitalWrite(MOTOR_LEFT_INB, LOW);
    analogWrite(MOTOR_RIGHT_PWM, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_INA, HIGH);
    digitalWrite(MOTOR_RIGHT_INB, LOW);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::moveBackward(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_PWM,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_INA, LOW);
    digitalWrite(MOTOR_LEFT_INB, HIGH);
    analogWrite(MOTOR_RIGHT_PWM, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_INA, LOW);
    digitalWrite(MOTOR_RIGHT_INB, HIGH);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::turnRight(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_PWM,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_INA, HIGH);
    digitalWrite(MOTOR_LEFT_INB, LOW);
    analogWrite(MOTOR_RIGHT_PWM, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_INA, LOW);
    digitalWrite(MOTOR_RIGHT_INB, HIGH);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::turnLeft(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_PWM,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_INA, LOW);
    digitalWrite(MOTOR_LEFT_INB, HIGH);
    analogWrite(MOTOR_RIGHT_PWM, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_INA, HIGH);
    digitalWrite(MOTOR_RIGHT_INB, LOW);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
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

        case CPP_M_MOVING:
            // After 60 s of driving without obstacles, the anti-stuck backoff
            // resets to the base value: past stuck events must not permanently
            // penalize future recoveries.
            if (millis() - stateStartTime > 60000UL) {
                stateStartTime        = millis();
                avoidance_attempts    = 0;
                current_stuck_backoff = 30000UL;
            }
            if (front_obs) {
                stats.obstacles_avoided++;
                seekTurnStartMs     = 0;   // reset anti-circling before avoidance
                seekRelocateUntilMs = 0;
                currentMovementState = CPP_M_AVOID_START;
            } else {
                // Light/shadow seeking with ANTI-CIRCLING: if the rotation towards
                // the source lasts too long without reaching the threshold (e.g.
                // uniform light), instead of spinning in circles forever the robot
                // "relocates" by driving forward for a moment and then retries. Never
                // riskier than moveForward (which only starts with a clear front).
                // Timings tunable on the bench.
                const unsigned long SEEK_TURN_MAX_MS = 8000UL;  // ~1 slow turn
                const unsigned long SEEK_RELOCATE_MS = 2000UL;
                const bool wantTurn = seekWantsTurn(targetMode == CPP_LIGHT,
                                                    targetMode == CPP_SHADOW,
                                                    cached_lux, lightThr);

                if (millis() < seekRelocateUntilMs) {
                    moveForward(config);                  // relocation phase
                } else if (wantTurn) {
                    if (seekTurnStartMs == 0) seekTurnStartMs = millis();
                    if (millis() - seekTurnStartMs > SEEK_TURN_MAX_MS) {
                        seekTurnStartMs     = 0;
                        seekRelocateUntilMs = millis() + SEEK_RELOCATE_MS;
                        moveForward(config);
                    } else if (targetMode == CPP_LIGHT) {
                        turnRight(config);                // too dark -> seek light
                    } else {
                        turnLeft(config);                 // too bright -> seek shadow
                    }
                } else {
                    seekTurnStartMs = 0;                  // threshold reached / IDLE
                    moveForward(config);
                }
            }
            break;

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
    }
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
