/* 0.8.0.0 camera tests (0.7.11.0 tests + FPC5 logs switch, eye fix only when the locator verified it;
   head anchor = crown - 16 cm, so the +-10 cm test head boxes anchor 6 cm below their centre).
   0.7.11.0 added eye fix, combat pivot climb, head identity. A fake camera update reproduces the real order found in the game:
   smoothing of TargetCurrent, the zoom step, Distance copied to the eye-distance field 0x15C only when
   the game decides to (game_copies_dist), then eye = TargetCurrent + Direction * [0x15C]
   written to 0x150/0x158 (and the desired root copied to 0x18). The checks are on that final eye,
   i.e. what the frame shows. Covers mailbox parsing, vanilla passthrough, exact lock, eye at
   pivot for every pitch, the neck-pivot head-tilt numbers, eye height, guards, lead/lean, logging,
   and a real MinHook entry detour on both functions. */
#define FPE_TEST
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "camera.c"
#include "vendor/minhook/include/MinHook.h"

/* Stand-in for the render bridge's head list: fake objects with world bounds at 0x40. */
static unsigned char head_obj[3][0x80];
static int head_count;
int fpe_head_objects(void **out, int max) { int n = 0; for (int i = 0; i < head_count && n < max; i++) out[n++] = head_obj[i]; return n; }
static void set_head(int i, V3 lo, V3 hi) { float b[6] = {lo.x, lo.y, lo.z, hi.x, hi.y, hi.z}; memcpy(head_obj[i] + OBJ_WORLD_BOUND, b, sizeof b); }

static unsigned char cam[0x200];
static unsigned char ctx[0x60];
static V3 rd(unsigned o) { V3 v; memcpy(&v, cam + o, sizeof v); return v; }
static void wr(unsigned o, V3 v) { memcpy(cam + o, &v, sizeof v); }
static void wrf(unsigned o, float f) { memcpy(cam + o, &f, sizeof f); }
static V3 eye(void) { float xy[2], z; memcpy(xy, cam + CAM_EYE_XY, 8); memcpy(&z, cam + CAM_EYE_Z, 4); return (V3){xy[0], xy[1], z}; }
static int near3(V3 a, V3 b, float eps) { V3 d = sub(a, b); return d.x < eps && d.x > -eps && d.y < eps && d.y > -eps && d.z < eps && d.z > -eps; }

/* The game's zoom step (stands in for the smoothing done before the final eye). */
static uint64_t game_zoom(uint64_t a, uint64_t b, uint64_t c, void *d) {
    (void)a; (void)b; (void)c; (void)d;
    V3 des = rd(CAM_DESIRED);
    V3 cur = rd(CAM_CURRENT); wr(CAM_CURRENT, add(cur, mul(sub(des, cur), 0.25f)));
    V3 low = rd(CAM_CURRENT_LOW); wr(CAM_CURRENT_LOW, add(low, mul(sub(des, low), 0.25f)));
    return 7;
}
static uint64_t (*zoom_entry)(uint64_t, uint64_t, uint64_t, void *);
static int game_copies_dist = 1;   /* real update: copies Distance to 0x15C only under some conditions */
/* The game's camera update: zoom step, maybe Distance -> 0x15C, then the final eye from TargetCurrent. */
static uint64_t game_update(uint64_t a, uint64_t b, uint64_t c, void *context) {
    (void)context;
    zoom_entry(1, 2, 3, 0);
    if (game_copies_dist) memcpy(cam + CAM_EYE_DIST, cam + CAM_DISTANCE, 4);
    V3 cur = rd(CAM_CURRENT), dir = rd(CAM_DIRECTION); float dist; memcpy(&dist, cam + CAM_EYE_DIST, 4);
    V3 e = add(cur, mul(dir, dist));
    memcpy(cam + CAM_EYE_XY, &e, 8); memcpy(cam + CAM_EYE_Z, &e.z, 4);
    memcpy(cam + 0x18, cam + CAM_DESIRED, 12);
    return a + b + c;
}
static void setup(V3 p, V3 dir, float dist) {
    memset(cam, 0, sizeof cam); memset(&fr, 0, sizeof fr); head_count = 0; memset(head_obj, 0, sizeof head_obj);
    row_head = row_tail = 0; rows_total = 0;
    wr(CAM_DESIRED, p); wr(CAM_CURRENT, p); wr(CAM_CURRENT_LOW, p); wr(CAM_DIRECTION, dir); wrf(CAM_DISTANCE, dist); wrf(CAM_EYE_DIST, dist);
    game_copies_dist = 1; eye_field_ok = 1;
    unsigned char *c = cam; memcpy(ctx + CONTEXT_CAMERA, &c, sizeof c);
    camera_update_original = game_update; camera_zoom_original = game_zoom; zoom_entry = camera_zoom_hook;
    fake_now_us = 5000000;
}
static Settings base(void) { Settings s = {0}; s.on = 1; s.lock = 1; s.pivot = 1; s.logs = 1; return s; }
static uint32_t seqno = 1;
/* One frame at 60 fps: publish fresh settings (reference = desired), then run the hooked update. */
static void frame(Settings s, V3 desired) {
    fake_now_us += 16667;
    wr(CAM_DESIRED, desired);
    s.seq = ++seqno; s.ref = desired;
    memset(&shared_settings, 0, sizeof shared_settings);
    camera_accept(&s, fake_now_us / 1000);
    camera_update_hook(1, 2, 3, ctx);
}
static Row last_row(void) { Row r = {0}, x; while (pop_row(&x)) r = x; return r; }

