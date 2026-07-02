// =====================================================================
// Native host unit test (g++) for the PURE navigation policies:
//   - proportionalDrive (zone-based differential steering + emergency)
//   - wallFollowDrive   (side-sensor P wall-following)
//   - navFrontEmergency / navClearance helpers
// Includes the REAL code (NavPolicy.h, pure header: <stdint.h>+<math.h>).
// =====================================================================
#include <cstdio>
#include <cstdint>
#include <cmath>

#include "../../firmware/2_platform-controller_mega/Radin_Giacomo_SmartVase_PlatformController_ArduinoMega/src/NavPolicy.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  [FAIL] %s\n", (msg)); ++g_failures; } \
    else         { printf("  [ ok ] %s\n", (msg)); } \
} while (0)

static NavDistances mk(float top, float fr, float fl, float l, float r) {
    NavDistances d; d.top = top; d.front_right = fr; d.front_left = fl; d.left = l; d.right = r;
    return d;
}

int main() {
    printf("== test_nav_policy ==\n");
    const NavParams p = navDefaultParams();   // emergency=20, slow=40, sideTarget=20, steerGain=6, steerMax=200
    const int16_t ML = 255, MR = 240;

    // --- Emergency detection (NaN must NOT trigger) ---
    CHECK(navFrontEmergency(mk(10, 100, 100, 100, 100), 20.0f), "front-top 10cm -> emergency");
    CHECK(navFrontEmergency(mk(100, 5, 100, 100, 100), 20.0f),  "front-right 5cm -> emergency");
    CHECK(!navFrontEmergency(mk(NAN, NAN, NAN, NAN, NAN), 20.0f), "all NaN -> NO emergency (dead sensors)");
    CHECK(!navFrontEmergency(mk(50, 50, 50, 50, 50), 20.0f),      "all far -> no emergency");

    // --- proportionalDrive: safe zone -> straight full speed ---
    {
        WheelCmd c = proportionalDrive(mk(100, 100, 100, 100, 100), ML, MR, p, 0.0f);
        CHECK(!c.emergency, "open field -> no emergency");
        CHECK(c.left == ML && c.right == MR, "open field -> full speed both wheels, straight");
    }

    // --- proportionalDrive: emergency zone ---
    {
        WheelCmd c = proportionalDrive(mk(15, 100, 100, 100, 100), ML, MR, p, 0.0f);
        CHECK(c.emergency, "front 15cm -> emergency flag");
        CHECK(c.left == 0 && c.right == 0, "emergency -> wheels commanded to 0 (FSM takes over)");
    }

    // --- proportionalDrive: obstacle on the RIGHT -> steer LEFT (left slows) ---
    {
        // right clearance small (front_right 25), left clear -> steer negative -> left < right
        WheelCmd c = proportionalDrive(mk(100, 25, 100, 100, 100), ML, MR, p, 0.0f);
        CHECK(!c.emergency, "right obstacle 25cm (slow zone) -> no emergency");
        CHECK(c.left < c.right, "obstacle on right -> steer left (left wheel slower)");
    }

    // --- proportionalDrive: obstacle on the LEFT -> steer RIGHT (right slows) ---
    {
        WheelCmd c = proportionalDrive(mk(100, 100, 25, 100, 100), ML, MR, p, 0.0f);
        CHECK(c.right < c.left, "obstacle on left -> steer right (right wheel slower)");
    }

    // --- proportionalDrive: slow zone reduces speed vs open field ---
    {
        WheelCmd open  = proportionalDrive(mk(100, 100, 100, 100, 100), ML, MR, p, 0.0f);
        WheelCmd slow  = proportionalDrive(mk(30, 30, 30, 100, 100), ML, MR, p, 0.0f);
        CHECK((slow.left + slow.right) < (open.left + open.right), "front at 30cm -> slower than open field");
        CHECK(slow.left > 0 && slow.right > 0, "slow zone -> still creeping (minSpeedFrac floor)");
    }

    // --- proportionalDrive: seek bias steers even with a clear field ---
    {
        WheelCmd c = proportionalDrive(mk(100, 100, 100, 100, 100), ML, MR, p, +80.0f);
        CHECK(c.left > c.right, "positive seek bias -> steer right (left faster)");
    }

    // --- wallFollowDrive: follow LEFT wall, too far -> steer toward wall (left) ---
    {
        // left side reads 35 (target 20) -> too far -> turn left -> left wheel slower
        WheelCmd c = wallFollowDrive(mk(100, 100, 100, 35, 100), ML, MR, p, /*followLeft=*/true);
        CHECK(!c.emergency, "wall-follow open front -> no emergency");
        CHECK(c.left < c.right, "left wall too far -> steer toward it (turn left)");
    }

    // --- wallFollowDrive: follow LEFT wall, too close -> steer away (right) ---
    {
        WheelCmd c = wallFollowDrive(mk(100, 100, 100, 8, 100), ML, MR, p, true);
        CHECK(c.right < c.left, "left wall too close -> steer away (turn right)");
    }

    // --- wallFollowDrive: follow RIGHT wall, too far -> steer toward it (right) ---
    {
        WheelCmd c = wallFollowDrive(mk(100, 100, 100, 100, 35), ML, MR, p, /*followLeft=*/false);
        CHECK(c.right < c.left, "right wall too far -> steer toward it (turn right)");
    }

    // --- wallFollowDrive: wall lost (NaN side) -> curve toward followed side ---
    {
        WheelCmd cl = wallFollowDrive(mk(100, 100, 100, NAN, 100), ML, MR, p, true);
        CHECK(cl.left < cl.right, "left wall lost -> curve left to reacquire");
        WheelCmd cr = wallFollowDrive(mk(100, 100, 100, 100, NAN), ML, MR, p, false);
        CHECK(cr.right < cr.left, "right wall lost -> curve right to reacquire");
    }

    // --- wallFollowDrive: emergency front still recovers ---
    {
        WheelCmd c = wallFollowDrive(mk(12, 100, 100, 20, 100), ML, MR, p, true);
        CHECK(c.emergency, "wall-follow + front 12cm -> emergency");
    }

    if (g_failures == 0) { printf("RESULT: ALL PASSED\n"); return 0; }
    printf("RESULT: %d FAILED\n", g_failures);
    return 1;
}
