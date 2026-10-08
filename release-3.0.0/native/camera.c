#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <xmmintrin.h>
#include "camera.h"

/* Camera object (same layout as Script Extender's ecl::GameCameraBehavior):
   0x24 TargetDestination (desired root, this frame), 0x3C TargetCurrentLow, 0x48 TargetCurrent
   (smoothed root), 0x54 Distance (arm length), 0x70 Direction (unit vector root -> eye),
   0x150 final eye (x,y) and 0x158 (z), written by the update as TargetCurrent + Direction * Distance
   after the zoom step. The update context holds the active camera at 0x30.
   0.7.11.0: the final eye actually uses 0x15C as its distance; the update copies Distance (0x54) there
   only under some conditions (bg3_dx11 update+0xB23, read at +0xB29), otherwise an older value stays.
   That stale distance pushed the eye 4 cm to 1 m back along the view line. The eye fix writes 0x15C. */
#define CAM_DESIRED 0x24
#define CAM_CURRENT_LOW 0x3C
#define CAM_CURRENT 0x48
#define CAM_DISTANCE 0x54
#define CAM_DIRECTION 0x70
#define CAM_EYE_XY 0x150
#define CAM_EYE_Z 0x158
#define CAM_EYE_DIST 0x15C
#define CONTEXT_CAMERA 0x30
#define OBJ_WORLD_BOUND 0x40   /* MoveableObject.WorldBound (Min xyz, Max xyz), per Script Extender */
#define HEARTBEAT_MS 400u
#define IDENTITY_M 1.5f
#define FEET_M 3.0f            /* head mode: the tagged head must be this close to the character */
#define PIVOT_TRUST_M 1.2f     /* a pivot further than this from the head is not used for the anchor */
#define CROWN_DROP_M 0.16f     /* head anchor = 16 cm below the top of the head (= centre of a vanilla 32 cm head box) */
#define LOG_ROWS 4096u
#define LOG_FILE_ROWS 50000u   /* then the log rolls over to FirstPersonExplorerCamera.prev.log */

CameraUpdateFn camera_update_original;
CameraUpdateFn camera_zoom_original;

typedef struct { float x, y, z; } V3;
typedef struct {
    uint32_t seq, mark;
    int on, lock, lead, lean_cm, fwd_cm, up_cm, pivot, neck_cm, anchor, bob_pct, stop_cm, combat, eyefix, have_feet, logs;
    V3 ref, feet;
    uint64_t received_ms;
} Settings;
/* One logged frame, integers only (no C runtime formatting). Positions in millimetres. */
typedef struct {
    uint64_t t_us; uint32_t dt_us, seq, mark, flags;
    int32_t lean, fwd, up, neck;
    int32_t desired[3], eye[3], target[3], head[3];
    int32_t heads, head_ext_y_mm;
    int32_t dist_mm, pitch_ddeg, speed_mms, eye_err_mm, behind_mm, ref_err_mm;
    int32_t game_eye_dist_mm, final_eye_dist_mm;
} Row;
#define F_ON 1u
#define F_LOCK 2u
#define F_LEAD 4u
#define F_APPLIED 8u
#define F_STALE 16u
#define F_IDENTITY 32u
#define F_INSANE 64u
#define F_PIVOT 128u
#define F_EYE_OK 256u
#define F_HEAD 512u
#define F_HEAD_MISS 1024u
#define F_STOP 2048u
#define F_EYEFIX 4096u
#define F_PIVOT_OFF 8192u
#define F_COMBAT 16384u

static SRWLOCK settings_lock = SRWLOCK_INIT;
static Settings shared_settings;
static SRWLOCK log_lock = SRWLOCK_INIT;
static Row rows[LOG_ROWS];
static unsigned row_head, row_tail;
static volatile LONG rows_total, rows_dropped, frames_seen, frames_applied, identity_misses, eye_misses;
static volatile LONG camera_ready_flag, eye_field_ok;   /* hooks installed; 0x15C verified by the locator */
int fpe_camera_ready(void) { return camera_ready_flag != 0; }
static LARGE_INTEGER qpc_freq;

