/*!
 * @file Movement.h
 * @ingroup MegaMovement
 * @brief Interfaccia della FSM di movimento autonomo (seeking luce/ombra, obstacle avoidance, stuck recovery).
 * @date 2026-04-29
 * @author Giacomo Radin
 */

/**
 * @defgroup MegaMovement Movimento e navigazione (Mega)
 * @brief State machine motori, seeking luce/ombra, obstacle avoidance, anti-circling e gestione stato "stuck".
 * @{
 */

#ifndef MOVEMENT_H
#define MOVEMENT_H

#include <Arduino.h>
#include "smartvase_aliases.h"

class Sensors;

/**
 * @struct ObstacleView
 * @brief Snapshot delle distanze rilevate dai sensori ad ultrasuoni (in cm).
 *
 * I valori sono impostati a NAN se la lettura del sensore corrispondente non è valida o è in errore.
 */
struct ObstacleView {
    float top;          /**< US1: Frontale alto (anti-collisione superiore) */
    float front_right;  /**< US2: Frontale destro */
    float front_left;   /**< US3: Frontale sinistro */
    float left;         /**< US5: Laterale sinistro */
    float right;        /**< US6: Laterale destro */
};

/**
 * @class Movement
 * @brief FSM (Finite State Machine) per il movimento autonomo e la ricerca delle condizioni ottimali.
 *
 * La classe si occupa di pilotare i motori per seguire la luce o l'ombra (seeking) e per evitare
 * gli ostacoli rilevati dai sensori ad ultrasuoni (obstacle avoidance). Gestisce inoltre lo stato
 * di "stuck" (bloccato) con backoff esponenziale.
 */
class Movement {
public:
    /**
     * @brief Costruttore della classe Movement.
     */
    Movement();

    /**
     * @brief Inizializza i pin del driver dei motori.
     */
    void init();

    /**
     * @brief Gestisce lo stato e la logica di movimento ad ogni ciclo.
     * 
     * Da chiamare ad alta frequenza nel main loop. Coordina la state machine di movimento
     * (M_IDLE, M_MOVING, M_AVOID_*, M_STUCK) in base alle letture dei sensori, alla luminosità
     * e alla modalità target desiderata.
     * 
     * @param v Struttura contenente le distanze attuali dagli ostacoli.
     * @param cached_lux Valore di luminosità attuale filtrato (lux).
     * @param config Configurazione corrente salvata in EEPROM.
     * @param stats Statistiche cumulative di utilizzo (per contare sessioni di seeking ed evitare circling).
     * @param degradedModeActive Flag che indica se il sistema è in modalità degradata (RAM insufficiente, ecc.).
     */
    void handleMovementSM(const ObstacleView& v, int cached_lux,
                          const DeviceConfig& config, CumulativeStats& stats,
                          bool degradedModeActive);

    /**
     * @brief Arresta immediatamente i motori del robot.
     * 
     * @param stats Riferimento alle statistiche per aggiornare lo stato di attività.
     */
    void stopMotors(CumulativeStats& stats);

    /**
     * @brief Imposta la modalità target del robot.
     * 
     * @param mode Nuova modalità di funzionamento (LIGHT, SHADOW, IDLE).
     */
    void setTargetMode(CppMode mode);

    /**
     * @brief Restituisce la modalità target impostata.
     * @return CppMode Modalità target corrente.
     */
    CppMode getTargetMode() const { return targetMode; }

    /**
     * @brief Restituisce lo stato di movimento corrente della state machine.
     * @return CppMovementState Stato di movimento corrente.
     */
    CppMovementState getCurrentState() const { return currentMovementState; }

    /**
     * @brief Esegue un movimento di test (avanti, indietro, sinistra, destra) per un tempo determinato.
     * 
     * Usato principalmente per debug da interfaccia CLI seriale.
     * 
     * @param dir Carattere indicante la direzione ('F'=Avanti, 'B'=Indietro, 'L'=Sinistra, 'R'=Destra).
     * @param ms Durata del movimento di test in millisecondi.
     * @param config Configurazione contenente i parametri di velocità/potenza dei motori.
     */
    void testMove(char dir, uint16_t ms, const DeviceConfig& config);

private:
    /**
     * @brief Attiva i motori per muovere il robot in avanti.
     */
    void moveForward(const DeviceConfig& config);

    /**
     * @brief Attiva i motori per muovere il robot all'indietro.
     */
    void moveBackward(const DeviceConfig& config);

    /**
     * @brief Ruota il robot verso destra sul proprio asse.
     */
    void turnRight(const DeviceConfig& config);

    /**
     * @brief Ruota il robot verso sinistra sul proprio asse.
     */
    void turnLeft(const DeviceConfig& config);

    /**
     * @brief Verifica se la traiettoria frontale è bloccata da ostacoli.
     * 
     * Combina i dati dei sensori US1 (frontale alto), US2 (frontale destro) e US3 (frontale sinistro).
     * 
     * @param v Vista degli ostacoli correnti.
     * @return true se c'è un ostacolo vicino sul fronte, false altrimenti.
     */
    bool frontBlocked(const ObstacleView& v) const;

    CppMovementState currentMovementState;    /**< Stato corrente della FSM dei motori */
    CppMode          targetMode;              /**< Modalità target impostata dall'utente o dal cloud */
    unsigned long    motorActiveStartTime;    /**< Timestamp di inizio dell'attività dei motori */
    unsigned long    stateStartTime;           /**< Timestamp di ingresso nello stato FSM corrente */
    uint8_t          avoidance_attempts;      /**< Numero di tentativi consecutivi di evitamento ostacolo falliti */
    unsigned long    stuck_cooldown_start_time;/**< Inizio del cooldown di attesa nello stato M_STUCK */
    uint32_t         current_stuck_backoff;   /**< Durata del backoff attuale per lo stato stuck (in ms) */
    unsigned long    seekTurnStartMs;         /**< Timestamp di inizio rotazione nella ricerca (per evitare circling) */
    unsigned long    seekRelocateUntilMs;     /**< Limite temporale entro il quale completare la rilocazione */
};

#endif // MOVEMENT_H

/** @} */ // end of MegaMovement
