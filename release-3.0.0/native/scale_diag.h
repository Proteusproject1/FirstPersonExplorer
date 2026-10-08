#pragma once
#include <windows.h>
#include <stdint.h>
/* Observation only. No API for writing scale or requesting a snap exists here. */
void scale_diag_run(HMODULE module, unsigned char *hook_point, const char *locate_failure);
void scale_diag_capture(float *record, const unsigned char *engine_frame);
extern void *scale_diag_trampoline;
void scale_diag_hook(void);
