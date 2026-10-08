#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include "locate.h"
#include "vendor/minhook/src/hde/hde64.h"
/* Reference code and table layout from the verified 4.1.1.7398727 executable. */
#include "verified_table.h"
#include "verified_scale.h"
#include "verified_camera.h"

#define SCALE_HOOK_OFFSET (SCALE_HOOK_RVA-SCALE_UPDATE_RVA)
#define MAX_RANGES 64
#define MAX_TABLES 8
/* LEA RDI,[R14+RSI*4]; MOVSS XMM4,[RDI+8]: the registers scale_diag_hook.S relies on. */
static const unsigned char hook_site[9]={0x49,0x8d,0x3c,0xb6,0xf3,0x0f,0x10,0x67,0x08};
typedef uint64_t unaligned_u64 __attribute__((aligned(1)));
typedef struct { unsigned char *start; SIZE_T size; } Range;
typedef struct { const unsigned char *reference; SIZE_T size; uint64_t prefix; unsigned char *hit; unsigned int count; } Pattern;

static unsigned int immediate_size(uint32_t flags) {
    if(flags&F_IMM64) return 8;
    if(flags&F_IMM32) return 4;
    if(flags&F_IMM16) return 2;
    if(flags&F_IMM8) return 1;
    return 0;
}
/* Instruction-by-instruction comparison. Only fields that change whenever code moves
   are ignored: 32-bit relative branch/call targets and RIP-relative displacements.
   Opcodes, registers, stack/frame offsets and immediates must match exactly. */
BOOL fpe_masked_equal(const unsigned char *candidate,const unsigned char *reference,SIZE_T size) {
    SIZE_T i=0;
    while(i<size) {
        hde64s hs;
        unsigned int length=hde64_disasm(candidate+i,&hs);
        if(!length || (hs.flags&F_ERROR)) return FALSE;
        SIZE_T skip_from=length,skip_to=length;
        if((hs.flags&F_RELATIVE) && (hs.flags&F_IMM32)) { skip_from=length-4; }
        else if((hs.flags&F_MODRM) && hs.modrm_mod==0 && hs.modrm_rm==5) {
            skip_to=length-immediate_size(hs.flags); skip_from=skip_to-4;
        }
        for(SIZE_T j=0;j<length && i+j<size;j++)
            if((j<skip_from || j>=skip_to) && candidate[i+j]!=reference[i+j]) return FALSE;
        i+=length;
    }
    return TRUE;
}
static uint64_t prefix_of(const unsigned char *bytes) { return *(const unaligned_u64*)bytes; }
static BOOL inside(const Range *ranges,int count,const void *p,SIZE_T size) {
    const unsigned char *b=(const unsigned char*)p;
    for(int i=0;i<count;i++) if(b>=ranges[i].start && b+size<=ranges[i].start+ranges[i].size) return TRUE;
    return FALSE;
}
/* Only committed, readable pages are ever read, so a guard or no-access page inside a
   section can never fault; a pattern crossing such a page is simply not found. */
