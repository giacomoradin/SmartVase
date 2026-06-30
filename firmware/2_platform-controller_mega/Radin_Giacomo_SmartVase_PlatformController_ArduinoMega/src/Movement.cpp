/*!
 * @file Movement.cpp
 * @ingroup MegaMovement
 * @brief Implementazione della classe Movement: driver motori H-bridge, FSM di navigazione,
 *        seeking luce/ombra con anti-circling, obstacle avoidance e stato "stuck" con backoff.
 * @date 2026-04-29
 * @author Giacomo Radin
 */

#include "Movement.h"
#include "SensorPolicy.h"
#include <avr/wdt.h>

// =================================================================
// Driver motori (PIN map autoritativo docs/PINS - Sheet1.csv)
// H-bridge canale A (sinistra) + canale B (destra).
// =================================================================
#define MOTOR_LEFT_ENA   6   // PWM
#define MOTOR_LEFT_IN1   43
#define MOTOR_LEFT_IN2   45
#define MOTOR_RIGHT_ENB  7   // PWM
#define MOTOR_RIGHT_IN3  47
#define MOTOR_RIGHT_IN4  49

// Soglia di "ostacolo vicino" sulle sonde frontali (cm)
#define FRONT_OBSTACLE_CM  20.0f
// Soglia per le sonde laterali (cm) — meno restrittiva
#define SIDE_OBSTACLE_CM   12.0f
// Timeout safety motori sempre attivi (ms)
#define MOTOR_SAFETY_TIMEOUT_MS  20000UL

// Inizializza i valori predefiniti della state machine (vedi Movement.h per il dettaglio dei campi).
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
    pinMode(MOTOR_LEFT_ENA, OUTPUT);
    pinMode(MOTOR_LEFT_IN1, OUTPUT);
    pinMode(MOTOR_LEFT_IN2, OUTPUT);
    pinMode(MOTOR_RIGHT_ENB, OUTPUT);
    pinMode(MOTOR_RIGHT_IN3, OUTPUT);
    pinMode(MOTOR_RIGHT_IN4, OUTPUT);
    analogWrite(MOTOR_LEFT_ENA, 0);
    analogWrite(MOTOR_RIGHT_ENB, 0);
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    digitalWrite(MOTOR_LEFT_IN2, LOW);
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    digitalWrite(MOTOR_RIGHT_IN4, LOW);
}

void Movement::stopMotors(CumulativeStats& stats) {
    analogWrite(MOTOR_LEFT_ENA, 0);
    analogWrite(MOTOR_RIGHT_ENB, 0);
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    digitalWrite(MOTOR_LEFT_IN2, LOW);
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    digitalWrite(MOTOR_RIGHT_IN4, LOW);
    if (motorActiveStartTime > 0) {
        stats.total_motor_active_time_s += (millis() - motorActiveStartTime) / 1000UL;
        motorActiveStartTime = 0;
    }
}

