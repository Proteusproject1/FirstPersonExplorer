#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

/* Exact-build DX11 prototype. No object layout writes or code detours. */
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
static void log_close(HANDLE log) {
    if(log!=INVALID_HANDLE_VALUE) CloseHandle(log);
}
static BOOL bytes_equal(const unsigned char *a,const unsigned char *b,int count) {
    for(int i=0;i<count;i++) if(a[i]!=b[i]) return FALSE;
    return TRUE;
}
static DWORD WINAPI initialize(void *module) {
    HANDLE log=open_log((HMODULE)module);
    unsigned char *base=(unsigned char*)GetModuleHandleW(NULL);
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64*)(base+dos->e_lfanew);
    #include "verified_table.h"
    if(nt->FileHeader.TimeDateStamp!=1780568190u || nt->OptionalHeader.SizeOfImage!=106659840u) {
        log_text(log,"DISABLED: unsupported executable. Requires verified BG3 DX11 build.\n"); log_close(log); return 0;
    }
    void **tables[4];
    for(int t=0;t<4;t++) {
        tables[t]=(void**)(base+verified_tables[t]);
        for(int i=0;i<30;i++) if(tables[t][i]!=(void*)(base+verified_rvas[t][i])) {
            log_text(log,"DISABLED: table "); log_number(log,t); log_text(log," mismatch at slot "); log_number(log,i); log_text(log,"\n");
            log_close(log); return 0;
        }
        for(int i=0;i<3;i++) if(!bytes_equal(base+verified_rvas[t][verified_slots[i]],verified_bytes[t][i],32)) {
            log_text(log,"DISABLED: method fingerprint mismatch table "); log_number(log,t); log_text(log," slot "); log_number(log,verified_slots[i]); log_text(log,"\n");
            log_close(log); return 0;
        }
        original_destroy[t]=(DestroyFn)tables[t][1];
    }
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
    log_text(log,"READY 0.7.6.7 Public Release: DX11 render bridge; four validated mesh tables including static equipment; 500ms expiry; no C runtime.\n");
    log_close(log);
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
