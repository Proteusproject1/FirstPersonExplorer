#define FPE_TEST
#include <stdio.h>
#include "fpe_render.c"
#undef NDEBUG
#include <assert.h>
static volatile LONG scale_calls,render_calls,destroy_calls[4];
static void test_scale(void *obj,const float *s){(void)obj;(void)s;InterlockedIncrement(&scale_calls);}
static void test_draw(void *obj,const void*a,const void*b,const void*c){(void)obj;(void)a;(void)b;(void)c;InterlockedIncrement(&render_calls);}
static void *d0(void *obj,unsigned int f){(void)f;destroy_calls[0]++;return obj;}
static void *d1(void *obj,unsigned int f){(void)f;destroy_calls[1]++;return obj;}
static void *d2(void *obj,unsigned int f){(void)f;destroy_calls[2]++;return obj;}
static void *d3(void *obj,unsigned int f){(void)f;destroy_calls[3]++;return obj;}
static DWORD WINAPI stress(void *arg) {
    int index=(int)(uintptr_t)arg;
    void *object=(void*)(uintptr_t)(10000+index);
    void *data=object;
    for(int i=0;i<10000;i++) {
        scale_hooks[index%4](object,hide_signal);
        render_hook(object,&data,0,0);
        scale_hooks[index%4](object,show_signal);
    }
    return 0;
}
int main(void) {
    int objects[4],receiver;
    original_scale=test_scale;original_render=test_draw;
    original_destroy[0]=d0;original_destroy[1]=d1;original_destroy[2]=d2;original_destroy[3]=d3;enabled=1;
    for(int i=0;i<4;i++) {
        scale_hooks[i](&objects[i],probe_signal);
        assert(scale_calls==0 && !should_hide(&objects[i],GetTickCount64()));
        scale_hooks[i](&objects[i],hide_signal);
        void *data=&objects[i];
        render_hook(&receiver,&data,0,0);assert(render_calls==0);
    }
    void *unrelated=&receiver;
    render_hook(&objects[0],&unrelated,0,0);assert(render_calls==1);
    float normal[3]={1,1,1};scale_hooks[0](&objects[0],normal);assert(scale_calls==1);
    assert(should_hide(&objects[0],GetTickCount64()));
    for(int i=0;i<4;i++) {
        destroy_hooks[i](&objects[i],1);
        assert(destroy_calls[i]==1 && !should_hide(&objects[i],GetTickCount64()));
    }
    renew(&objects[0],100);assert(should_hide(&objects[0],599));assert(!should_hide(&objects[0],600));
    scale_hooks[0](&objects[0],hide_signal);scale_hooks[0](&objects[0],show_signal);
    assert(!should_hide(&objects[0],GetTickCount64()));
    for(int i=0;i<512;i++)assert(renew((void*)(uintptr_t)(i+1),1000));
    assert(!renew((void*)9999,1000));assert(renew((void*)9999,1500));
    HANDLE threads[4];
    for(int i=0;i<4;i++)threads[i]=CreateThread(NULL,0,stress,(void*)(uintptr_t)i,0,NULL);
    assert(WaitForMultipleObjects(4,threads,TRUE,15000)==WAIT_OBJECT_0);
    for(int i=0;i<4;i++){CloseHandle(threads[i]);assert(!should_hide((void*)(uintptr_t)(10000+i),GetTickCount64()));}
    void **page=VirtualAlloc(NULL,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);assert(page);
    page[20]=(void*)test_draw;DWORD old;assert(VirtualProtect(page,4096,PAGE_READONLY,&old));
    assert(exchange_slot(&page[20],(void*)test_draw,(void*)render_hook));
    assert(page[20]==(void*)render_hook);
    assert(!exchange_slot(&page[20],(void*)test_draw,(void*)scale_hook_0));
    assert(exchange_slot(&page[20],(void*)render_hook,(void*)test_draw));
    MEMORY_BASIC_INFORMATION mbi;VirtualQuery(page,&mbi,sizeof(mbi));assert(mbi.Protect==PAGE_READONLY);
    VirtualFree(page,0,MEM_RELEASE);
    puts("PASS native: 4 table dispatch including static equipment, preflight, drawn-object identity, unrelated objects, animation, destructor routing, expiry, capacity, 4-thread stress, protected-slot install/conflict/rollback");
    return 0;
}
