/*!
    @file   NavPolicy.h

    @ingroup MegaPolicy

    @brief  Pure navigation policies: proportional obstacle avoidance and wall-following.

    @details `static inline` functions with no hardware dependency (only
             `<stdint.h>`/`<math.h>`), meant to be included both by the firmware
             (`Movement.cpp`) and by the host unit tests in `tests/host/`
             (see `test_nav_policy`). They compute the desired per-wheel speeds
             from the (already filtered) ultrasonic distances, replacing the
             old bang-bang "stop-and-turn" avoidance with a continuous,
             proportional differential-drive command.

             Distance convention: values are in cm; `NAN` means "no valid
             reading" and is treated as **clear/far** for steering (so a dead
             sensor does not create phantom repulsion) and as **not an
             obstacle** for the emergency check (a close obstacle must be a real
             small reading, never assumed from a missing one).

             Wheel-speed convention: signed, magnitude 0..255 (PWM), sign =
             direction. `proportionalDrive`/`wallFollowDrive` return
             forward-biased speeds (>= 0); the caller's emergency FSM handles
             reverse.

    @date   2026-06-30

    @author Giacomo Radin
*/

#ifndef NAV_POLICY_H
#define NAV_POLICY_H

#include <stdint.h>
#include <math.h>   // isnan, NAN, lroundf

/*!
    @addtogroup MegaPolicy
    @{
*/

/*!
    @struct NavDistances
    @brief  Snapshot of the (filtered) navigation distances in cm (NaN = invalid).
    @note   Mirror of `ObstacleView` (Movement.h), kept here so the pure policy
            stays self-contained and host-testable without pulling in Arduino.
*/
struct NavDistances {
    float top;          /**< US1: front-top. */
    float front_right;  /**< US2: front-right. */
    float front_left;   /**< US3: front-left. */
    float left;         /**< US5: left side. */
    float right;        /**< US6: right side. */
};

/*!
    @struct NavParams
    @brief  Tuning parameters for the navigation policies (all distances in cm).
*/
struct NavParams {
    float emergencyCm;   /**< A valid front reading below this triggers emergency avoidance. */
    float slowCm;        /**< Below this front distance the robot slows down and steers; above it goes full/straight. */
    float sideTargetCm;  /**< Wall-following target distance from the followed wall. */
    float minSpeedFrac;  /**< Speed floor (fraction of max) inside the slow zone, so it keeps creeping instead of stalling. */
    float steerGain;     /**< Steering aggressiveness: cm of clearance asymmetry → PWM differential. */
    float steerMaxPwm;   /**< Clamp on the steering differential (PWM units). */
};

/*!
    @struct WheelCmd
    @brief  Desired per-wheel command plus an emergency flag.
*/
struct WheelCmd {
    int16_t left;      /**< Left wheel signed speed (−255..255). */
    int16_t right;     /**< Right wheel signed speed (−255..255). */
    bool    emergency; /**< true if a front obstacle is within `emergencyCm`: caller should run the reverse/turn recovery. */
};

/*! @brief Returns sensible default navigation parameters. */
static inline NavParams navDefaultParams() {
    NavParams p;
    p.emergencyCm  = 20.0f;
    p.slowCm       = 40.0f;
    p.sideTargetCm = 20.0f;
    p.minSpeedFrac = 0.35f;
    p.steerGain    = 6.0f;
    p.steerMaxPwm  = 200.0f;
    return p;
}

/*! @brief Minimum of two floats (portable: avr-libc lacks a reliable `fminf`). */
static inline float navMinf(float a, float b) { return (a < b) ? a : b; }

/*! @brief Clamps `v` to the inclusive range [lo, hi]. */
static inline float navClampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*! @brief Clamps a float speed to a signed int16 PWM in [-255, 255]. */
static inline int16_t navClampPwm(float v) {
    if (v >  255.0f) return  255;
    if (v < -255.0f) return -255;
    return (int16_t)lroundf(v);
}