void Movement::moveForward(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_ENA,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_IN1, HIGH);
    digitalWrite(MOTOR_LEFT_IN2, LOW);
    analogWrite(MOTOR_RIGHT_ENB, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_IN3, HIGH);
    digitalWrite(MOTOR_RIGHT_IN4, LOW);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::moveBackward(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_ENA,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    digitalWrite(MOTOR_LEFT_IN2, HIGH);
    analogWrite(MOTOR_RIGHT_ENB, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    digitalWrite(MOTOR_RIGHT_IN4, HIGH);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::turnRight(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_ENA,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_IN1, HIGH);
    digitalWrite(MOTOR_LEFT_IN2, LOW);
    analogWrite(MOTOR_RIGHT_ENB, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    digitalWrite(MOTOR_RIGHT_IN4, HIGH);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

void Movement::turnLeft(const DeviceConfig& config) {
    analogWrite(MOTOR_LEFT_ENA,  config.motorCalibLeft);
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    digitalWrite(MOTOR_LEFT_IN2, HIGH);
    analogWrite(MOTOR_RIGHT_ENB, config.motorCalibRight);
    digitalWrite(MOTOR_RIGHT_IN3, HIGH);
    digitalWrite(MOTOR_RIGHT_IN4, LOW);
    if (motorActiveStartTime == 0) motorActiveStartTime = millis();
}

bool Movement::frontBlocked(const ObstacleView& v) const {
    auto near = [](float d) { return !isnan(d) && d < FRONT_OBSTACLE_CM; };
    return near(v.top) || near(v.front_right) || near(v.front_left);
}

void Movement::handleMovementSM(const ObstacleView& v, int cached_lux,
                                const DeviceConfig& config, CumulativeStats& stats,
                                bool degradedModeActive) {
    // In degraded mode il motore va fermo e la SM resta in IDLE.
    if (degradedModeActive) {
        if (currentMovementState != CPP_M_IDLE) {
            stopMotors(stats);
            currentMovementState = CPP_M_IDLE;
        }
        return;
    }

    // Safety: nessun movimento puo' durare ininterrotto > MOTOR_SAFETY_TIMEOUT_MS.
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
            // Dopo 60 s di marcia senza ostacoli il backoff anti-stuck torna
            // al valore base: gli stuck passati non devono penalizzare per
            // sempre i recovery futuri.
            if (millis() - stateStartTime > 60000UL) {
                stateStartTime        = millis();
                avoidance_attempts    = 0;
                current_stuck_backoff = 30000UL;
            }
            if (front_obs) {
                stats.obstacles_avoided++;
                seekTurnStartMs     = 0;   // reset anti-circling prima dell'avoidance
                seekRelocateUntilMs = 0;
                currentMovementState = CPP_M_AVOID_START;
            } else {
                // Seeking luce/ombra con ANTI-CIRCLING: se la rotazione verso la
                // sorgente dura troppo senza raggiungere la soglia (es. luce
                // uniforme), invece di girare in tondo all'infinito il robot si
                // "rilocalizza" avanzando un istante e poi riprova. Mai piu'
                // rischioso di moveForward (che parte solo a fronte libero).
                // Tempi tarabili a banco.
                const unsigned long SEEK_TURN_MAX_MS = 8000UL;  // ~1 giro lento
                const unsigned long SEEK_RELOCATE_MS = 2000UL;
                const bool wantTurn = seekWantsTurn(targetMode == CPP_LIGHT,
                                                    targetMode == CPP_SHADOW,
                                                    cached_lux, lightThr);

                if (millis() < seekRelocateUntilMs) {
                    moveForward(config);                  // fase di rilocazione
                } else if (wantTurn) {
                    if (seekTurnStartMs == 0) seekTurnStartMs = millis();
                    if (millis() - seekTurnStartMs > SEEK_TURN_MAX_MS) {
                        seekTurnStartMs     = 0;
                        seekRelocateUntilMs = millis() + SEEK_RELOCATE_MS;
                        moveForward(config);
                    } else if (targetMode == CPP_LIGHT) {
                        turnRight(config);                // troppo buio -> cerca luce
                    } else {
                        turnLeft(config);                 // troppa luce -> cerca ombra
                    }
                } else {
                    seekTurnStartMs = 0;                  // soglia raggiunta / IDLE
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
                // Verifica se siamo riusciti a liberare il fronte.
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
                    stateStartTime       = millis(); // riparte la finestra dei 60 s
                }
                break;
            }
            // Direzione di rotazione: preferisci il lato libero.
            // Se entrambi liberi/occupati allo stesso modo, scegli random.
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
                current_stuck_backoff += 10000UL; // backoff esponenziale (incremento lineare di 10s)
            }
            break;
    }
}

void Movement::setTargetMode(CppMode mode) {
    targetMode = mode;
}

void Movement::testMove(char dir, uint16_t ms, const DeviceConfig& config) {
    // Helper bloccante per CLI manuale. Tetto duro a MAX_TEST_MS per
    // restare ben sotto MOTOR_SAFETY_TIMEOUT_MS, e wdt_reset() periodico
    // per non far scattare il watchdog (4 s) durante l'attesa.
    const uint16_t MAX_TEST_MS = 5000;
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

    analogWrite(MOTOR_LEFT_ENA, 0);
    analogWrite(MOTOR_RIGHT_ENB, 0);
    digitalWrite(MOTOR_LEFT_IN1, LOW);
    digitalWrite(MOTOR_LEFT_IN2, LOW);
    digitalWrite(MOTOR_RIGHT_IN3, LOW);
    digitalWrite(MOTOR_RIGHT_IN4, LOW);
}