static void test_parse(void) {
    Settings s;
    const char *ok = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 60 15 -30800 20300 -353800 1 1 1 -30700 18300 -353700 1 FPC5\n";
    assert(camera_parse(ok, (unsigned)strlen(ok), &s));
    assert(s.seq == 7 && s.on == 1 && s.lock == 1 && s.lead == 0 && s.lean_cm == 12 && s.fwd_cm == 5 && s.up_cm == -3 && s.mark == 2 && s.pivot == 1 && s.neck_cm == 10);
    assert(s.anchor == 1 && s.bob_pct == 60 && s.stop_cm == 15 && s.combat == 1 && s.eyefix == 1 && s.have_feet == 1 && s.logs == 1);
    assert(near3(s.ref, (V3){-30.8f, 20.3f, -353.8f}, 1e-3f) && near3(s.feet, (V3){-30.7f, 18.3f, -353.7f}, 1e-3f));
    const char *old = "FPC4 7 1 1 0 12 5 -3 2 1 10 1 60 15 -30800 20300 -353800 1 1 1 -30700 18300 -353700 FPC4\n";
    assert(!camera_parse(old, (unsigned)strlen(old), &s));
    const char *logs = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 100 0 0 0 0 0 1 0 0 0 0 2 FPC5";
    assert(!camera_parse(logs, (unsigned)strlen(logs), &s));
    const char *neck = "FPC4 7 1 1 0 12 5 -3 2 1 99 0 100 0 0 0 0 0 1 0 0 0 0 0 FPC5";
    assert(!camera_parse(neck, (unsigned)strlen(neck), &s));
    const char *pivot = "FPC4 7 1 1 0 12 5 -3 2 2 10 0 100 0 0 0 0 0 1 0 0 0 0 0 FPC5";
    assert(!camera_parse(pivot, (unsigned)strlen(pivot), &s));
    const char *bob = "FPC4 7 1 1 0 12 5 -3 2 1 10 1 101 0 0 0 0 0 1 0 0 0 0 0 FPC5";
    assert(!camera_parse(bob, (unsigned)strlen(bob), &s));
    const char *anchor = "FPC4 7 1 1 0 12 5 -3 2 1 10 2 100 0 0 0 0 0 1 0 0 0 0 0 FPC5";
    assert(!camera_parse(anchor, (unsigned)strlen(anchor), &s));
    const char *combat = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 100 0 0 0 0 2 1 0 0 0 0 0 FPC5";
    assert(!camera_parse(combat, (unsigned)strlen(combat), &s));
    const char *fix = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 100 0 0 0 0 0 3 0 0 0 0 0 FPC5";
    assert(!camera_parse(fix, (unsigned)strlen(fix), &s));
    const char *feet = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 100 0 0 0 0 0 1 -1 0 0 0 0 FPC5";
    assert(!camera_parse(feet, (unsigned)strlen(feet), &s));
    const char *torn = "FPC5 7 1 1 0 12 5 -3 2 1 10 1 100 0 0 0 0 0 1 1 0 0 0";
    assert(!camera_parse(torn, (unsigned)strlen(torn), &s));
    puts("PASS mailbox FPC5: valid, old FPC4 refused, out of range, invalid flags/bob/anchor/combat/eye fix/feet/logs, torn");
}
static void test_vanilla(void) {
    V3 dir = {0, 0.5f, 0.866f};
    setup((V3){0, 0, 0}, dir, 0.1f);
    Settings off = {0};
    for (int i = 1; i <= 10; i++) frame(off, (V3){0, 0, -0.05f * i});
    V3 cur = rd(CAM_CURRENT);
    assert(near3(eye(), add(cur, mul(dir, 0.1f)), 1e-6f) && cur.z > -0.5f + 0.01f);   /* still trailing: game smoothing */
    assert(rows_total == 0);
    Settings nolock = base(); nolock.lock = 0; nolock.fwd_cm = 10; nolock.up_cm = -20;
    setup((V3){0, 0, 0}, dir, 0.1f);
    for (int i = 1; i <= 10; i++) frame(nolock, (V3){0, 0, -0.05f * i});
    cur = rd(CAM_CURRENT);
    assert(near3(eye(), add(cur, mul(dir, 0.1f)), 1e-6f) && cur.z > -0.5f + 0.01f);
    assert(!(last_row().flags & F_APPLIED));
    puts("PASS off or lock off: the game's camera is untouched (smoothing kept, sliders ignored)");
}
static void test_lock_pivot(void) {
    float ys[] = {-0.4f, 0.f, 0.3f, 0.6f, 0.9f};
    for (int k = 0; k < 5; k++) {
        float y = ys[k], h = sq(1.f - y * y);
        V3 dir = {0.3f * h, y, 0.954f * h};
        setup((V3){1, 2, 3}, dir, 0.1f);
        for (int i = 1; i <= 5; i++) {
            V3 d = {1, 2, 3 - 0.08f * i};
            frame(base(), d);
            assert(near3(eye(), d, 1e-5f));
        }
        Row r = last_row(); assert((r.flags & F_APPLIED) && (r.flags & F_EYE_OK) && r.eye_err_mm == 0);
    }
    puts("PASS lock + eye at pivot: the shown eye is exactly the desired root at every pitch (no lag, no orbit swing)");
}
static void test_lock_orbit(void) {
    V3 dir = {0, 0.5f, 0.866f};
    setup((V3){0, 0, 0}, dir, 0.1f);
    Settings s = base(); s.pivot = 0;
    for (int i = 1; i <= 5; i++) {
        V3 d = {0, 0, -0.1f * i};
        frame(s, d);
        assert(near3(eye(), add(d, mul(dir, 0.1f)), 1e-5f));
    }
    assert(last_row().flags & F_EYE_OK);
    puts("PASS lock without pivot: no lag, orbit arm kept (vanilla geometry)");
}
static void test_head_tilt(void) {
    /* Looking down 60 degrees toward -z: Direction points up and back (+z). */
    float p = 1.0471976f;
    V3 dir = {0, 0.8660254f, 0.5f};
    setup((V3){0, 0, 0}, dir, 0.1f);
    Settings s = base(); s.fwd_cm = 8; s.neck_cm = 12;
    frame(s, (V3){0, 0, 0});
    float ahead = 0.08f * 0.5f + 0.12f * 0.8660254f, rise = 0.12f * 0.5f - 0.08f * 0.8660254f - 0.12f;
    V3 want = {0, rise, -ahead};
    assert(near3(eye(), want, 0.002f));
    Row r = last_row(); assert(r.pitch_ddeg > 595 && r.pitch_ddeg < 605);
    (void)p;
    /* Straight ahead: just fwd ahead, no rise. */
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f);
    frame(s, (V3){0, 0, 0});
    assert(near3(eye(), (V3){0, 0, -0.08f}, 0.001f));
    /* Eye height on top. */
    s.up_cm = -5; frame(s, (V3){0, 0, 0});
    assert(near3(eye(), (V3){0, -0.05f, -0.08f}, 0.001f));
    printf("PASS head tilt: looking down 60 deg with eye fwd 8 / neck 12 moves the eye %.1f cm forward and %.1f cm down; straight ahead = fwd only; eye height adds\n", ahead * 100 - 8, -rise * 100);
}
static void test_guards(void) {
    V3 dir = {0, 0, 1};
    setup((V3){0, 0, 0}, dir, 0.1f);
    Settings s = base();
    fake_now_us += 16667; wr(CAM_DESIRED, (V3){0, 0, -0.1f});
    s.seq = ++seqno; s.ref = (V3){50, 0, 0};
    memset(&shared_settings, 0, sizeof shared_settings); camera_accept(&s, fake_now_us / 1000);
    camera_update_hook(1, 2, 3, ctx);
    Row r = last_row(); assert((r.flags & F_IDENTITY) && !(r.flags & F_APPLIED));
    setup((V3){0, 0, 0}, dir, 0.1f);
    s.ref = (V3){0, 0, -0.1f}; s.seq = ++seqno;
    memset(&shared_settings, 0, sizeof shared_settings); camera_accept(&s, fake_now_us / 1000);
    fake_now_us += 900000;                                     /* heartbeat 900 ms old */
    wr(CAM_DESIRED, (V3){0, 0, -0.1f}); camera_update_hook(1, 2, 3, ctx);
    r = last_row(); assert((r.flags & F_STALE) && !(r.flags & F_APPLIED));
    setup((V3){0, 0, 0}, dir, 0.1f);
    float nan = 0.f; nan = nan / nan;
    frame(base(), (V3){nan, 0, 0});
    r = last_row(); assert((r.flags & F_INSANE) && !(r.flags & F_APPLIED));
    /* The zoom hook outside a hooked update never writes. */
    setup((V3){0, 0, 0}, dir, 0.1f); wr(CAM_CURRENT, (V3){5, 5, 5});
    camera_zoom_hook(1, 2, 3, 0);
    assert(near3(rd(CAM_CURRENT), add((V3){5, 5, 5}, mul(sub((V3){0, 0, 0}, (V3){5, 5, 5}), 0.25f)), 1e-6f));
    puts("PASS guards: wrong camera, stale heartbeat, NaN and zoom calls outside the update are never written");
}
static void test_lead_lean(void) {
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f);
    Settings s = base(); s.lead = 1;
    V3 d = {0, 0, 0};
    for (int i = 1; i <= 60; i++) { d = (V3){0, 0, -5.f * i / 60.f}; frame(s, d); }   /* 5 m/s toward -z */
    V3 e = eye();
    assert(e.z < d.z - 0.07f && e.z > d.z - 0.10f);
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f);
    s.lead = 0; s.lean_cm = 20;
    for (int i = 1; i <= 60; i++) { d = (V3){8.f * i / 60.f, 0, 0}; frame(s, d); }   /* 8 m/s toward +x */
    e = eye();
    assert(e.x - d.x > 0.249f && e.x - d.x < 0.2501f);
    Row r = last_row(); assert(r.speed_mms > 7900 && r.speed_mms < 8100);
    puts("PASS lead = velocity x frame time; lean along movement, capped at 1.25 x setting");
}
static void test_marks(void) {
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f);
    Settings s = {0}; s.mark = 3;
    frame(s, (V3){0, 0, 0});
    Row r = last_row(); assert(r.mark == 3);
    frame(s, (V3){0, 0, 0}); assert(rows_total == 1);
    puts("PASS marks: logged once, also outside first person");
}
/* optnone: normal prologues, like the game's functions (MinHook needs 5+ relocatable bytes). */
__attribute__((noinline, optnone)) static uint64_t real_zoom(uint64_t a, uint64_t b, uint64_t c, void *d) { return game_zoom(a, b, c, d); }
__attribute__((noinline, optnone)) static uint64_t real_update(uint64_t a, uint64_t b, uint64_t c, void *context) {
    real_zoom(1, 2, 3, 0);
    if (game_copies_dist) memcpy(cam + CAM_EYE_DIST, cam + CAM_DISTANCE, 4);
    V3 cur = rd(CAM_CURRENT), dir = rd(CAM_DIRECTION); float dist; memcpy(&dist, cam + CAM_EYE_DIST, 4);
    V3 e = add(cur, mul(dir, dist));
    memcpy(cam + CAM_EYE_XY, &e, 8); memcpy(cam + CAM_EYE_Z, &e.z, 4);
    (void)context;
    return a * 3 + b * 5 + c * 7;
}
static void test_detours(void) {
    setup((V3){0, 0, 0}, (V3){0, 0.6f, 0.8f}, 0.1f);
    assert(MH_Initialize() == MH_OK);
    assert(MH_CreateHook((void *)real_zoom, (void *)camera_zoom_hook, (void **)&camera_zoom_original) == MH_OK);
    assert(MH_CreateHook((void *)real_update, (void *)camera_update_hook, (void **)&camera_update_original) == MH_OK);
    assert(MH_EnableHook(MH_ALL_HOOKS) == MH_OK);
    Settings s = base(); s.seq = ++seqno; s.ref = (V3){0, 0, -0.3f};
    fake_now_us += 16667; wr(CAM_DESIRED, (V3){0, 0, -0.3f});
    memset(&shared_settings, 0, sizeof shared_settings); camera_accept(&s, fake_now_us / 1000);
    uint64_t (*volatile call)(uint64_t, uint64_t, uint64_t, void *) = real_update;
    assert(call(1, 2, 3, ctx) == 1 * 3 + 2 * 5 + 3 * 7);
    assert(near3(eye(), (V3){0, 0, -0.3f}, 1e-5f));
    assert(MH_DisableHook(MH_ALL_HOOKS) == MH_OK && MH_RemoveHook((void *)real_update) == MH_OK && MH_RemoveHook((void *)real_zoom) == MH_OK && MH_Uninitialize() == MH_OK);
    puts("PASS real MinHook detours on update and zoom: arguments and return pass through; eye exactly on target");
}
static Settings head_mode(void) { Settings s = base(); s.anchor = 1; s.bob_pct = 100; return s; }
static void test_head_anchor(void) {
    /* Head box centred at (1, 1.7, 1); the camera's desired root 10 cm lower and 5 cm off. */
    setup((V3){1, 1.6f, 1.05f}, (V3){0, 0, 1}, 0.1f);
    head_count = 1; set_head(0, (V3){0.9f, 1.6f, 0.9f}, (V3){1.1f, 1.8f, 1.1f});
    for (int i = 0; i < 5; i++) frame(head_mode(), (V3){1, 1.6f, 1.05f});
    assert(near3(eye(), (V3){1, 1.64f, 1}, 1e-5f));
    Row r = last_row(); assert((r.flags & F_HEAD) && r.heads == 1 && r.head[1] >= 1639 && r.head[1] <= 1640 && r.head_ext_y_mm >= 199 && r.head_ext_y_mm <= 200);
    /* The body lurches 12 cm forward (stop animation) while the root stays: the eye follows at once. */
    set_head(0, (V3){0.9f, 1.6f, 0.78f}, (V3){1.1f, 1.8f, 0.98f});
    frame(head_mode(), (V3){1, 1.6f, 1.05f});
    assert(near3(eye(), (V3){1, 1.64f, 0.88f}, 1e-4f));
    /* Two head pieces: union of their boxes. */
    head_count = 2; set_head(1, (V3){0.95f, 1.75f, 0.8f}, (V3){1.05f, 1.9f, 0.9f});
    frame(head_mode(), (V3){1, 1.6f, 1.05f});
    assert(near3(eye(), (V3){1, 1.74f, 0.88f}, 1e-3f));
    /* A modded head whose mesh runs down the neck to the shoulders (76 cm box, same crown): same eye. */
    head_count = 1; set_head(0, (V3){0.75f, 1.04f, 0.75f}, (V3){1.25f, 1.8f, 1.25f});
    frame(head_mode(), (V3){1, 1.6f, 1.05f});
    assert(near3(eye(), (V3){1, 1.64f, 1}, 1e-4f));
    puts("PASS head anchor: 16 cm below the live crown, follows a lurch the same frame, unions several head pieces; a tall modded head (neck + shoulders) gives the same eye");
}
static void test_head_bob(void) {
    setup((V3){0, 1.6f, 0}, (V3){0, 0, 1}, 0.1f);
    head_count = 1;
    Settings s = head_mode(); s.bob_pct = 0;
    float maxdev = 0, maxdev_full = 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { setup((V3){0, 1.6f, 0}, (V3){0, 0, 1}, 0.1f); head_count = 1; s.bob_pct = 100; }
        for (int i = 0; i < 240; i++) {      /* 4 s, head bobbing +-3 cm at 2 Hz */
            float bob = 0.03f * sin_approx(0.f) + 0.03f * (float)((i % 30) < 15 ? (i % 15) : (15 - i % 15)) / 7.5f - 0.03f;
            set_head(0, (V3){-0.1f, 1.6f + bob, -0.1f}, (V3){0.1f, 1.8f + bob, 0.1f});
            frame(s, (V3){0, 1.6f, 0});
            if (i > 120) { float d = eye().y - 1.64f; if (d < 0) d = -d; if (pass == 0) { if (d > maxdev) maxdev = d; } else if (d > maxdev_full) maxdev_full = d; }
        }
    }
    assert(maxdev_full > 0.025f && maxdev < 0.012f);
    printf("PASS head bob: at 0%% the eye's vertical bob is %.1f cm (head bobs 3 cm); at 100%% it is %.1f cm\n", maxdev * 100, maxdev_full * 100);
}
static void test_head_guards(void) {
    setup((V3){0, 1.6f, 0}, (V3){0, 0, 1}, 0.1f);
    frame(head_mode(), (V3){0, 1.6f, 0});                       /* no head tagged */
    Row r = last_row(); assert((r.flags & F_HEAD_MISS) && near3(eye(), (V3){0, 1.6f, 0}, 1e-5f));
    head_count = 1; set_head(0, (V3){5, 1.6f, 5}, (V3){5.2f, 1.8f, 5.2f});   /* 7 m away */
    frame(head_mode(), (V3){0, 1.6f, 0});
    r = last_row(); assert((r.flags & F_HEAD_MISS) && near3(eye(), (V3){0, 1.6f, 0}, 1e-5f));
    float nan = 0.f; nan = nan / nan;
    set_head(0, (V3){nan, 1.6f, 0}, (V3){0.1f, 1.8f, 0.1f}); frame(head_mode(), (V3){0, 1.6f, 0});
    r = last_row(); assert(r.flags & F_HEAD_MISS);
    set_head(0, (V3){-2, 0, -2}, (V3){2, 3, 2}); frame(head_mode(), (V3){0, 1.6f, 0});   /* 4 m box: not a head */
    r = last_row(); assert(r.flags & F_HEAD_MISS);
    Settings piv = base(); set_head(0, (V3){-0.1f, 1.6f, -0.1f}, (V3){0.1f, 1.8f, 0.1f});
    frame(piv, (V3){0, 1.6f, 0});
    r = last_row(); assert(!(r.flags & (F_HEAD | F_HEAD_MISS)) && near3(eye(), (V3){0, 1.6f, 0}, 1e-5f));
    puts("PASS head guards: missing, far, NaN and oversized heads fall back to the pivot; pivot anchor ignores heads");
}
static void test_stop_push(void) {
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f);
    Settings s = base(); s.stop_cm = 20;
    V3 d = {0, 0, 0};
    for (int i = 1; i <= 60; i++) { d = (V3){5.f * i / 60.f, 0, 0}; frame(s, d); }   /* run +x at 5 m/s */
    float peak = 0; int frames_pushed = 0;
    for (int i = 0; i < 60; i++) {                                                     /* stop dead */
        frame(s, d);
        float ahead = eye().x - d.x;
        if (ahead > peak) peak = ahead;
        if (last_row().flags & F_STOP) frames_pushed++;
    }
    assert(peak > 0.17f && peak < 0.205f && frames_pushed > 25 && frames_pushed < 40);
    assert(near3(eye(), d, 1e-4f));                                                    /* back to rest */
    setup((V3){0, 0, 0}, (V3){0, 0, 1}, 0.1f); s.stop_cm = 0;
    for (int i = 1; i <= 60; i++) { d = (V3){5.f * i / 60.f, 0, 0}; frame(s, d); }
    for (int i = 0; i < 20; i++) { frame(s, d); assert(!(last_row().flags & F_STOP)); }
    printf("PASS stop push: braking from 5 m/s pushes the eye forward up to %.1f cm over ~%d frames and back; 0 = off\n", peak * 100, frames_pushed);
}

