#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "scale_diag.h"
#include "vendor/minhook/include/MinHook.h"
#include "verified_scale.h"

#define QUEUE_SIZE 2048u
#define SAMPLE_LIMIT 20000u
typedef struct { uint64_t tick,entity; uint32_t bits[4]; BOOL snapped; } ScaleSample;
static ScaleSample queue[QUEUE_SIZE];
static unsigned int head,tail;
static volatile LONG captured,dropped,rejected,finished;
static SRWLOCK sample_lock=SRWLOCK_INIT;
void *scale_diag_trampoline;
static BOOL equal_bytes(const unsigned char*,const unsigned char*,SIZE_T);
static unsigned int put_hex(char*,uint64_t,unsigned int);
#include "scale_requests.h"

static BOOL copy_read(void *out,const void *source,SIZE_T size) {
    SIZE_T count=0;
    return source && ReadProcessMemory(GetCurrentProcess(),source,out,size,&count) && count==size;
}
static BOOL valid_float_bits(uint32_t v) {
    return !(v&0x80000000u) && (v&0x7f800000u)!=0x7f800000u && v!=0;
}
/* Engine-thread-only: authorize a one-shot current-scale write. No retained
   engine pointers or file I/O; the worker owns mailbox and diagnostic files. */
void scale_diag_capture(float *record,const unsigned char *frame) {
    ScaleSample sample;
    uintptr_t handles=0,visual=0;
    int index=-1;
    if(!copy_read(&index,frame+0x2c8,sizeof(index)) || index<0 || index>1048576
        || !copy_read(&handles,frame+0x2b8,sizeof(handles)) || !handles
        || !copy_read(&sample.entity,(void*)(handles+(uintptr_t)index*8),8)
        || !copy_read(sample.bits,record,12)
        || !copy_read(&visual,frame+0x70,sizeof(visual)) || !visual
        || !copy_read(&sample.bits[3],(void*)(visual+0xc),4)) {
        InterlockedIncrement(&rejected); return;
    }
    if(!sample.entity || !valid_float_bits(sample.bits[1]) || !valid_float_bits(sample.bits[2])
        || !valid_float_bits(sample.bits[3])) { InterlockedIncrement(&rejected); return; }
    sample.tick=GetTickCount64();
    sample.snapped=try_snap(record,sample.entity,sample.tick);
    if(InterlockedCompareExchange(&finished,0,0)) return;
    if(!TryAcquireSRWLockExclusive(&sample_lock)) { InterlockedIncrement(&dropped); return; }
    if(InterlockedCompareExchange(&finished,0,0)) { ReleaseSRWLockExclusive(&sample_lock); return; }
    unsigned int next=(head+1)%QUEUE_SIZE;
    if(next==tail) { ReleaseSRWLockExclusive(&sample_lock); InterlockedIncrement(&dropped); return; }
    queue[head]=sample; head=next;
    LONG total=InterlockedIncrement(&captured);
    if(total>=(LONG)SAMPLE_LIMIT) InterlockedExchange(&finished,1);
    ReleaseSRWLockExclusive(&sample_lock);
}
static BOOL pop_sample(ScaleSample *sample) {
    AcquireSRWLockExclusive(&sample_lock);
    BOOL found=head!=tail;
    if(found) { *sample=queue[tail]; tail=(tail+1)%QUEUE_SIZE; }
    ReleaseSRWLockExclusive(&sample_lock);
    return found;
}
static BOOL equal_bytes(const unsigned char *a,const unsigned char *b,SIZE_T size) {
    for(SIZE_T i=0;i<size;i++) if(a[i]!=b[i]) return FALSE;
    return TRUE;
}
static void write_text(HANDLE file,const char *text) {
    DWORD size=0,written;
    while(text[size]) size++;
    WriteFile(file,text,size,&written,NULL);
}
static unsigned int put_uint(char *out,uint64_t value) {
    char reversed[20]; unsigned int n=0;
    do { reversed[n++]=(char)('0'+value%10); value/=10; } while(value);
    for(unsigned int i=0;i<n;i++) out[i]=reversed[n-i-1];
    return n;
}
static unsigned int put_hex(char *out,uint64_t value,unsigned int digits) {
    static const char hex[]="0123456789abcdef";
    for(unsigned int i=0;i<digits;i++) out[i]=hex[(value>>((digits-1-i)*4))&15];
    return digits;
}
static void write_sample(HANDLE file,const ScaleSample *s) {
    char line[100]; unsigned int n=put_uint(line,s->tick);
    line[n++]='\t'; n+=put_hex(line+n,s->entity,16);
    for(int i=0;i<4;i++) { line[n++]='\t'; n+=put_hex(line+n,s->bits[i],8); }
    line[n++]='\t'; line[n++]=s->snapped?'1':'0';
    line[n++]='\n'; DWORD written; WriteFile(file,line,n,&written,NULL);
}
static void write_count(HANDLE file,const char *label,LONG value) {
    char line[24]; unsigned int n=put_uint(line,(uint32_t)value); line[n++]='\n';
    DWORD written; write_text(file,label); WriteFile(file,line,n,&written,NULL);
}
static HANDLE open_diagnostic(HMODULE module) {
    const wchar_t name[]=L"FirstPersonExplorerScaleDiagnostic.log";
    wchar_t path[MAX_PATH]; DWORD n=GetModuleFileNameW(module,path,MAX_PATH);
    if(!n || n>=MAX_PATH) return INVALID_HANDLE_VALUE;
    while(n && path[n-1]!=L'\\') n--;
    if(!n || n+sizeof(name)/sizeof(name[0])>MAX_PATH) return INVALID_HANDLE_VALUE;
    for(unsigned int i=0;i<sizeof(name)/sizeof(name[0]);i++) path[n+i]=name[i];
    return CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
}
void scale_diag_run(HMODULE module,unsigned char *hook_point,const char *locate_failure) {
    HANDLE file=open_diagnostic(module);
    if(file==INVALID_HANDLE_VALUE) return;
    write_text(file,"FPE Native 3.0.0.0 INSTANT SCALE; one-shot PAK authorization required\n");
    if(!mailbox_init()) { write_text(file,"DISABLED: mailbox path unavailable\n"); CloseHandle(file); return; }
    /* locate.c verified the full function (masked only for moved code) and the hook instructions. */
    if(!hook_point) {
        write_text(file,"DISABLED: "); write_text(file,locate_failure?locate_failure:"scale-transition hook not located");
        write_text(file,"; rendering bridge remains independent\n");
        CloseHandle(file); return;
    }
    /* MinHook is shared with the camera hook (0.7.8.0); it may already be initialised. */
    MH_STATUS status=MH_Initialize();
    if(status==MH_ERROR_ALREADY_INITIALIZED) status=MH_OK;
    if(status!=MH_OK) { write_count(file,"DISABLED: MinHook initialize status=",status); CloseHandle(file); return; }
    void *target=hook_point;
    status=MH_CreateHook(target,(void*)scale_diag_hook,&scale_diag_trampoline);
    if(status==MH_OK) status=MH_EnableHook(target);
    if(status!=MH_OK) {
        write_count(file,"DISABLED: diagnostic hook status=",status);
        MH_RemoveHook(target); CloseHandle(file); return;
    }
    write_text(file,"READY: guarded instant transitions; native-clock ticket TTL=350ms; other scale multipliers fall back\n");
    write_text(file,"uptime_ms\tserver_entity_hex\tfield0_f32_hex\ttarget_f32_hex\tcurrent_f32_hex\tvisual_f32_hex\tsnapped\n");
    ULONGLONG next_stats=GetTickCount64()+5000;
    ULONGLONG deadline=GetTickCount64()+15*60*1000;
    BOOL capture_closed=FALSE;
    for(;;) {
        mailbox_poll(GetTickCount64());
        if(GetTickCount64()>=deadline) InterlockedExchange(&finished,1);
        ScaleSample sample;
        while(pop_sample(&sample)) write_sample(file,&sample);
        if(GetTickCount64()>=next_stats) {
            write_count(file,"# captured=",captured); write_count(file,"# dropped=",dropped); write_count(file,"# rejected=",rejected);
            write_count(file,"# snapped=",snap_count); write_count(file,"# mismatched=",mismatch_count);
            FlushFileBuffers(file); next_stats=GetTickCount64()+5000;
        }
        if(!capture_closed && InterlockedCompareExchange(&finished,0,0)) {
            while(pop_sample(&sample)) write_sample(file,&sample);
            write_text(file,"SAMPLE CAPTURE COMPLETE: instant transitions and summary counters remain active\n");
            capture_closed=TRUE;
        }
        Sleep(5);
    }
}