/* Per-frame state, touched only by the thread running the camera update. */
typedef struct {
    unsigned char *camera;     /* camera of the update in progress, 0 outside it */
    DWORD thread;
    Settings s;
    uint32_t flags;
    V3 target;                 /* intended final eye this frame */
    int have_target;
    float ref_err, pitch, game_eye_dist;
    /* follow state across frames */
    unsigned char *last_camera;
    int have_prev;
    V3 prev_desired, vel, look;
    uint64_t prev_us, t;
    uint32_t dt_us, last_mark;
    /* head anchor: slow-moving part of (head - desired), so bob can be softened separately */
    V3 slow, head, move_dir, push_dir; int have_slow, heads, pushing; float head_ext_y, prev_speed, push_t;
    int vel_from_head;         /* velocity source: the head when the pivot is not trusted */
} Frame;
static Frame fr;

static float sq(float v) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(v))); }
static float vlen(V3 v) { return sq(v.x * v.x + v.y * v.y + v.z * v.z); }
static float hlen(V3 v) { return sq(v.x * v.x + v.z * v.z); }
static V3 add(V3 a, V3 b) { return (V3){a.x + b.x, a.y + b.y, a.z + b.z}; }
static V3 sub(V3 a, V3 b) { return (V3){a.x - b.x, a.y - b.y, a.z - b.z}; }
static V3 mul(V3 a, float s) { return (V3){a.x * s, a.y * s, a.z * s}; }
static BOOL finite_f(float f) { return f == f && f > -100000.f && f < 100000.f; }
static BOOL finite_v(V3 v) { return finite_f(v.x) && finite_f(v.y) && finite_f(v.z); }
/* asin for |x| <= 1 without a C runtime: atan2(x, sqrt(1-x^2)) via a short series is enough here. */
static float asin_approx(float x) {
    if (x > 1.f) x = 1.f;
    if (x < -1.f) x = -1.f;
    /* Abramowitz-Stegun 4.4.45, error < 7e-5 rad */
    float a = x < 0 ? -x : x;
    float r = 1.5707288f - 0.2121144f * a + 0.0742610f * a * a - 0.0187293f * a * a * a;
    r = 1.5707964f - sq(1.f - a) * r;
    return x < 0 ? -r : r;
}
static float sin_approx(float x) { /* |x| <= 1.6 */ float x2 = x * x; return x * (1.f - x2 / 6.f * (1.f - x2 / 20.f * (1.f - x2 / 42.f))); }
/* sin on [0, pi] (folded into [0, pi/2] where the series is accurate). */
static float sin_pi(float u) { if (u > 1.5707964f) u = 3.1415927f - u; return sin_approx(u); }
static float cos_approx(float x) { float x2 = x * x; return 1.f - x2 / 2.f * (1.f - x2 / 12.f * (1.f - x2 / 30.f * (1.f - x2 / 56.f))); }
static BOOL copy_mem(void *out, const void *source, SIZE_T size) {
    SIZE_T count = 0;
    return source && ReadProcessMemory(GetCurrentProcess(), source, out, size, &count) && count == size;
}
static V3 get3(unsigned char *c, unsigned o) { V3 v = {0, 0, 0}; copy_mem(&v, c + o, sizeof v); return v; }
static float getf(unsigned char *c, unsigned o) { float f = 0; copy_mem(&f, c + o, sizeof f); return f; }
static void put3(unsigned char *c, unsigned o, V3 v) { *(V3 *)(c + o) = v; }
static int32_t mm(float v) { float s = v * 1000.f; if (s > 2.0e9f) s = 2.0e9f; if (s < -2.0e9f) s = -2.0e9f; return (int32_t)s; }
static uint64_t now_us(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    if (!qpc_freq.QuadPart) QueryPerformanceFrequency(&qpc_freq);
    return (uint64_t)((t.QuadPart / qpc_freq.QuadPart) * 1000000ull + (t.QuadPart % qpc_freq.QuadPart) * 1000000ull / qpc_freq.QuadPart);
}
#ifdef FPE_TEST
static uint64_t fake_now_us;   /* tests drive the clock */
static uint64_t clock_us(void) { return fake_now_us ? fake_now_us : now_us(); }
#else
static uint64_t clock_us(void) { return now_us(); }
#endif
static void push_row(const Row *r) {
    if (!TryAcquireSRWLockExclusive(&log_lock)) { InterlockedIncrement(&rows_dropped); return; }
    unsigned next = (row_head + 1) % LOG_ROWS;
    if (next == row_tail) { ReleaseSRWLockExclusive(&log_lock); InterlockedIncrement(&rows_dropped); return; }
    rows[row_head] = *r; row_head = next;
    ReleaseSRWLockExclusive(&log_lock);
    InterlockedIncrement(&rows_total);
}
static BOOL pop_row(Row *r) {
    AcquireSRWLockExclusive(&log_lock);
    BOOL found = row_head != row_tail;
    if (found) { *r = rows[row_tail]; row_tail = (row_tail + 1) % LOG_ROWS; }
    ReleaseSRWLockExclusive(&log_lock);
    return found;
}
static Settings snapshot(void) {
    static Settings cached;
    if (TryAcquireSRWLockShared(&settings_lock)) { cached = shared_settings; ReleaseSRWLockShared(&settings_lock); }
    return cached;
}