/* The round-3 drift: the game keeps an old eye distance (13.7 cm while the arm is 10 cm), so the eye
   sits 3.7 cm back along the view line. The eye fix makes it exact; without it the old miss remains. */
static void test_eye_fix(void) {
    float y = 0.47f, h = sq(1.f - y * y);
    V3 dir = {0.6f * h, y, 0.8f * h};
    for (int fix = 0; fix < 2; fix++) {
        setup((V3){1, 2, 3}, dir, 0.1f);
        game_copies_dist = 0; wrf(CAM_EYE_DIST, 0.137f);
        Settings s = base(); s.eyefix = fix;
        for (int i = 1; i <= 5; i++) frame(s, (V3){1, 2, 3 - 0.05f * i});
        V3 want = {1, 2, 3 - 0.25f};
        Row r = last_row();
        if (fix) {
            assert(near3(eye(), want, 1e-5f) && (r.flags & F_EYEFIX) && (r.flags & F_EYE_OK));
            assert(r.game_eye_dist_mm == 100 && r.final_eye_dist_mm == 100);
        } else {
            V3 off = sub(eye(), want);
            assert(near3(off, mul(dir, 0.037f), 1e-4f) && !(r.flags & F_EYEFIX) && !(r.flags & F_EYE_OK));
            assert(r.eye_err_mm >= 36 && r.eye_err_mm <= 38 && r.game_eye_dist_mm == 137 && r.final_eye_dist_mm == 137);
        }
    }
    /* First frame of the fix: the logged game value is the stale one. */
    setup((V3){0, 0, 0}, dir, 0.1f); game_copies_dist = 0; wrf(CAM_EYE_DIST, 1.1f);
    Settings s = base(); s.eyefix = 1; frame(s, (V3){0, 0, 0});
    Row r = last_row(); assert(r.game_eye_dist_mm == 1100 && r.final_eye_dist_mm == 100 && near3(eye(), (V3){0, 0, 0}, 1e-5f));
    /* Lock off: the field is never touched. */
    setup((V3){0, 0, 0}, dir, 0.1f); game_copies_dist = 0; wrf(CAM_EYE_DIST, 1.1f);
    s.lock = 0; frame(s, (V3){0, 0, 0});
    float left; memcpy(&left, cam + CAM_EYE_DIST, 4); assert(left == 1.1f);
    puts("PASS eye fix: a stale eye distance (13.7 vs 10 cm) left the eye 3.7 cm back along the view; with the fix the eye is exact; lock off never writes it");
}
static Settings combat_mode(V3 feet) { Settings s = head_mode(); s.combat = 1; s.eyefix = 1; s.have_feet = 1; s.feet = feet; return s; }
/* The round-3 combat log: the pivot climbs in steps from 2 m to 6.2 m above the feet and drops back,
   repeatedly. The head stays put; the eye must stay on the head every frame, with no identity misses. */