static BOOL readable(const MEMORY_BASIC_INFORMATION *m) {
    if(m->State!=MEM_COMMIT || (m->Protect&(PAGE_GUARD|PAGE_NOACCESS))) return FALSE;
    return (m->Protect&(PAGE_READONLY|PAGE_READWRITE|PAGE_WRITECOPY|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY))!=0;
}
static void add_readable(Range *list,int *count,unsigned char *start,SIZE_T size) {
    unsigned char *p=start,*end=start+size;
    while(p<end && *count<MAX_RANGES) {
        MEMORY_BASIC_INFORMATION m;
        if(!VirtualQuery(p,&m,sizeof(m))) return;
        unsigned char *region_end=(unsigned char*)m.BaseAddress+m.RegionSize;
        if(region_end>end) region_end=end;
        if(readable(&m)) {
            if(*count>0 && list[*count-1].start+list[*count-1].size==p) list[*count-1].size+=(SIZE_T)(region_end-p);
            else list[(*count)++]=(Range){p,(SIZE_T)(region_end-p)};
        }
        p=region_end;
    }
}
static BOOL sections(unsigned char *image,Range *code,int *codes,Range *data,int *datas,unsigned int *stamp) {
    IMAGE_DOS_HEADER *dos=(IMAGE_DOS_HEADER*)image;
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE) return FALSE;
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64*)(image+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) return FALSE;
    *stamp=nt->FileHeader.TimeDateStamp;
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    *codes=*datas=0;
    for(unsigned int i=0;i<nt->FileHeader.NumberOfSections;i++) {
        DWORD c=s[i].Characteristics; SIZE_T size=s[i].Misc.VirtualSize;
        if(!size || s[i].VirtualAddress+size>nt->OptionalHeader.SizeOfImage) continue;
        if(c&IMAGE_SCN_MEM_EXECUTE) add_readable(code,codes,image+s[i].VirtualAddress,size);
        else if((c&IMAGE_SCN_CNT_INITIALIZED_DATA) && (c&IMAGE_SCN_MEM_READ) && !(c&IMAGE_SCN_MEM_WRITE))
            add_readable(data,datas,image+s[i].VirtualAddress,size);
    }
    return *codes>0 && *datas>0;
}
static void scan(const Range *code,int codes,Pattern *patterns,int count) {
    for(int r=0;r<codes;r++) {
        unsigned char *start=code[r].start,*end=start+code[r].size;
        for(unsigned char *p=start;p+8<=end;p++) {
            uint64_t v=*(const unaligned_u64*)p;
            for(int k=0;k<count;k++) {
                Pattern *x=&patterns[k];
                /* 16 spare bytes: the decoder may read a full instruction past the end. */
                if(v==x->prefix && p+x->size+16<=end && fpe_masked_equal(p,x->reference,x->size)) {
                    if(!x->hit) x->hit=p;
                    x->count++;
                }
            }
        }
    }
}
/* Do the found tables share functions exactly where the reference tables did? */
static BOOL same_layout(void **found[4],const int order[4]) {
    for(int a=0;a<4;a++) for(int k=0;k<30;k++) for(int b=0;b<4;b++) for(int l=0;l<30;l++) {
        BOOL reference_same=verified_rvas[a][k]==verified_rvas[b][l];
        BOOL found_same=found[order[a]][k]==found[order[b]][l];
        if(reference_same!=found_same) return FALSE;
    }
    return TRUE;
}
/* 0.8.0.0: the camera update copies Distance to 0x15C and builds the final eye from 0x15C right after
   (mov [rdi+0x15C],eax ; movss xmm2,[rdi+0x15C]). The eye fix writes 0x15C, so it is allowed only when
   this exact pair appears once in the update's first 0x1400 bytes, all inside readable code. */
