#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* Stable 0.7.6.7 rendering bridge plus guarded instant scale. 0.7.7.0: the hooked
   engine code is located by masked signature (locate.c), not by exact build. */
#ifndef FPE_TEST
#include "scale_diag.h"
#include "locate.h"
#include "camera.h"
#include "vendor/minhook/include/MinHook.h"
#endif
typedef void (*ScaleFn)(void *, const float *);
typedef void (*RenderFn)(void *, const void *, const void *, const void *);
typedef void *(*DestroyFn)(void *, unsigned int);
static ScaleFn original_scale;
static RenderFn original_render;
static DestroyFn original_destroy[4];
static volatile LONG table_registrations[4];
static SRWLOCK registry_lock = SRWLOCK_INIT;
typedef struct { void *object; ULONGLONG until; } Entry;
static Entry registry[512];
static volatile LONG enabled, hidden_calls, drawn_calls, registrations;
static const float hide_signal[3] = {0.000011f,0.000013f,0.000017f};
static const float show_signal[3] = {0.000017f,0.000013f,0.000011f};
static const float probe_signal[3] = {0.000019f,0.000023f,0.000029f};
/* 0.7.10.0: "this hidden object is part of the head" (full body). Consumed, never forwarded. */
static const float head_signal[3] = {0.000023f,0.000029f,0.000031f};
/* 0.8.0.0: "is the full-body camera here?" Consumed only while the camera hooks are installed, so an
   older or camera-disabled DLL forwards it and the PAK (which restores the scale) falls back to classic. */