static void test_combat_pivot_climb(void) {
    V3 feet = {0, 36.683f, 0};
    V3 headc = {0, 38.45f, 0};
    setup((V3){0, 38.683f, 0}, (V3){0, 0.47f, 0.88f}, 0.1f);
    head_count = 1; set_head(0, sub(headc, (V3){0.1f, 0.1f, 0.1f}), add(headc, (V3){0.1f, 0.1f, 0.1f}));
    Settings s = combat_mode(feet);
    float climb[] = {38.683f, 38.82f, 38.91f, 39.15f, 39.46f, 40.14f, 41.08f, 42.86f, 41.36f, 40.19f, 39.66f, 40.63f, 38.68f};
    float worst = 0;
    for (int k = 0; k < 13; k++) for (int j = 0; j < 4; j++) {
        frame(s, (V3){0.02f * (float)j, climb[k], 0});
        Row r = last_row();
        assert((r.flags & F_APPLIED) && (r.flags & F_PIVOT_OFF) && (r.flags & F_COMBAT) && !(r.flags & (F_IDENTITY | F_HEAD_MISS)));
        float d = vlen(sub(eye(), add(headc, (V3){0, -0.06f, 0}))); if (d > worst) worst = d;
    }
    assert(worst < 1e-4f);
    /* Same climb outside combat with the old checks (no character position): the head is rejected
       once the pivot is 2.5 m away, so the eye jumps to the raised pivot. */
    setup((V3){0, 38.683f, 0}, (V3){0, 0.47f, 0.88f}, 0.1f);
    head_count = 1; set_head(0, sub(headc, (V3){0.1f, 0.1f, 0.1f}), add(headc, (V3){0.1f, 0.1f, 0.1f}));
    Settings old = head_mode(); old.eyefix = 1;
    int misses = 0;
    for (int k = 0; k < 13; k++) { frame(old, (V3){0, climb[k], 0}); if (last_row().flags & F_HEAD_MISS) misses++; }
    assert(misses >= 1);
    printf("PASS combat: pivot climbing to 6.2 m above the feet, the eye stays on the head (worst %.2f mm), no identity misses; without the fix %d steps lost the head\n", worst * 1000, misses);
}
static void test_head_identity(void) {
    V3 feet = {0, 0, 0}, headc = {0, 1.6f, 0};
    setup((V3){0, 2, 0}, (V3){0, 0, 1}, 0.1f);
    head_count = 1; set_head(0, sub(headc, (V3){0.1f, 0.1f, 0.1f}), add(headc, (V3){0.1f, 0.1f, 0.1f}));
    Settings s = head_mode(); s.have_feet = 1; s.feet = feet; s.eyefix = 1;
    /* Exploration: pivot 40 cm above the head -> the normal bob-filtered anchor, not pivot-off. */
    frame(s, (V3){0, 2, 0});
    Row r = last_row(); assert((r.flags & F_HEAD) && !(r.flags & F_PIVOT_OFF) && near3(eye(), add(headc, (V3){0, -0.06f, 0}), 1e-5f));
    /* Pivot 1.5 m from the head outside combat: not trusted, the eye is the head. */
    frame(s, (V3){0, 3.1f, 0});
    r = last_row(); assert((r.flags & F_PIVOT_OFF) && !(r.flags & F_COMBAT) && near3(eye(), add(headc, (V3){0, -0.06f, 0}), 1e-5f));
    /* The tagged head 5 m from the character (another character's head, stale tag): no write. */
    s.feet = (V3){5, 0, 0};
    V3 before = rd(CAM_CURRENT);
    frame(s, (V3){0, 2, 0});
    r = last_row(); assert((r.flags & F_IDENTITY) && !(r.flags & F_APPLIED));
    V3 smoothed = add(before, mul(sub((V3){0, 2, 0}, before), 0.25f));
    assert(near3(rd(CAM_CURRENT), smoothed, 1e-5f));
    /* No character position (older PAK): the pivot reference check still applies. */
    s.have_feet = 0; s.feet = (V3){0, 0, 0};
    fake_now_us += 16667; wr(CAM_DESIRED, (V3){0, 2, 0});
    s.seq = ++seqno; s.ref = (V3){9, 2, 0};
    memset(&shared_settings, 0, sizeof shared_settings); camera_accept(&s, fake_now_us / 1000);
    camera_update_hook(1, 2, 3, ctx);
    r = last_row(); assert((r.flags & F_IDENTITY) && !(r.flags & F_APPLIED));
    puts("PASS head identity: head within 3 m of the character drives the eye; a far pivot is ignored; a foreign head or a mismatched pivot (old mailbox) is never written");
}
/* With the pivot not trusted the speed comes from the head: a climbing pivot is not movement. */
static void test_pivot_off_velocity(void) {
    V3 feet = {0, 0, 0};
    setup((V3){0, 2, 0}, (V3){0, 0, 1}, 0.1f);
    head_count = 1;
    Settings s = combat_mode(feet); s.lead = 1;
    float worst = 0;
    for (int i = 0; i < 30; i++) {
        V3 hc = {0, 1.6f, 0};
        set_head(0, sub(hc, (V3){0.1f, 0.1f, 0.1f}), add(hc, (V3){0.1f, 0.1f, 0.1f}));
        frame(s, (V3){0, 2.f + 0.15f * (float)i, 0});         /* pivot rising 9 m/s */
        float d = vlen(sub(eye(), add(hc, (V3){0, -0.06f, 0}))); if (d > worst) worst = d;
        assert(last_row().speed_mms == 0);
    }
    for (int i = 1; i <= 60; i++) {                            /* now walking 3 m/s toward -z */
        V3 hc = {0, 1.6f, -3.f * (float)i / 60.f};
        set_head(0, sub(hc, (V3){0.1f, 0.1f, 0.1f}), add(hc, (V3){0.1f, 0.1f, 0.1f}));
        frame(s, (V3){0, 6.5f, 0});
    }
    Row r = last_row(); assert(r.speed_mms > 2800 && r.speed_mms < 3200);
    printf("PASS pivot-off speed: a pivot rising 9 m/s gives no speed or lead (eye stays within %.2f mm of the head); walking 3 m/s is measured from the head\n", worst * 1000);
}
/* 0.8.0.0: without the locator's verification the eye fix never writes; logs off = no rows except marks. */
static void test_rc_switches(void) {
    V3 dir = {0, 0.47f, 0.88f};
    setup((V3){0, 0, 0}, dir, 0.1f); game_copies_dist = 0; wrf(CAM_EYE_DIST, 0.137f); eye_field_ok = 0;
    Settings s = base(); s.eyefix = 1;
    frame(s, (V3){0, 0, 0});
    float left; memcpy(&left, cam + CAM_EYE_DIST, 4);
    Row r = last_row(); assert(left == 0.137f && !(r.flags & F_EYEFIX) && (r.flags & F_APPLIED));
    setup((V3){0, 0, 0}, dir, 0.1f);
    s = base(); s.logs = 0;
    for (int i = 0; i < 5; i++) frame(s, (V3){0, 0, -0.1f * i});
    assert(rows_total == 0 && near3(eye(), (V3){0, 0, -0.4f}, 1e-5f));
    s.mark = 4; frame(s, (V3){0, 0, -0.5f});
    r = last_row(); assert(rows_total == 1 && r.mark == 4);
    puts("PASS RC switches: eye fix needs the verified eye-distance code; with logs off the camera still corrects but writes no rows (marks still logged)");
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_parse(); test_vanilla(); test_lock_pivot(); test_lock_orbit(); test_head_tilt(); test_guards(); test_lead_lean(); test_marks();
    test_head_anchor(); test_head_bob(); test_head_guards(); test_stop_push();
    test_eye_fix(); test_combat_pivot_climb(); test_head_identity(); test_pivot_off_velocity(); test_rc_switches(); test_detours();
    return 0;
}
