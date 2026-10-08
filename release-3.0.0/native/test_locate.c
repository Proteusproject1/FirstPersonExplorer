/* Locator tests: synthetic masking cases, then the real installed bg3_dx11.exe mapped the
   way the Windows loader maps it (sections at their RVAs, base relocations applied at a
   different address), then tampered copies that must be refused. Reads the game file only. */
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "locate.c"

static unsigned char *image; static SIZE_T image_size;
static unsigned int rva(const void *p) { return (unsigned int)((const unsigned char*)p-image); }
static void map_exe(const wchar_t *path) {
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    assert(f!=INVALID_HANDLE_VALUE);
    LARGE_INTEGER size; assert(GetFileSizeEx(f,&size));
    unsigned char *file=VirtualAlloc(NULL,(SIZE_T)size.QuadPart,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(file);
    DWORD got; SIZE_T done=0;
    while(done<(SIZE_T)size.QuadPart) { assert(ReadFile(f,file+done,(DWORD)((SIZE_T)size.QuadPart-done>(1u<<30)?(1u<<30):(SIZE_T)size.QuadPart-done),&got,NULL) && got); done+=got; }
    CloseHandle(f);
    IMAGE_NT_HEADERS64 *nt=(IMAGE_NT_HEADERS64*)(file+((IMAGE_DOS_HEADER*)file)->e_lfanew);
    image_size=nt->OptionalHeader.SizeOfImage;
    image=VirtualAlloc(NULL,image_size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE); assert(image);
    memcpy(image,file,nt->OptionalHeader.SizeOfHeaders);
    IMAGE_SECTION_HEADER *s=IMAGE_FIRST_SECTION(nt);
    for(unsigned i=0;i<nt->FileHeader.NumberOfSections;i++) {
        SIZE_T n=s[i].SizeOfRawData; if(s[i].Misc.VirtualSize && n>s[i].Misc.VirtualSize) n=s[i].Misc.VirtualSize;
        if(s[i].VirtualAddress+n>image_size) n=image_size-s[i].VirtualAddress;
        memcpy(image+s[i].VirtualAddress,file+s[i].PointerToRawData,n);
    }
    /* Apply base relocations: this buffer is not at the preferred ImageBase, like ASLR. */
    uint64_t delta=(uint64_t)(uintptr_t)image-nt->OptionalHeader.ImageBase; assert(delta);
    IMAGE_DATA_DIRECTORY dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    unsigned char *r=image+dir.VirtualAddress,*end=r+dir.Size; unsigned applied=0;
    while(r<end) {
        IMAGE_BASE_RELOCATION *b=(IMAGE_BASE_RELOCATION*)r; if(!b->SizeOfBlock) break;
        WORD *e=(WORD*)(b+1); unsigned count=(b->SizeOfBlock-sizeof(*b))/2;
        for(unsigned i=0;i<count;i++) if((e[i]>>12)==IMAGE_REL_BASED_DIR64) {
            *(unaligned_u64*)(image+b->VirtualAddress+(e[i]&0xfff))+=delta; applied++;
        }
        r+=b->SizeOfBlock;
    }
    assert(applied>1000);
    VirtualFree(file,0,MEM_RELEASE);
}
static void synthetic(void) {
    /* call rel32; mov rax,[rip+disp32]; mov [rsp+8],rbx; jmp rel8; cmp dword [rip+disp32],imm8 */
    unsigned char ref[]={0xe8,1,2,3,4, 0x48,0x8b,0x05,5,6,7,8, 0x48,0x89,0x5c,0x24,0x08, 0xeb,0x10, 0x83,0x3d,9,10,11,12,0x7f, 0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90,0x90};
    unsigned char c[sizeof(ref)]; SIZE_T n=26;
    memcpy(c,ref,sizeof(c)); c[1]=0x99; c[4]=0x77; c[8]=0x55; c[11]=0x44; c[21]=0x33; c[24]=0x22;
    assert(fpe_masked_equal(c,ref,n));                              /* moved code: only targets differ */
    memcpy(c,ref,sizeof(c)); c[0]=0xe9; assert(!fpe_masked_equal(c,ref,n)); /* call became jmp */
    memcpy(c,ref,sizeof(c)); c[16]=0x10; assert(!fpe_masked_equal(c,ref,n)); /* stack offset changed */
    memcpy(c,ref,sizeof(c)); c[18]=0x11; assert(!fpe_masked_equal(c,ref,n)); /* short branch changed */
    memcpy(c,ref,sizeof(c)); c[25]=0x7e; assert(!fpe_masked_equal(c,ref,n)); /* immediate after RIP disp changed */
    memcpy(c,ref,sizeof(c)); c[7]=0x45; assert(!fpe_masked_equal(c,ref,n)); /* RIP form became [rbp+disp8] */
    puts("PASS masking: relative call/RIP displacements ignored; opcode, stack offset, short branch, immediate, addressing form enforced");
}
static void expect_real(const FpeLocation *w) {
    assert(!w->failure && !w->scale_failure && !w->camera_failure);
    assert(rva(w->camera_update)==0x1e0ddb0 && rva(w->camera_zoom)==0x1e1a230 && w->eye_fix_ok==1);
    assert(rva(w->scale_method)==0x3c60050 && rva(w->render_method)==0x3c5f050);
    assert(rva(w->scale_update)==0x2d9b260 && rva(w->scale_hook)==0x2d9b260+0x543);
    static const unsigned expected[4]={0x58b8ff8,0x58c2a88,0x58b9118,0x58b2d40};
    for(int t=0;t<4;t++) assert(rva(w->tables[t])==expected[t]);
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    synthetic();
    map_exe(L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Baldurs Gate 3\\bin\\bg3_dx11.exe");
    FpeLocation w; ULONGLONG t0=GetTickCount64(); fpe_locate(image,&w); ULONGLONG ms=GetTickCount64()-t0;
    printf("installed exe: stamp=%u tables=%u failure=%s scale_failure=%s scan=%llums\n",w.stamp,w.table_count,
        w.failure?w.failure:"none",w.scale_failure?w.scale_failure:"none",(unsigned long long)ms);
    expect_real(&w);
    printf("PASS updated game (stamp %u): scale 0x%x render 0x%x update 0x%x tables 0x%x 0x%x 0x%x 0x%x\n",w.stamp,
        rva(w.scale_method),rva(w.render_method),rva(w.scale_update),rva(w.tables[0]),rva(w.tables[1]),rva(w.tables[2]),rva(w.tables[3]));
    /* Tampering: each change must be refused, and restoring it must be accepted again. */
    unsigned char *sm=w.scale_method,*rm=w.render_method,*su=w.scale_update; void **t2=w.tables[2];
    unsigned char saved[64]; unsigned char b;
    b=rm[0]; rm[0]^=0xff; fpe_locate(image,&w); assert(w.failure && !strcmp(w.failure,"render method not found")); rm[0]=b;
    b=sm[13]; sm[13]^=0x01; fpe_locate(image,&w); assert(w.failure && !strcmp(w.failure,"scale method not found")); sm[13]=b;
    memcpy(saved,su,64); memcpy(su,sm,48); fpe_locate(image,&w);
    assert(w.failure && !strcmp(w.failure,"scale method found more than once") && w.scale_failure); memcpy(su,saved,64);
    void *slot=t2[20]; t2[20]=t2[21]; fpe_locate(image,&w); assert(w.failure && w.table_count==3); t2[20]=slot;
    slot=t2[7]; t2[7]=t2[1]; fpe_locate(image,&w); assert(w.failure && !strcmp(w.failure,"render-object table layout changed")); t2[7]=slot;
    /* The hook point lies inside the full-function check, so it is refused there first. */
    b=su[0x543]; su[0x543]^=0x01; fpe_locate(image,&w); assert(!w.failure && w.scale_failure); su[0x543]=b;
    /* A frame-offset change inside the function must disable instant scale (the hook reads that frame). */
    unsigned char *frame_use=0;
    for(SIZE_T i=0;i+7<sizeof(scale_update_bytes);i++) if(su[i]==0x48 && su[i+1]==0x8d && su[i+2]==0x95) { frame_use=su+i; break; }
    assert(frame_use); b=frame_use[3]; frame_use[3]^=0x08; fpe_locate(image,&w); assert(!w.failure && w.scale_failure && !strcmp(w.scale_failure,"scale-transition function not found")); frame_use[3]=b;
    /* An unreadable page must be skipped, never read (a read would crash the game). */
    DWORD old; unsigned char *page=(unsigned char*)((uintptr_t)rm&~(uintptr_t)0xfff);
    assert(VirtualProtect(page,4096,PAGE_NOACCESS,&old)); fpe_locate(image,&w);
    assert(w.failure && !strcmp(w.failure,"render method not found") && !w.scale_failure);
    assert(VirtualProtect(page,4096,PAGE_GUARD|PAGE_READWRITE,&old)); fpe_locate(image,&w);
    assert(w.failure && !strcmp(w.failure,"render method not found"));
    assert(VirtualProtect(page,4096,PAGE_READWRITE,&old));
    fpe_locate(image,&w); expect_real(&w);
    puts("PASS no-access and guard pages are skipped without being touched");
    /* 0.7.8.0: a changed camera prologue disables only the camera feature. */
    unsigned char *cu=w.camera_update; b=cu[45]; cu[45]^=0x01; fpe_locate(image,&w);
    assert(!w.failure && !w.scale_failure && w.camera_failure && !strcmp(w.camera_failure,"camera update not found") && !w.camera_update); cu[45]=b;
    fpe_locate(image,&w); expect_real(&w);
    puts("PASS camera update found at 0x1e0ddb0; a changed camera prologue disables only the camera feature");
    /* 0.7.9.0: the zoom step is required too. */
    unsigned char *cz=w.camera_zoom; b=cz[40]; cz[40]^=0x01; fpe_locate(image,&w);
    assert(!w.failure && w.camera_failure && !strcmp(w.camera_failure,"camera zoom step not found") && !w.camera_update && !w.camera_zoom); cz[40]=b;
    fpe_locate(image,&w); expect_real(&w);
    puts("PASS camera zoom step found at 0x1e1a230; a changed zoom prologue disables only the camera feature");
    /* 0.8.0.0: the eye fix needs the eye-distance code (update+0xB23); changed or doubled = eye fix off only. */
    unsigned char *ed=w.camera_update+0xb23;
    assert(ed[0]==0x89 && ed[2]==0x5c && ed[6]==0xf3 && ed[10]==0x5c);
    b=ed[2]; ed[2]=0x60; fpe_locate(image,&w); assert(!w.failure && !w.camera_failure && w.camera_update && w.eye_fix_ok==0); ed[2]=b;
    unsigned char spare[14]; unsigned char *dup=w.camera_update+0x1300; memcpy(spare,dup,14); memcpy(dup,ed,14);
    fpe_locate(image,&w); assert(!w.camera_failure && w.eye_fix_ok==0); memcpy(dup,spare,14);
    fpe_locate(image,&w); expect_real(&w);
    puts("PASS eye-distance code found once at update+0xb23; changed or duplicated disables only the eye fix");
    puts("PASS refusals: changed render/scale code, duplicate match, missing table, changed table layout, changed hook instructions, changed frame offset; restored image accepted again");
    return 0;
}
