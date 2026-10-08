#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "scale_diag.c"
extern void diag_test_engine(void*,void*,void*);
extern void diag_test_site(void);
static void reset_queue(void) { head=tail=0; captured=dropped=rejected=finished=0; }
static unsigned packet(char *out,uint64_t tick,uint32_t serial,uint64_t entity,uint64_t born,uint32_t id,uint32_t units,char direction) {
    memcpy(out,"FPE9",4); put_hex(out+4,mailbox_nonce,16); put_hex(out+20,tick,16);
    put_hex(out+36,serial,8); out[44]=entity?'1':'0'; out[45]='\n';
    unsigned n=46;
    if(entity) {
        put_hex(out+n,entity,16); put_hex(out+n+16,born,16); put_hex(out+n+32,id,8);
        put_hex(out+n+40,units,8); out[n+48]=direction; out[n+49]='\n'; n+=50;
    }
    memcpy(out+n,out,46); return n+46;
}
static void test_requests(void) {
    char data[493]; mailbox_nonce=0x123456789abcdef0ull;
    uint64_t t=GetTickCount64(),entity=0x0200000100000088ull;
    unsigned n=packet(data,t,1,entity,t,1,1100000,'I');
    assert(!requests_receive(data,n-1,t));
    data[4]='0'; assert(!requests_receive(data,n,t)); data[4]='1';
    assert(!requests_receive(data,n,t+351)); assert(!requests_receive(data,n,t-1));
    assert(requests_receive(data,n,t));
    float record[3]={1.1f,0.165f,1.1f};
    assert(!try_snap(record,entity+1,t)); assert(record[2]==1.1f);
    assert(try_snap(record,entity,t)); assert(record[2]==record[1]);
    record[2]=0.8f; assert(!try_snap(record,entity,t));
    n=packet(data,t,2,entity,t,1,1100000,'I'); assert(requests_receive(data,n,t));
    assert(!try_snap(record,entity,t)); /* republishing cannot re-arm */
    n=packet(data,t,3,entity,t,3,1100000,'O'); assert(requests_receive(data,n,t));
    assert(!try_snap(record,entity,t)); /* old shrink target before status removal */
    record[1]=1.1f; assert(try_snap(record,entity,t)); assert(record[2]==1.1f);
    n=packet(data,t,4,entity,t,4,1100000,'I'); assert(requests_receive(data,n,t));
    record[1]=0.25f; assert(!try_snap(record,entity,t));
    record[1]=0.165f; assert(!try_snap(record,entity,t)); /* ambiguous target invalidates */
    n=packet(data,t,5,entity,t,5,1100000,'I'); assert(requests_receive(data,n,t));
    assert(!try_snap(record,entity,t+351));
    n=packet(data,t,6,entity,t,6,1100000,'I'); assert(requests_receive(data,n,t));
    record[0]=1.5f; assert(!try_snap(record,entity,t)); record[0]=1.1f;
    assert(!try_snap(record,entity,t));
    n=packet(data,t,7,entity,t,7,1100000,'I'); assert(requests_receive(data,n,t));
    n=packet(data,t,8,0,0,0,0,'I'); assert(requests_receive(data,n,t));
    assert(!try_snap(record,entity,t));
    requests_clear(); last_packet_tick=0; last_packet_id=0;
    puts("PASS mailbox nonce/framing/freshness, exact entity, one-shot replay guard, reversals, target/base mismatch and cancellation");
}
int main(void) {
    test_requests();
    unsigned char frame[0x400]={0},visual[32]={0};
    float record[3]={1.0f,0.15f,0.75f},saved[3];
    uint64_t handles[2]={0x123456789abc0001ull,0x223456789abc0002ull};
    uintptr_t p=(uintptr_t)handles,v=(uintptr_t)visual;
    memcpy(frame+0x2b8,&p,8); memcpy(frame+0x70,&v,8);
    float scale=0.75f; memcpy(visual+12,&scale,4); memcpy(saved,record,sizeof(record));
    scale_diag_capture(record,frame);
    ScaleSample s; assert(pop_sample(&s)); assert(s.entity==handles[0]);
    assert(memcmp(s.bits,record,12)==0); assert(memcmp(&s.bits[3],&scale,4)==0);
    assert(memcmp(record,saved,sizeof(record))==0); assert(!pop_sample(&s));
    puts("PASS exact numeric observation; entity identity; no scale writes");
    int bad=-1; memcpy(frame+0x2c8,&bad,4); scale_diag_capture(record,frame); assert(rejected==1);
    bad=0; memcpy(frame+0x2c8,&bad,4); scale_diag_capture((void*)1,frame); assert(rejected==2);
    record[1]=0; scale_diag_capture(record,frame); assert(rejected==3); record[1]=saved[1];
    puts("PASS invalid index, invalid memory and invalid float rejection");
    reset_queue(); for(unsigned i=0;i<QUEUE_SIZE+10;i++) scale_diag_capture(record,frame);
    assert(captured==QUEUE_SIZE-1 && dropped==11);
    reset_queue(); captured=SAMPLE_LIMIT-1; scale_diag_capture(record,frame); scale_diag_capture(record,frame);
    assert(captured==SAMPLE_LIMIT && finished==1);
    puts("PASS bounded queue, drop accounting and capture cap");
    reset_queue();
    unsigned char baseline[192]={0},hooked[192]={0};
    diag_test_engine(frame,record,baseline);
    assert(MH_Initialize()==MH_OK);
    assert(MH_CreateHook((void*)diag_test_site,(void*)scale_diag_hook,&scale_diag_trampoline)==MH_OK);
    assert(MH_EnableHook((void*)diag_test_site)==MH_OK);
    for(int i=0;i<100;i++) {
        diag_test_engine(frame,record,hooked);
        assert(memcmp(baseline,hooked,sizeof(baseline))==0);
    }
    assert(captured==100 && memcmp(record,saved,sizeof(record))==0);
    /* The actual assembly hook must replay MOVSS from the newly snapped value,
       preserving every other register exactly as the unhooked engine would. */
    uint64_t now=GetTickCount64(); char packet_data[493];
    unsigned packet_size=packet(packet_data,now,1,handles[0],now,1,1000000,'I');
    assert(requests_receive(packet_data,packet_size,now));
    diag_test_engine(frame,record,hooked);
    assert(record[2]==record[1]);
    memcpy(baseline+144,&record[1],4);
    assert(memcmp(baseline,hooked,sizeof(baseline))==0);
    requests_clear();
    puts("PASS authorized write through real hook; resumed MOVSS sees target; other CPU state preserved");
    assert(MH_DisableHook((void*)diag_test_site)==MH_OK);
    assert(MH_RemoveHook((void*)diag_test_site)==MH_OK); assert(MH_Uninitialize()==MH_OK);
    diag_test_engine(frame,record,hooked); assert(memcmp(baseline,hooked,sizeof(baseline))==0);
    puts("PASS real mid-function hook: volatile registers, SIMD, flags, original instructions, stack and removal");
    char hex[16]; put_hex(hex,handles[0],16); assert(memcmp(hex,"123456789abc0001",16)==0);
    unsigned char changed[sizeof(scale_update_bytes)]; memcpy(changed,scale_update_bytes,sizeof(changed));
    assert(equal_bytes(changed,scale_update_bytes,sizeof(changed))); changed[100]^=1;
    assert(!equal_bytes(changed,scale_update_bytes,sizeof(changed)));
    puts("PASS exact 64-bit handle formatting and full-function mismatch guard");
    return 0;
}