/* Live head position: union of the world bounds of the tagged head objects, measured from its top
   (the crown): x/z centre, crown - 16 cm. Modded head meshes often include the neck and shoulders,
   which moves a box centre down by ~20 cm but not the crown. Objects whose bounds are not finite or
   larger than 1.5 m on any axis are ignored. */
static int head_center(V3 *center, V3 *ext) {
    void *objs[8]; int n = fpe_head_objects(objs, 8), used = 0;
    V3 mn = {1e9f, 1e9f, 1e9f}, mx = {-1e9f, -1e9f, -1e9f};
    for (int i = 0; i < n; i++) {
        float b[6];
        if (!copy_mem(b, (unsigned char *)objs[i] + OBJ_WORLD_BOUND, sizeof b)) continue;
        V3 lo = {b[0], b[1], b[2]}, hi = {b[3], b[4], b[5]};
        if (!finite_v(lo) || !finite_v(hi)) continue;
        V3 size = sub(hi, lo);
        if (size.x < 0.f || size.y < 0.f || size.z < 0.f || size.x > 1.5f || size.y > 1.5f || size.z > 1.5f) continue;
        if (lo.x < mn.x) mn.x = lo.x;
        if (lo.y < mn.y) mn.y = lo.y;
        if (lo.z < mn.z) mn.z = lo.z;
        if (hi.x > mx.x) mx.x = hi.x;
        if (hi.y > mx.y) mx.y = hi.y;
        if (hi.z > mx.z) mx.z = hi.z;
        used++;
    }
    if (!used) return 0;
    *center = mul(add(mn, mx), 0.5f); center->y = mx.y - CROWN_DROP_M; *ext = sub(mx, mn);
    return used;
}
/* Intended final eye: desired root + eye height + neck-pivot head tilt + lead + lean.
   Head tilt: the eye sits fwd ahead and neck above a neck pivot; looking down by pitch p rotates it
   about that pivot: forward = fwd*cos p + neck*sin p, up = neck*cos p - fwd*sin p (relative to the
   pivot, which is neck below the straight-ahead eye). At p = 0 this is desired + up + fwd ahead. */
static V3 eye_target(V3 desired, V3 dir, const Settings *s, float dt, float *pitch_out) {
    V3 look = {-dir.x, 0, -dir.z};
    float h = hlen(look);
    if (h > 0.05f) { look = mul(look, 1.f / h); fr.look = look; } else look = fr.look;
    float p = asin_approx(dir.y);               /* camera above the root = looking down = positive */
    if (p < -1.0f) p = -1.0f;
    if (p > 1.4f) p = 1.4f;
    if (pitch_out) *pitch_out = p;
    float fwd = (float)s->fwd_cm / 100.f, neck = (float)s->neck_cm / 100.f;
    float c = cos_approx(p), sn = sin_approx(p);
    float ahead = fwd * c + neck * sn;
    float rise = neck * c - fwd * sn - neck;      /* 0 when looking straight ahead */
    V3 e = add(desired, (V3){0, (float)s->up_cm / 100.f + rise, 0});
    e = add(e, mul(look, ahead));
    if (s->lead && dt > 0.f && dt < 0.1f) e = add(e, mul(fr.vel, dt));
    float speed = hlen(fr.vel);
    if (s->lean_cm && speed > 0.05f) {
        float k = speed / 4.f; if (k > 1.25f) k = 1.25f;
        e = add(e, mul((V3){fr.vel.x / speed, 0, fr.vel.z / speed}, k * (float)s->lean_cm / 100.f));
    }
    return e;
}
/* Called right after the game's zoom step, inside the camera update: the final eye is computed
   from TargetCurrent immediately afterwards, so a value written here is what this frame shows. */
