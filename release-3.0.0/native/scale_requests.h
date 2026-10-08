/* Local PAK-to-DLL mailbox. Only the worker accesses files. The engine thread
   consumes numeric, short-lived requests under a nonblocking lock. */
#define REQUEST_SLOTS 8
#define REQUEST_TTL 350u
typedef struct {
    uint64_t entity, born;
    uint32_t id, units;
    char direction;
    BOOL consumed;
} SnapRequest;
static SnapRequest requests[REQUEST_SLOTS];
static SRWLOCK request_lock=SRWLOCK_INIT;
static uint64_t mailbox_nonce, last_packet_tick;
static uint32_t last_packet_id;
static wchar_t mailbox_path[MAX_PATH], ticket_path[MAX_PATH];
static volatile LONG snap_count, mismatch_count;

static BOOL hex_read(const char *s,unsigned n,uint64_t *out) {
    uint64_t v=0;
    for(unsigned i=0;i<n;i++) {
        unsigned c=(unsigned char)s[i],d;
        if(c>='0' && c<='9') d=c-'0';
        else if(c>='a' && c<='f') d=c-'a'+10;
        else return FALSE;
        v=(v<<4)|d;
    }
    *out=v; return TRUE;
}
static BOOL fresh_tick(uint64_t tick,uint64_t now) { return tick<=now && now-tick<=REQUEST_TTL; }
static void requests_clear(void) {
    AcquireSRWLockExclusive(&request_lock);
    for(unsigned i=0;i<REQUEST_SLOTS;i++) requests[i].entity=0;
    ReleaseSRWLockExclusive(&request_lock);
}
/* Fixed-length records and a repeated header reject truncated/mixed file reads.
   Freshness uses the DLL's clock ticket, never a guessed Lua clock offset. */
static BOOL requests_receive(const char *s,unsigned length,uint64_t now) {
    uint64_t nonce,tick,packet,count;
    if(length<92 || !equal_bytes((const unsigned char*)s,(const unsigned char*)"FPE9",4)
       || !hex_read(s+4,16,&nonce) || nonce!=mailbox_nonce
       || !hex_read(s+20,16,&tick) || !fresh_tick(tick,now)
       || !hex_read(s+36,8,&packet) || !hex_read(s+44,1,&count) || count>REQUEST_SLOTS
       || s[45]!='\n' || length!=92+50*count
       || !equal_bytes((const unsigned char*)s,(const unsigned char*)s+46+50*count,46)) return FALSE;
    if(tick<last_packet_tick || (tick==last_packet_tick && packet<=last_packet_id)) return TRUE;
    SnapRequest next[REQUEST_SLOTS]={0};
    for(unsigned i=0;i<count;i++) {
        const char *r=s+46+50*i; uint64_t id,units;
        if(!hex_read(r,16,&next[i].entity) || !next[i].entity
           || !hex_read(r+16,16,&next[i].born) || next[i].born>tick
           || !hex_read(r+32,8,&id) || !id || !hex_read(r+40,8,&units)
           || units<10000 || units>100000000 || (r[48]!='I' && r[48]!='O') || r[49]!='\n') return FALSE;
        next[i].id=(uint32_t)id; next[i].units=(uint32_t)units; next[i].direction=r[48];
        for(unsigned j=0;j<i;j++) if(next[j].entity==next[i].entity) return FALSE;
    }
    AcquireSRWLockExclusive(&request_lock);
    for(unsigned i=0;i<count;i++) for(unsigned j=0;j<REQUEST_SLOTS;j++) {
        if(next[i].entity==requests[j].entity && next[i].born==requests[j].born && next[i].id==requests[j].id) {
            /* An existing identity cannot change its payload or re-arm itself. */
            next[i].consumed=requests[j].consumed || next[i].units!=requests[j].units || next[i].direction!=requests[j].direction;
        }
    }
    for(unsigned i=0;i<REQUEST_SLOTS;i++) requests[i]=next[i];
    last_packet_tick=tick; last_packet_id=(uint32_t)packet;
    ReleaseSRWLockExclusive(&request_lock);
    return TRUE;
}
static BOOL near_scale(float a,float b) {
    float difference=a-b; if(difference<0) difference=-difference;
    return difference<=b*0.001f;
}
static BOOL try_snap(float *record,uint64_t entity,uint64_t now) {
    if(!TryAcquireSRWLockExclusive(&request_lock)) return FALSE;
    BOOL snapped=FALSE;
    for(unsigned i=0;i<REQUEST_SLOTS;i++) {
        SnapRequest *r=&requests[i];
        if(r->entity!=entity || r->consumed || !fresh_tick(r->born,now)) continue;
        float base=(float)r->units/1000000.0f;
        float target=base*(r->direction=='I'?0.15f:1.0f);
        if(!near_scale(record[0],base)) {
            r->consumed=TRUE; InterlockedIncrement(&mismatch_count); break;
        }
        if(!near_scale(record[1],target)) {
            /* The opposite FPE target may precede status processing. Anything
               else means ownership is ambiguous: permanently discard this ID. */
            float previous=base*(r->direction=='I'?1.0f:0.15f);
            if(!near_scale(record[1],previous)) { r->consumed=TRUE; InterlockedIncrement(&mismatch_count); }
            break;
        }
        r->consumed=TRUE;
        /* This record is the engine's writable current-iteration component.
           The resumed instructions load it, update the visual and finish cleanup. */
        record[2]=record[1];
        InterlockedIncrement(&snap_count); snapped=TRUE; break;
    }
    ReleaseSRWLockExclusive(&request_lock);
    return snapped;
}
static BOOL append_path(wchar_t *out,const wchar_t *base,const wchar_t *suffix) {
    unsigned n=0,i=0;
    while(base[n]) { if(n>=MAX_PATH-1) return FALSE; out[n]=base[n]; n++; }
    while(suffix[i]) { if(n>=MAX_PATH-1) return FALSE; out[n++]=suffix[i++]; }
    out[n]=0; return TRUE;
}
static BOOL mailbox_init(void) {
    requests_clear();
    wchar_t local[MAX_PATH],directory[MAX_PATH];
    DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",local,MAX_PATH);
    if(!n || n>=MAX_PATH || !append_path(directory,local,L"\\Larian Studios\\Baldur's Gate 3\\Script Extender\\FirstPersonExplorer")) return FALSE;
    /* SE creates its parent directory. The worker retries if it is not ready. */
    CreateDirectoryW(directory,NULL);
    if(!append_path(mailbox_path,directory,L"\\instant_request_0769.txt")
       || !append_path(ticket_path,directory,L"\\instant_ticket_0769.txt")) return FALSE;
    FILETIME time; GetSystemTimeAsFileTime(&time);
    mailbox_nonce=((uint64_t)time.dwHighDateTime<<32)|time.dwLowDateTime;
    mailbox_nonce^=(uint64_t)GetCurrentProcessId()<<32;
    return TRUE;
}
static void mailbox_poll(uint64_t now) {
    static uint64_t next_ticket;
    if(now>=next_ticket) {
        char ticket[33]; put_hex(ticket,mailbox_nonce,16); put_hex(ticket+16,now,16); ticket[32]='\n';
        HANDLE f=CreateFileW(ticket_path,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        if(f!=INVALID_HANDLE_VALUE) { DWORD written; WriteFile(f,ticket,33,&written,NULL); CloseHandle(f); }
        next_ticket=now+50;
    }
    HANDLE f=CreateFileW(mailbox_path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) return;
    char data[493]; DWORD n=0;
    if(ReadFile(f,data,sizeof(data),&n,NULL)) requests_receive(data,n,now);
    CloseHandle(f);
}
