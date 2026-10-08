#pragma once
#include <windows.h>
/* Finds the engine code this DLL hooks by masked signature instead of fixed addresses,
   so a game hotfix that only moves code keeps working. Every check that guarded the
   exact-build version remains: a unique match per function, exactly the expected
   render-object tables with the expected slot layout, and the exact instructions at
   the mid-function hook point. Anything else disables the feature it belongs to. */
typedef struct {
    void **tables[4];             /* ordered like the reference tables */
    unsigned char *scale_method;  /* shared table slot 4 */
    unsigned char *render_method; /* shared table slot 20 */
    unsigned char *scale_update;  /* start of the scale-transition update function */
    unsigned char *scale_hook;    /* mid-function hook point inside it */
    const char *failure;          /* render bridge: NULL when everything was found */
    const char *scale_failure;    /* instant scale: NULL when the hook point was found */
    unsigned int stamp;           /* PE build stamp, for the log only */
    unsigned int table_count;     /* candidate tables found (diagnostics) */
    unsigned char *camera_update; /* 0.7.8.0 experiment: camera update function entry */
    unsigned char *camera_zoom;   /* 0.7.9.0 experiment: camera zoom step, called just before the final eye */
    const char *camera_failure;   /* NULL when the camera update was found */
    int eye_fix_ok;               /* 0.8.0.0: the update's final eye reads its distance from 0x15C (eye fix allowed) */
} FpeLocation;
void fpe_locate(unsigned char *image, FpeLocation *out);
BOOL fpe_masked_equal(const unsigned char *candidate, const unsigned char *reference, SIZE_T size);
