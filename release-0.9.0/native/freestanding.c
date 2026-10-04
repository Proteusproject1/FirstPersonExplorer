/* Minimal memory helpers for the static MinHook dependency; no CRT startup. */
#include <stddef.h>
void *memcpy(void *restrict dst,const void *restrict src,size_t n) {
    unsigned char *d=dst; const unsigned char *s=src;
    for(size_t i=0;i<n;i++) d[i]=s[i];
    return dst;
}
void *memset(void *dst,int value,size_t n) {
    unsigned char *d=dst; for(size_t i=0;i<n;i++) d[i]=(unsigned char)value;
    return dst;
}
int memcmp(const void *a,const void *b,size_t n) {
    const unsigned char *x=a,*y=b;
    for(size_t i=0;i<n;i++) if(x[i]!=y[i]) return x[i]<y[i]?-1:1;
    return 0;
}