/*!
    @brief    "Clearance" of a sensor for steering: NaN or out-of-zone → capped at `cap`.
    @details  Distances beyond the slow zone carry no steering information, so
              they are capped; invalid readings are treated as fully clear.
    @param[in] d   Distance in cm (may be NaN).
    @param[in] cap Cap value (typically `slowCm`).
    @return   `cap` if `d` is NaN or ≥ `cap`, otherwise `d`.
*/
static inline float navClearance(float d, float cap) {
    if (isnan(d) || d >= cap) return cap;
    return d;
}

/*!
    @brief    True if any front sensor reports a real obstacle closer than `emergencyCm`.
    @details  NaN readings are ignored (a missing reading is never treated as a
              close obstacle), so a dead front sensor cannot force a permanent
              emergency.
    @param[in] d           Navigation distances.
    @param[in] emergencyCm Emergency threshold in cm.
    @return   true if `top`, `front_right` or `front_left` is a valid value < `emergencyCm`.
*/
static inline bool navFrontEmergency(const NavDistances& d, float emergencyCm) {
    if (!isnan(d.top)         && d.top         < emergencyCm) return true;
    if (!isnan(d.front_right) && d.front_right < emergencyCm) return true;
    if (!isnan(d.front_left)  && d.front_left  < emergencyCm) return true;
    return false;
}

/*!
    @brief    Smallest valid front distance, or `fallback` if none is valid.
    @param[in] d        Navigation distances.
    @param[in] fallback Value returned when all front sensors are NaN.
    @return   min over the valid front sensors, else `fallback`.
*/
static inline float navFrontMin(const NavDistances& d, float fallback) {
    float m = fallback;
    bool any = false;
    if (!isnan(d.top))         { m = any ? navMinf(m, d.top)         : d.top;         any = true; }
    if (!isnan(d.front_right)) { m = any ? navMinf(m, d.front_right) : d.front_right; any = true; }
    if (!isnan(d.front_left))  { m = any ? navMinf(m, d.front_left)  : d.front_left;  any = true; }
    return any ? m : fallback;
}

/*!
    @brief    Proportional differential-drive obstacle avoidance (replaces bang-bang).

    @details  Behavior by zone (front distance = nearest valid front sensor):
              - **Emergency** (a front sensor < `emergencyCm`): returns
                `{0, 0, emergency=true}` — the caller runs the reverse/turn FSM.
              - **Slow zone** (`emergencyCm`..`slowCm`): forward speed ramps down
                from full towards `minSpeedFrac`, and the robot steers **away**
                from the closer side (toward the larger clearance).
              - **Safe zone** (> `slowCm`, all clear): full speed, straight
                (only a residual seeking bias, if any, applies via `seekBiasPwm`).

              Steering is computed from the clearance asymmetry between the
              left group (front_left ∪ left) and the right group
              (front_right ∪ right): `steer = steerGain · (rightClear −
              leftClear)`, positive = steer right (toward the clearer right).
              An external `seekBiasPwm` (e.g. from light/shadow seeking, or 0)
              is added to the steering so obstacle avoidance and target seeking
              compose like a virtual-force field.

    @param[in] d           Filtered navigation distances (cm, NaN = invalid).
    @param[in] maxLeft     Max PWM for the left wheel (per-wheel straight-drive calibration).
    @param[in] maxRight    Max PWM for the right wheel.
    @param[in] p           Tuning parameters.
    @param[in] seekBiasPwm Extra steering bias in PWM (positive = bias right); 0 if no seeking.

    @return   Forward-biased wheel command, or an emergency flag.
*/
static inline WheelCmd proportionalDrive(const NavDistances& d,
                                         int16_t maxLeft, int16_t maxRight,
                                         const NavParams& p, float seekBiasPwm) {
    WheelCmd cmd; cmd.left = 0; cmd.right = 0; cmd.emergency = false;

    if (navFrontEmergency(d, p.emergencyCm)) {
        cmd.emergency = true;
        return cmd;
    }

    // Forward speed fraction: 1.0 above slowCm, ramping down to minSpeedFrac at emergencyCm.
    const float frontMin = navFrontMin(d, p.slowCm);
    float t = (frontMin - p.emergencyCm) / (p.slowCm - p.emergencyCm); // 0 at emergency, 1 at slow
    t = navClampf(t, 0.0f, 1.0f);
    const float frac = p.minSpeedFrac + (1.0f - p.minSpeedFrac) * t;

    // Steering from left/right clearance asymmetry (capped at slowCm).
    const float leftClear  = navMinf(navClearance(d.front_left,  p.slowCm),
                                     navClearance(d.left,         p.slowCm));
    const float rightClear = navMinf(navClearance(d.front_right, p.slowCm),
                                     navClearance(d.right,        p.slowCm));
    float steer = p.steerGain * (rightClear - leftClear) + seekBiasPwm;
    steer = navClampf(steer, -p.steerMaxPwm, p.steerMaxPwm);

    cmd.left  = navClampPwm(frac * (float)maxLeft  + steer);
    cmd.right = navClampPwm(frac * (float)maxRight - steer);
    // Forward-biased: never let steering reverse a wheel here (the emergency FSM owns reverse).
    if (cmd.left  < 0) cmd.left  = 0;
    if (cmd.right < 0) cmd.right = 0;
    return cmd;
}