static void camera_apply(unsigned char *c) {
    unsigned char probe[0x160];
    if (!c || !copy_mem(probe, c, sizeof probe)) return;
    V3 desired = get3(c, CAM_DESIRED), dir = get3(c, CAM_DIRECTION);
    float dist = getf(c, CAM_DISTANCE);
    const Settings *s = &fr.s;
    if (!finite_v(desired) || !finite_v(dir) || !finite_f(dist) || dist < 0.f || dist > 100.f) { fr.flags |= F_INSANE; return; }
    /* Live head (head anchor): also the identity check when the character's position is known. */
    V3 head = {0, 0, 0}, ext = {0, 0, 0};
    int n = s->anchor ? head_center(&head, &ext) : 0;
    if (n) { fr.head = head; fr.heads = n; fr.head_ext_y = ext.y; }
    BOOL by_head = s->anchor && s->have_feet && n;
    /* The pivot is not trusted in combat (the combat camera moves it, up to several metres) or when it
       is far from the head; the eye then follows the head exactly. */
    BOOL pivot_off = n && (s->combat || vlen(sub(head, desired)) >= PIVOT_TRUST_M);
    if (s->combat) fr.flags |= F_COMBAT;
    /* Velocity of the followed point (the desired root, or the head when the pivot is not trusted),
       smoothed; a 2 m jump is a teleport; switching source starts over. */
    V3 src = pivot_off ? head : desired;
    float dt = (float)fr.dt_us / 1000000.f;
    if (fr.vel_from_head != pivot_off) { fr.have_prev = 0; fr.vel_from_head = pivot_off; }
    if (fr.have_prev && dt > 0.f && dt < 0.25f) {
        V3 step = sub(src, fr.prev_desired);
        if (vlen(step) > 2.f) fr.vel = (V3){0, 0, 0};
        else fr.vel = add(mul(fr.vel, 0.65f), mul(step, 0.35f / dt));
    } else fr.vel = (V3){0, 0, 0};
    fr.prev_desired = src; fr.have_prev = 1;
    BOOL fresh = s->on && s->received_ms + HEARTBEAT_MS >= fr.t / 1000ull;
    if (s->on && !fresh) fr.flags |= F_STALE;
    /* Identity: in head mode the tagged head must be within 3 m of the character; otherwise the camera's
       desired root must match the reference sent by the PAK. */
    fr.ref_err = by_head ? vlen(sub(head, s->feet)) : vlen(sub(desired, s->ref));
    if (s->on && fresh && fr.ref_err > (by_head ? FEET_M : IDENTITY_M)) { fr.flags |= F_IDENTITY; InterlockedIncrement(&identity_misses); }
    if (!fresh || !s->lock || (fr.flags & (F_IDENTITY | F_INSANE))) return;
    /* Anchor: the camera's desired root, or the live head. The slow part of (head - desired)
       carries size and lean; the fast part carries the lurch and bob. Horizontal motion is followed
       fully; vertical by the head-bob setting. Without the character's position a head more than
       2.5 m from the pivot is not trusted. With the pivot not trusted the eye is the head itself. */
    V3 base = desired;
    if (s->anchor) {
        if (n && (by_head || vlen(sub(head, desired)) < 2.5f)) {
            if (pivot_off) { base = head; fr.have_slow = 0; fr.flags |= F_PIVOT_OFF; }
            else {
                V3 off = sub(head, desired);
                float a = (dt > 0.f && dt < 0.25f) ? dt / (0.5f + dt) : 1.f;
                if (!fr.have_slow) { fr.slow = off; fr.have_slow = 1; }
                else fr.slow = add(fr.slow, mul(sub(off, fr.slow), a));
                V3 fast = sub(off, fr.slow);
                base = add(desired, fr.slow);
                base.x += fast.x; base.z += fast.z; base.y += fast.y * (float)s->bob_pct / 100.f;
            }
            fr.flags |= F_HEAD;
        } else { fr.flags |= F_HEAD_MISS; fr.have_slow = 0; }
    } else fr.have_slow = 0;
    /* Stop push (fallback): when you brake hard, push the eye forward and back over 0.6 s along
       your last direction, like the stop animation's lurch. */
    float spd = hlen(fr.vel);
    if (s->stop_cm && dt > 0.f && dt < 0.25f) {
        float accel = (spd - fr.prev_speed) / dt;
        if (!fr.pushing && fr.prev_speed > 1.5f && accel < -6.f) { fr.pushing = 1; fr.push_t = 0.f; fr.push_dir = fr.move_dir; }
        if (fr.pushing) {
            fr.push_t += dt;
            if (fr.push_t >= 0.6f) fr.pushing = 0;
            else { base = add(base, mul(fr.push_dir, sin_pi(3.1415927f * fr.push_t / 0.6f) * (float)s->stop_cm / 100.f)); fr.flags |= F_STOP; }
        }
    } else fr.pushing = 0;
    if (spd > 0.5f) fr.move_dir = mul((V3){fr.vel.x, 0, fr.vel.z}, 1.f / spd);
    fr.prev_speed = spd;
    V3 target = eye_target(base, dir, s, dt, &fr.pitch);
    /* Eye at pivot: cancel the orbit arm so the eye is the target whatever the pitch. */
    V3 root = s->pivot ? sub(target, mul(dir, dist)) : target;
    if (!s->pivot) target = add(target, mul(dir, dist));
    put3(c, CAM_CURRENT_LOW, root);
    put3(c, CAM_CURRENT, root);
    /* Eye fix: the final eye uses 0x15C as its distance; make it the one the root was computed with. */
    fr.game_eye_dist = getf(c, CAM_EYE_DIST);
    if (s->eyefix && eye_field_ok) { *(float *)(c + CAM_EYE_DIST) = dist; fr.flags |= F_EYEFIX; }
    fr.target = target; fr.have_target = 1;
    fr.flags |= F_APPLIED;
    InterlockedIncrement(&frames_applied);
}
uint64_t camera_zoom_hook(uint64_t a, uint64_t b, uint64_t c, void *d) {
    uint64_t result = camera_zoom_original(a, b, c, d);
    if (fr.camera && fr.thread == GetCurrentThreadId()) camera_apply(fr.camera);
    return result;
}
static void camera_finish(unsigned char *c) {
    if (!fr.s.on && fr.s.mark == fr.last_mark) { fr.last_mark = fr.s.mark; return; }
    if (!fr.s.logs && fr.s.mark == fr.last_mark) return;     /* rows only while diagnostic logs are on (marks always) */
    Row r = {0};
    r.t_us = fr.t; r.dt_us = fr.dt_us; r.seq = fr.s.seq; r.flags = fr.flags;
    r.mark = fr.s.mark != fr.last_mark ? fr.s.mark : 0;
    r.lean = fr.s.lean_cm; r.fwd = fr.s.fwd_cm; r.up = fr.s.up_cm; r.neck = fr.s.neck_cm;
    V3 desired = get3(c, CAM_DESIRED), eye = {0, 0, 0};
    float exy[2] = {0, 0}; copy_mem(exy, c + CAM_EYE_XY, sizeof exy); eye.x = exy[0]; eye.y = exy[1]; eye.z = getf(c, CAM_EYE_Z);
    r.desired[0] = mm(desired.x); r.desired[1] = mm(desired.y); r.desired[2] = mm(desired.z);
    r.eye[0] = mm(eye.x); r.eye[1] = mm(eye.y); r.eye[2] = mm(eye.z);
    if (fr.have_target) {
        r.target[0] = mm(fr.target.x); r.target[1] = mm(fr.target.y); r.target[2] = mm(fr.target.z);
        float err = vlen(sub(eye, fr.target));
        r.eye_err_mm = mm(err);
        if (err < 0.01f) r.flags |= F_EYE_OK; else InterlockedIncrement(&eye_misses);
    }
    r.dist_mm = mm(getf(c, CAM_DISTANCE));
    r.pitch_ddeg = (int32_t)(fr.pitch * 572.958f);
    float speed = hlen(fr.vel);
    r.speed_mms = mm(speed);
    V3 back = sub(desired, eye); back.y = 0;      /* positive = eye behind the root along movement */
    r.behind_mm = speed > 0.05f ? mm((back.x * fr.vel.x + back.z * fr.vel.z) / speed) : 0;
    r.ref_err_mm = mm(fr.ref_err);
    r.game_eye_dist_mm = fr.game_eye_dist < 0.f ? -1 : mm(fr.game_eye_dist);
    r.final_eye_dist_mm = mm(getf(c, CAM_EYE_DIST));
    if (fr.heads) { r.head[0] = mm(fr.head.x); r.head[1] = mm(fr.head.y); r.head[2] = mm(fr.head.z); r.heads = fr.heads; r.head_ext_y_mm = mm(fr.head_ext_y); }
    push_row(&r);
    fr.last_mark = fr.s.mark;
}
uint64_t camera_update_hook(uint64_t a, uint64_t b, uint64_t c, void *context) {
    unsigned char *camera = 0;
    if (context) copy_mem(&camera, (unsigned char *)context + CONTEXT_CAMERA, sizeof camera);
    unsigned char probe[0x160];
    if (camera && !copy_mem(probe, camera, sizeof probe)) camera = 0;
    if (camera != fr.last_camera) { fr.last_camera = camera; fr.have_prev = 0; }
    uint64_t t = clock_us();
    fr.dt_us = fr.prev_us && t > fr.prev_us ? (uint32_t)(t - fr.prev_us) : 0;
    fr.prev_us = t; fr.t = t;
    fr.s = snapshot();
    fr.flags = (fr.s.on ? F_ON : 0) | (fr.s.lock ? F_LOCK : 0) | (fr.s.lead ? F_LEAD : 0) | (fr.s.pivot ? F_PIVOT : 0);
    fr.have_target = 0; fr.pitch = 0; fr.heads = 0; fr.game_eye_dist = -1.f;
    fr.thread = GetCurrentThreadId();
    fr.camera = camera;
    uint64_t result = camera_update_original(a, b, c, context);
    fr.camera = 0;
    if (camera) { InterlockedIncrement(&frames_seen); camera_finish(camera); }
    return result;
}