static const float camera_signal[3] = {0.000029f,0.000031f,0.000037f};
int fpe_camera_ready(void);
typedef struct { void *object; ULONGLONG until; } HeadEntry;
static HeadEntry heads[8];
static SRWLOCK head_lock = SRWLOCK_INIT;
static void tag_head(void *object, ULONGLONG now) {
    int slot=-1;
    AcquireSRWLockExclusive(&head_lock);
    for(int i=0;i<8;i++) { if(heads[i].object==object) { slot=i; break; } if(slot<0 && (!heads[i].object || heads[i].until<=now)) slot=i; }
    if(slot>=0) heads[slot]=(HeadEntry){object,now+500};
    ReleaseSRWLockExclusive(&head_lock);
}
static void untag_head(void *object) {
    AcquireSRWLockExclusive(&head_lock);
    for(int i=0;i<8;i++) if(heads[i].object==object) heads[i]=(HeadEntry){0,0};
    ReleaseSRWLockExclusive(&head_lock);
}
/* Live head objects (tagged within the last 500 ms and not destroyed since). */
int fpe_head_objects(void **out, int max) {
    int n=0; ULONGLONG now=GetTickCount64();
    AcquireSRWLockShared(&head_lock);
    for(int i=0;i<8 && n<max;i++) if(heads[i].object && heads[i].until>now) out[n++]=heads[i].object;
    ReleaseSRWLockShared(&head_lock);
    return n;
}
static BOOL same(const float *a, const float *b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2];
}
static BOOL renew(void *object, ULONGLONG now) {
    int available=-1;
    AcquireSRWLockExclusive(&registry_lock);
    for(int i=0;i<512;i++) {
        if(registry[i].object==object) { available=i; break; }
        if(available<0 && (!registry[i].object || registry[i].until<=now)) available=i;
    }
    if(available>=0) registry[available]=(Entry){object,now+500};
    ReleaseSRWLockExclusive(&registry_lock);
    return available>=0;
}
static void forget(void *object) {
    AcquireSRWLockExclusive(&registry_lock);
    for(int i=0;i<512;i++) if(registry[i].object==object) registry[i]=(Entry){0,0};
    ReleaseSRWLockExclusive(&registry_lock);
}
static BOOL should_hide(void *object, ULONGLONG now) {
    BOOL hide=FALSE;
    AcquireSRWLockShared(&registry_lock);
    for(int i=0;i<512;i++) if(registry[i].object==object) { hide=registry[i].until>now; break; }
    ReleaseSRWLockShared(&registry_lock);
    return hide;
}
static void scale_dispatch(int table_id,void *object,const float *scale) {
    if(enabled && same(scale,probe_signal)) return;
    if(enabled && same(scale,head_signal)) { tag_head(object,GetTickCount64()); return; }
    if(enabled && same(scale,camera_signal) && fpe_camera_ready()) return;
    if(enabled && same(scale,hide_signal) && renew(object,GetTickCount64())) {
        InterlockedIncrement(&registrations); InterlockedIncrement(&table_registrations[table_id]); return;
    }
    if(enabled && same(scale,show_signal)) { forget(object); return; }
    original_scale(object,scale);
}
static void render_hook(void *object,const void *a,const void *b,const void *c) {
    /* This executable's RenderObjectData begins with the drawn object pointer.
       The receiver can be a shared dispatch object, so never filter on receiver alone. */
    void *drawn_object = a ? *(void *const *)a : object;
    if(enabled && should_hide(drawn_object,GetTickCount64())) { InterlockedIncrement(&hidden_calls); return; }
    InterlockedIncrement(&drawn_calls);
    original_render(object,a,b,c);
}
static void *destroy_dispatch(int table_id,void *object,unsigned int flags) {
    forget(object);
    untag_head(object);
    return original_destroy[table_id](object,flags);
}
#define TABLE_HOOKS(N) \
static void scale_hook_##N(void *o,const float *s){scale_dispatch(N,o,s);} \
static void *destroy_hook_##N(void *o,unsigned int f){return destroy_dispatch(N,o,f);}
TABLE_HOOKS(0)
TABLE_HOOKS(1)
TABLE_HOOKS(2)
TABLE_HOOKS(3)
static ScaleFn scale_hooks[4]={scale_hook_0,scale_hook_1,scale_hook_2,scale_hook_3};
static DestroyFn destroy_hooks[4]={destroy_hook_0,destroy_hook_1,destroy_hook_2,destroy_hook_3};
static BOOL exchange_slot(void **slot,void *expected,void *replacement) {
    DWORD old,ignored;
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&old)) return FALSE;
    void *found=InterlockedCompareExchangePointer((void *volatile *)slot,replacement,expected);
    if(!VirtualProtect(slot,sizeof(void*),old,&ignored)) {
        if(found==expected) InterlockedCompareExchangePointer((void *volatile *)slot,expected,replacement);
        VirtualProtect(slot,sizeof(void*),old,&ignored);
        return FALSE;
    }
    return found==expected;
}
#ifndef FPE_TEST
/* The DLL is linked without a C runtime: it imports only KERNEL32 and exports nothing. */
static HANDLE open_log(HMODULE module) {
    static const wchar_t name[]=L"FirstPersonExplorerNative.log";
    wchar_t path[MAX_PATH];
    DWORD length=GetModuleFileNameW(module,path,MAX_PATH);
    if(length==0 || length>=MAX_PATH) return INVALID_HANDLE_VALUE;
    DWORD tail=length;
    while(tail>0 && path[tail-1]!=L'\\') tail--;
    if(tail==0 || tail+sizeof(name)/sizeof(name[0])>MAX_PATH) return INVALID_HANDLE_VALUE;
    for(DWORD i=0;i<sizeof(name)/sizeof(name[0]);i++) path[tail+i]=name[i];
    return CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
}
static void log_bytes(HANDLE log,const char *bytes,DWORD count) {
    DWORD written;
    if(log!=INVALID_HANDLE_VALUE) WriteFile(log,bytes,count,&written,NULL);
}
static void log_text(HANDLE log,const char *text) {
    DWORD count=0;
    while(text[count]) count++;
    log_bytes(log,text,count);
}
static void log_number(HANDLE log,unsigned int value) {
    char digits[10];
    DWORD first=sizeof(digits);
    do { digits[--first]=(char)('0'+value%10); value/=10; } while(value);
    log_bytes(log,digits+first,sizeof(digits)-first);
}
static void log_hex(HANDLE log,unsigned int value) {
    static const char digits[]="0123456789abcdef";
    char text[10]={'0','x'};
    for(int i=0;i<8;i++) text[2+i]=digits[(value>>((7-i)*4))&15];
    log_bytes(log,text,10);
}
static void log_close(HANDLE log) {
    if(log!=INVALID_HANDLE_VALUE) CloseHandle(log);
}
/* 0.7.8.0 experiment: hook the camera update at its entry (MinHook) and start its worker. */
static struct { HMODULE module; unsigned char *base, *update, *zoom; const char *failure; int status, eye_fix_ok; } camera_args;
static DWORD WINAPI camera_thread(void *unused) {
    (void)unused;
    camera_worker_run(camera_args.module,camera_args.base,camera_args.update,camera_args.zoom,camera_args.failure,camera_args.status,camera_args.eye_fix_ok);
    return 0;
}
static void camera_start(HMODULE module,unsigned char *base,unsigned char *update,unsigned char *zoom,const char *failure,int eye_fix_ok) {
    int status=0;
    if(update && zoom) {
        MH_STATUS s=MH_Initialize();
        if(s==MH_ERROR_ALREADY_INITIALIZED) s=MH_OK;
        /* Zoom hook first: it only acts inside an update the update hook has marked. */
        if(s==MH_OK) s=MH_CreateHook(zoom,(void*)camera_zoom_hook,(void**)&camera_zoom_original);
        if(s==MH_OK) s=MH_CreateHook(update,(void*)camera_update_hook,(void**)&camera_update_original);
        if(s==MH_OK) s=MH_EnableHook(zoom);
        if(s==MH_OK) s=MH_EnableHook(update);
        if(s!=MH_OK) { MH_DisableHook(update); MH_DisableHook(zoom); MH_RemoveHook(update); MH_RemoveHook(zoom); status=(int)s+1000; }
    }
    camera_args.module=module; camera_args.base=base; camera_args.update=update; camera_args.zoom=zoom; camera_args.failure=failure; camera_args.status=status; camera_args.eye_fix_ok=eye_fix_ok;
    HANDLE thread=CreateThread(NULL,0,camera_thread,NULL,0,NULL);
    if(thread) CloseHandle(thread);
}
static DWORD WINAPI initialize(void *module) {
    HANDLE log=open_log((HMODULE)module);
    unsigned char *base=(unsigned char*)GetModuleHandleW(NULL);
    FpeLocation where;
    ULONGLONG started=GetTickCount64();
    fpe_locate(base,&where);
    log_text(log,"FPE Native 3.0.0.0; game build stamp "); log_number(log,where.stamp);
    log_text(log,"; code search "); log_number(log,(unsigned int)(GetTickCount64()-started)); log_text(log," ms\n");
    if(where.failure) {
        log_text(log,"DISABLED: "); log_text(log,where.failure);
        log_text(log,". A game update changed the code this mod hooks; a First Person Explorer native update is needed.\n");
        log_close(log); return 0;
    }
    void **tables[4];
    for(int t=0;t<4;t++) {
        tables[t]=where.tables[t];
        original_destroy[t]=(DestroyFn)tables[t][1];
    }
    log_text(log,"Found: tables");
    for(int t=0;t<4;t++) { log_text(log," "); log_hex(log,(unsigned int)((unsigned char*)tables[t]-base)); }
    log_text(log,"; scale "); log_hex(log,(unsigned int)(where.scale_method-base));
    log_text(log,"; render "); log_hex(log,(unsigned int)(where.render_method-base)); log_text(log,"\n");
    original_scale=(ScaleFn)tables[0][4]; original_render=(RenderFn)tables[0][20];
    for(int t=1;t<4;t++) if(tables[t][4]!=(void*)original_scale || tables[t][20]!=(void*)original_render) {
        log_text(log,"DISABLED: incompatible render family\n"); log_close(log); return 0;
    }
    HMODULE pinned;
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,(LPCWSTR)&render_hook,&pinned)) {
        log_close(log); return 0;
    }
    BOOL installed=TRUE;
    for(int t=0;t<4 && installed;t++) {
        installed=exchange_slot(&tables[t][1],(void*)original_destroy[t],(void*)destroy_hooks[t])
            && exchange_slot(&tables[t][20],(void*)original_render,(void*)render_hook)
            && exchange_slot(&tables[t][4],(void*)original_scale,(void*)scale_hooks[t]);
    }
    if(!installed) {
        for(int t=3;t>=0;t--) {
            exchange_slot(&tables[t][4],(void*)scale_hooks[t],(void*)original_scale);
            exchange_slot(&tables[t][20],(void*)render_hook,(void*)original_render);
            exchange_slot(&tables[t][1],(void*)destroy_hooks[t],(void*)original_destroy[t]);
        }
        log_text(log,"DISABLED: installation failed; own hooks rolled back\n"); log_close(log); return 0;
    }
    InterlockedExchange(&enabled,1);
    log_text(log,"READY 3.0.0.0: render bridge; 500ms expiry; no C runtime. Instant scale reports separately in FirstPersonExplorerScaleDiagnostic.log, camera in FirstPersonExplorerCamera.log.\n");
    log_close(log);
    camera_start((HMODULE)module,base,where.camera_update,where.camera_zoom,where.camera_failure,where.eye_fix_ok);
    scale_diag_run((HMODULE)module,where.scale_hook,where.scale_failure);
    return 0;
}
BOOL WINAPI _DllMainCRTStartup(HINSTANCE module,DWORD reason,LPVOID reserved) {
    (void)reserved;
    if(reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread=CreateThread(NULL,0,initialize,module,0,NULL);
        if(thread)CloseHandle(thread);
    }
    return TRUE;
}
#endif