/*!
    @brief    Wall-following differential drive using one side sensor (P controller).

    @details  Keeps the followed wall at `sideTargetCm`:
              - **Emergency** front obstacle → `{0,0,emergency=true}` (caller recovers).
              - Wall **visible**: `err = side − target`; a proportional steer
                turns toward the wall when too far and away when too close.
              - Wall **lost** (side NaN): gently curves toward the followed side
                to reacquire it.
              Sign is handled so that `followLeft` follows the left wall and
              `!followLeft` the right wall.

    @param[in] d          Filtered navigation distances (cm).
    @param[in] maxLeft    Max PWM for the left wheel.
    @param[in] maxRight   Max PWM for the right wheel.
    @param[in] p          Tuning parameters (`sideTargetCm`, `steerGain`, `steerMaxPwm`, `minSpeedFrac`, `slowCm`, `emergencyCm`).
    @param[in] followLeft true = follow the left wall, false = the right wall.

    @return   Forward-biased wheel command, or an emergency flag.
*/
static inline WheelCmd wallFollowDrive(const NavDistances& d,
                                       int16_t maxLeft, int16_t maxRight,
                                       const NavParams& p, bool followLeft) {
    WheelCmd cmd; cmd.left = 0; cmd.right = 0; cmd.emergency = false;

    if (navFrontEmergency(d, p.emergencyCm)) {
        cmd.emergency = true;
        return cmd;
    }

    // Slow down as the front gets closer (same ramp as proportionalDrive).
    const float frontMin = navFrontMin(d, p.slowCm);
    float t = navClampf((frontMin - p.emergencyCm) / (p.slowCm - p.emergencyCm), 0.0f, 1.0f);
    const float frac = p.minSpeedFrac + (1.0f - p.minSpeedFrac) * t;

    const float side = followLeft ? d.left : d.right;
    float steer;
    if (isnan(side)) {
        // Wall lost: curve toward the followed side to reacquire it.
        const float reacquire = 0.5f * p.steerMaxPwm;
        steer = followLeft ? -reacquire : reacquire;
    } else {
        // err > 0: too far from the wall → steer toward it; err < 0: too close → steer away.
        const float err = side - p.sideTargetCm;
        float s = p.steerGain * err;
        steer = followLeft ? -s : s;
        steer = navClampf(steer, -p.steerMaxPwm, p.steerMaxPwm);
    }

    cmd.left  = navClampPwm(frac * (float)maxLeft  + steer);
    cmd.right = navClampPwm(frac * (float)maxRight - steer);
    if (cmd.left  < 0) cmd.left  = 0;
    if (cmd.right < 0) cmd.right = 0;
    return cmd;
}

/*! @} */ // MegaPolicy

#endif // NAV_POLICY_H