/* ---- Mailbox: "FPC5 seq on lock lead lean fwd up mark pivot neck anchor bob stop x_mm y_mm z_mm
        combat eyefix feet_ok feet_x_mm feet_y_mm feet_z_mm logs FPC5" ---- */
static BOOL parse_int(const char **p, const char *end, int32_t *out) {
    const char *s = *p; while (s < end && *s == ' ') s++;
    int neg = 0; if (s < end && *s == '-') { neg = 1; s++; }
    if (s >= end || *s < '0' || *s > '9') return FALSE;
    int64_t v = 0;
    while (s < end && *s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); if (v > 2000000000) return FALSE; s++; }
    *out = (int32_t)(neg ? -v : v); *p = s; return TRUE;
}
static BOOL parse_tag(const char **p, const char *end) {
    const char *s = *p; while (s < end && *s == ' ') s++;
    if (end - s < 4 || s[0] != 'F' || s[1] != 'P' || s[2] != 'C' || s[3] != '5') return FALSE;
    *p = s + 4; return TRUE;
}
BOOL camera_parse(const char *data, unsigned length, Settings *out) {
    const char *p = data, *end = data + length;
    int32_t v[23];
    if (!parse_tag(&p, end)) return FALSE;
    for (int i = 0; i < 23; i++) if (!parse_int(&p, end, &v[i])) return FALSE;
    if (!parse_tag(&p, end)) return FALSE;
    if (v[0] < 0 || (v[1] != 0 && v[1] != 1) || (v[2] != 0 && v[2] != 1) || (v[3] != 0 && v[3] != 1) || (v[8] != 0 && v[8] != 1)) return FALSE;
    if (v[4] < 0 || v[4] > 60 || v[5] < -30 || v[5] > 60 || v[6] < -60 || v[6] > 60 || v[7] < 0 || v[9] < 0 || v[9] > 40) return FALSE;
    if ((v[10] != 0 && v[10] != 1) || v[11] < 0 || v[11] > 100 || v[12] < 0 || v[12] > 60) return FALSE;
    if ((v[16] != 0 && v[16] != 1) || (v[17] != 0 && v[17] != 1) || (v[18] != 0 && v[18] != 1) || (v[22] != 0 && v[22] != 1)) return FALSE;
    out->seq = (uint32_t)v[0]; out->on = v[1]; out->lock = v[2]; out->lead = v[3];
    out->lean_cm = v[4]; out->fwd_cm = v[5]; out->up_cm = v[6]; out->mark = (uint32_t)v[7];
    out->pivot = v[8]; out->neck_cm = v[9]; out->anchor = v[10]; out->bob_pct = v[11]; out->stop_cm = v[12];
    out->ref = (V3){(float)v[13] / 1000.f, (float)v[14] / 1000.f, (float)v[15] / 1000.f};
    out->combat = v[16]; out->eyefix = v[17]; out->have_feet = v[18];
    out->feet = (V3){(float)v[19] / 1000.f, (float)v[20] / 1000.f, (float)v[21] / 1000.f};
    out->logs = v[22];
    return TRUE;
}
/* Accept a parsed record; a changed sequence number is the heartbeat. */
void camera_accept(const Settings *in, uint64_t now_ms) {
    AcquireSRWLockExclusive(&settings_lock);
    if (in->seq != shared_settings.seq || !shared_settings.received_ms) {
        shared_settings = *in;
        shared_settings.received_ms = now_ms;
    }
    ReleaseSRWLockExclusive(&settings_lock);
}

