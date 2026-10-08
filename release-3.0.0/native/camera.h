#pragma once
#include <windows.h>
#include <stdint.h>
/* 0.7.9.0 FULL BODY EXPERIMENT: in-frame camera corrections.
   The game's camera update computes the final eye as TargetCurrent + Direction * Distance right
   after its zoom step returns. Two entry hooks: the update (which camera, logging) and the zoom
   step (apply). After the zoom step, while the PAK reports full-body first person with camera lock
   on (fresh heartbeat, matching camera reference), TargetCurrent is set so the final eye lands on
   the target: the desired root plus eye height, a neck-pivot head-tilt model (eye forward, moving
   forward and down as you look down), speed lead and lean compensation; with "eye at pivot" the
   orbit arm is cancelled so looking up/down turns around the eye. Lock off = vanilla camera.
   0.7.10.0: with the head anchor the eye follows the live head mesh (its world bounds, read each
   frame from the objects the PAK tagged as head through the render bridge), so the camera moves
   with the body (stop lurch, lean, size); the head-bob setting softens only the vertical motion.
   0.7.11.0: eye fix (the final eye's own distance field 0x15C is set to the arm length used for the
   root, so the eye lands exactly on target); in combat, or when the pivot is far from the head, the
   eye follows the head exactly and identity is checked against the character's position (FPC4);
   the camera log rolls over instead of stopping at 60,000 rows.
   0.8.0.0: the eye fix runs only when the locator verified the 0x15C eye-distance code; the PAK can
   ask whether the camera is installed (render-bridge capability signal); per-frame rows are written
   only while the PAK's diagnostic logs are on (FPC5).
   The head anchor is 16 cm below the top of the head (crown), not the head box centre. */
int fpe_head_objects(void **out, int max);
typedef uint64_t (*CameraUpdateFn)(uint64_t, uint64_t, uint64_t, void *);
extern CameraUpdateFn camera_update_original;
extern CameraUpdateFn camera_zoom_original;
uint64_t camera_update_hook(uint64_t a, uint64_t b, uint64_t c, void *context);
uint64_t camera_zoom_hook(uint64_t a, uint64_t b, uint64_t c, void *d);
void camera_worker_run(HMODULE module, unsigned char *game_base, unsigned char *update_fn, unsigned char *zoom_fn, const char *locate_failure, int hook_status, int eye_fix_ok);
int fpe_camera_ready(void);