static const unsigned char eye_distance_bytes[14]={0x89,0x87,0x5c,0x01,0x00,0x00,0xf3,0x0f,0x10,0x97,0x5c,0x01,0x00,0x00};
#define EYE_SEARCH 0x1400
static int eye_distance_found(const Range *code,int codes,const unsigned char *update) {
    if(!inside(code,codes,update,EYE_SEARCH)) return 0;
    int count=0;
    for(SIZE_T i=0;i+sizeof(eye_distance_bytes)<=EYE_SEARCH;i++) {
        SIZE_T k=0; while(k<sizeof(eye_distance_bytes) && update[i+k]==eye_distance_bytes[k]) k++;
        if(k==sizeof(eye_distance_bytes)) count++;
    }
    return count==1;
}
void fpe_locate(unsigned char *image,FpeLocation *out) {
    Range code[MAX_RANGES],data[MAX_RANGES]; int codes,datas;
    for(SIZE_T i=0;i<sizeof(*out);i++) ((unsigned char*)out)[i]=0;
    out->scale_failure="not checked (render bridge unavailable)";
    out->camera_failure="not checked (render bridge unavailable)";
    if(!sections(image,code,&codes,data,&datas,&out->stamp)) { out->failure="executable layout not recognised"; return; }
    Pattern p[5]={
        {verified_bytes[0][1],32,prefix_of(verified_bytes[0][1]),0,0},  /* slot 4: scale  */
        {verified_bytes[0][2],32,prefix_of(verified_bytes[0][2]),0,0},  /* slot 20: render */
        {scale_update_bytes,sizeof(scale_update_bytes),prefix_of(scale_update_bytes),0,0},
        {camera_update_bytes,sizeof(camera_update_bytes),prefix_of(camera_update_bytes),0,0},
        {camera_zoom_bytes,sizeof(camera_zoom_bytes),prefix_of(camera_zoom_bytes),0,0}};
    scan(code,codes,p,5);
    if(p[3].count!=1) out->camera_failure=p[3].count?"camera update found more than once":"camera update not found";
    else if(p[4].count!=1) out->camera_failure=p[4].count?"camera zoom step found more than once":"camera zoom step not found";
    else { out->camera_update=p[3].hit; out->camera_zoom=p[4].hit; out->camera_failure=0; out->eye_fix_ok=eye_distance_found(code,codes,p[3].hit); }
    /* Instant scale is independent of the render bridge, exactly as before. */
    if(p[2].count==0) out->scale_failure="scale-transition function not found";
    else if(p[2].count>1) out->scale_failure="scale-transition function found more than once";
    else {
        unsigned char *hook=p[2].hit+SCALE_HOOK_OFFSET;
        BOOL same=TRUE;
        for(unsigned int i=0;i<sizeof(hook_site);i++) if(hook[i]!=hook_site[i]) same=FALSE;
        if(!same) out->scale_failure="scale-transition hook instructions changed";
        else { out->scale_update=p[2].hit; out->scale_hook=hook; out->scale_failure=0; }
    }
    if(p[0].count!=1) { out->failure=p[0].count?"scale method found more than once":"scale method not found"; return; }
    if(p[1].count!=1) { out->failure=p[1].count?"render method found more than once":"render method not found"; return; }
    out->scale_method=p[0].hit; out->render_method=p[1].hit;
    void **found[MAX_TABLES]; unsigned int n=0;
    for(int r=0;r<datas;r++) {
        unsigned char *start=data[r].start+((8-((uintptr_t)data[r].start&7))&7),*end=data[r].start+data[r].size;
        for(unsigned char *t=start;t+30*sizeof(void*)<=end;t+=sizeof(void*)) {
            void **table=(void**)t;
            if(table[4]==(void*)p[0].hit && table[20]==(void*)p[1].hit) { if(n<MAX_TABLES) found[n]=table; n++; }
        }
    }
    out->table_count=n;
    if(n!=4) { out->failure="render-object tables not recognised (expected exactly 4)"; return; }
    for(unsigned int f=0;f<4;f++) for(int k=0;k<30;k++)
        if(!inside(code,codes,found[f][k],1)) { out->failure="render-object table points outside game code"; return; }
    /* Pair each found table with a reference table: its destroy method (slot 1) must
       match that table's reference code, and the whole 4x30 slot layout must agree. */
    static const int orders[24][4]={{0,1,2,3},{0,1,3,2},{0,2,1,3},{0,2,3,1},{0,3,1,2},{0,3,2,1},
        {1,0,2,3},{1,0,3,2},{1,2,0,3},{1,2,3,0},{1,3,0,2},{1,3,2,0},{2,0,1,3},{2,0,3,1},{2,1,0,3},{2,1,3,0},
        {2,3,0,1},{2,3,1,0},{3,0,1,2},{3,0,2,1},{3,1,0,2},{3,1,2,0},{3,2,0,1},{3,2,1,0}};
    BOOL destroy_ok[4][4];
    for(int t=0;t<4;t++) for(int f=0;f<4;f++)
        destroy_ok[t][f]=inside(code,codes,found[f][1],48) && fpe_masked_equal((const unsigned char*)found[f][1],verified_bytes[t][0],32);
    for(int o=0;o<24;o++) {
        const int *order=orders[o];
        BOOL ok=TRUE;
        for(int t=0;t<4 && ok;t++) ok=destroy_ok[t][order[t]];
        if(ok && same_layout(found,order)) {
            for(int t=0;t<4;t++) out->tables[t]=found[order[t]];
            out->failure=0;
            return;
        }
    }
    out->failure="render-object table layout changed";
}