#ifndef FPE_TEST
static void text(HANDLE f, const char *s) { DWORD n = 0, w; while (s[n]) n++; WriteFile(f, s, n, &w, NULL); }
static unsigned put_int(char *out, int64_t v) {
    char rev[24]; unsigned n = 0, k = 0; int neg = v < 0; uint64_t u = neg ? (uint64_t)(-v) : (uint64_t)v;
    do { rev[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) out[k++] = '-';
    while (n) out[k++] = rev[--n];
    return k;
}
static void line_int(HANDLE f, const char *label, int64_t v) { char b[32]; unsigned n = put_int(b, v); b[n++] = '\n'; text(f, label); DWORD w; WriteFile(f, b, n, &w, NULL); }
static BOOL path_beside(HMODULE module, const wchar_t *name, wchar_t *path) {
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return FALSE;
    while (n && path[n - 1] != L'\\') n--;
    unsigned i = 0; while (name[i]) { if (n + i >= MAX_PATH - 1) return FALSE; path[n + i] = name[i]; i++; }
    path[n + i] = 0;
    return TRUE;
}
static HANDLE create_log(const wchar_t *path) { return CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); }
static void columns(HANDLE f) {
    text(f, "flags: 1=on 2=lock 4=lead 8=applied 16=stale 32=identity-mismatch 64=insane 128=pivot 256=eye-on-target 512=head-anchor 1024=head-missing 2048=stop-push 4096=eye-fix 8192=pivot-not-trusted 16384=combat; positions mm; pitch 0.1 deg (down +); game_eye_dist = 0x15C before the fix (-1 = not applied), final_eye_dist = 0x15C after the update\n");
    text(f, "ms\tdt_us\tseq\tmark\tflags\tlean_cm\tfwd_cm\tup_cm\tneck_cm\tdes_x\tdes_y\tdes_z\teye_x\teye_y\teye_z\ttgt_x\ttgt_y\ttgt_z\tdist\tpitch\tspeed\teye_err\tbehind\tref_err\thead_x\thead_y\thead_z\theads\thead_ext_y\tgame_eye_dist\tfinal_eye_dist\n");
}
static void write_row(HANDLE f, const Row *r) {
    char b[512]; unsigned n = 0;   /* 31 columns x up to 12 characters */
    int64_t cols[] = { (int64_t)(r->t_us / 1000), r->dt_us, r->seq, r->mark, r->flags, r->lean, r->fwd, r->up, r->neck,
        r->desired[0], r->desired[1], r->desired[2], r->eye[0], r->eye[1], r->eye[2], r->target[0], r->target[1], r->target[2],
        r->dist_mm, r->pitch_ddeg, r->speed_mms, r->eye_err_mm, r->behind_mm, r->ref_err_mm,
        r->head[0], r->head[1], r->head[2], r->heads, r->head_ext_y_mm, r->game_eye_dist_mm, r->final_eye_dist_mm };
    for (unsigned i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) { if (i) b[n++] = '\t'; n += put_int(b + n, cols[i]); }
    b[n++] = '\n'; DWORD w; WriteFile(f, b, n, &w, NULL);
}
void camera_worker_run(HMODULE module, unsigned char *game_base, unsigned char *update_fn, unsigned char *zoom_fn, const char *locate_failure, int hook_status, int eye_fix_ok) {
    static wchar_t log_path[MAX_PATH], prev_path[MAX_PATH];
    if (!path_beside(module, L"FirstPersonExplorerCamera.log", log_path) || !path_beside(module, L"FirstPersonExplorerCamera.prev.log", prev_path)) return;
    DeleteFileW(prev_path);
    HANDLE f = create_log(log_path);
    if (f == INVALID_HANDLE_VALUE) return;
    text(f, "FPE Native 3.0.0.0 camera log (rows only while the PAK's diagnostic logs are on)\n");
    if (!update_fn || !zoom_fn) { text(f, "DISABLED: "); text(f, locate_failure ? locate_failure : "camera code not located"); text(f, "\n"); CloseHandle(f); return; }
    line_int(f, "camera update rva=", (int64_t)(update_fn - game_base));
    line_int(f, "camera zoom rva=", (int64_t)(zoom_fn - game_base));
    if (hook_status != 0) { line_int(f, "DISABLED: MinHook status=", hook_status); CloseHandle(f); return; }
    InterlockedExchange(&eye_field_ok, eye_fix_ok ? 1 : 0);
    text(f, eye_fix_ok ? "eye fix: eye-distance code verified\n" : "eye fix: DISABLED (eye-distance code not recognised; the rest of the camera works)\n");
    text(f, "READY: camera hooks installed; corrections apply only with camera lock on and a fresh full-body heartbeat\n");
    columns(f);
    FlushFileBuffers(f);
    wchar_t local[MAX_PATH], path[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    static const wchar_t tail[] = L"\\Larian Studios\\Baldur's Gate 3\\Script Extender\\FirstPersonExplorer\\fullbody_camera_0778.txt";
    if (!n || n + sizeof(tail) / sizeof(tail[0]) >= MAX_PATH) { text(f, "DISABLED: mailbox path\n"); CloseHandle(f); return; }
    for (DWORD i = 0; i < n; i++) path[i] = local[i];
    for (unsigned i = 0; i < sizeof(tail) / sizeof(tail[0]); i++) path[n + i] = tail[i];
    InterlockedExchange(&camera_ready_flag, 1);
    ULONGLONG next_flush = GetTickCount64() + 250, next_stats = GetTickCount64() + 5000;
    unsigned file_rows = 0, part = 1;
    for (;;) {
        HANDLE m = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (m != INVALID_HANDLE_VALUE) {
            char data[400]; DWORD got = 0;
            if (ReadFile(m, data, sizeof(data), &got, NULL)) { Settings s = {0}; if (camera_parse(data, got, &s)) camera_accept(&s, now_us() / 1000ull); }
            CloseHandle(m);
        }
        ULONGLONG t = GetTickCount64();
        if (t >= next_flush) {
            Row r;
            while (pop_row(&r)) {
                if (file_rows >= LOG_FILE_ROWS) {
                    /* Roll over: the newest rows are always kept (this file + the .prev.log before it). */
                    CloseHandle(f);
                    MoveFileExW(log_path, prev_path, MOVEFILE_REPLACE_EXISTING);
                    f = create_log(log_path);   /* a failed reopen only loses log lines; the camera keeps working */
                    line_int(f, "FPE Native 3.0.0.0 camera log, continued; earlier rows in FirstPersonExplorerCamera.prev.log; part ", ++part);
                    columns(f);
                    file_rows = 0;
                }
                write_row(f, &r); file_rows++;
            }
            next_flush = t + 250;
        }
        if (t >= next_stats) {
            line_int(f, "# frames_seen=", frames_seen); line_int(f, "# frames_applied=", frames_applied);
            line_int(f, "# eye_misses=", eye_misses); line_int(f, "# identity_misses=", identity_misses);
            line_int(f, "# rows_dropped=", rows_dropped);
            FlushFileBuffers(f); next_stats = t + 5000;
        }
        Sleep(5);
    }
}
#endif
