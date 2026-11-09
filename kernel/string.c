#include "../include/string.h"
#include "../include/types.h"

void *memset(void *dst, int c, size_t n){
    uint8_t *p=dst; while(n--)*p++=c; return dst;
}
void *memcpy(void *dst, const void *src, size_t n){
    uint8_t *d=dst; const uint8_t *s=src; while(n--)*d++=*s++; return dst;
}
/* v0.7.12: memmove was missing entirely.  memcpy() here copies strictly
   forward, so any caller shifting a buffer down over itself (ring compaction,
   list deletion, protocol reassembly) had to hand-roll a loop or corrupt its
   own data.  GCC can also synthesise a memmove() call on its own for a
   struct assignment, which would have failed to link. */
void *memmove(void *dst, const void *src, size_t n){
    uint8_t *d=dst; const uint8_t *s=src;
    if(d==s||n==0) return dst;
    if(d<s){ while(n--)*d++=*s++; }
    else   { d+=n; s+=n; while(n--)*--d=*--s; }
    return dst;
}
int memcmp(const void *a, const void *b, size_t n){
    const uint8_t *p=a,*q=b; while(n--){if(*p!=*q)return *p-*q; p++;q++;} return 0;
}
size_t strlen(const char *s){ size_t n=0; while(*s++)n++; return n; }
int strcmp(const char *a, const char *b){
    while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
/* v0.7.5 CRITICAL FIX:
 * Old body was:
 *   while(n--&&*a&&*a==*b){a++;b++;} if(!n)return 0; return *a-*b;
 * When the loop ran to completion, the final `n--` wrapped n from 0 to
 * SIZE_MAX, so `if(!n)` was never true and the function compared the
 * character AT index n — one past the compare window.  Result: strncmp()
 * only ever returned 0 for fully-equal strings.  Every prefix test in the
 * OS silently failed (kernel cmdline flags, tab-complete, if/while in
 * scripts, HTTP Content-Length / Location / Transfer-Encoding, ls in
 * subdirectories, grep).  Now decrements only when a character is
 * actually consumed. */
int strncmp(const char *a, const char *b, size_t n){
    while(n && *a && *a==*b){ a++; b++; n--; }
    if(n==0) return 0;
    return (unsigned char)*a-(unsigned char)*b;
}
char *strcpy(char *dst, const char *src){ char *r=dst; while((*dst++=*src++)); return r; }
char *strncpy(char *dst, const char *src, size_t n){
    char *r=dst; while(n&&(*dst++=*src++))n--; while(n--)*dst++=0; return r;
}
char *strcat(char *dst, const char *src){ char *r=dst; while(*dst)dst++; while((*dst++=*src++)); return r; }
char *strncat(char *dst, const char *src, uint32_t n){ char *r=dst; while(*dst)dst++; while(n--&&*src)*dst++=*src++; *dst=0; return r; }
char *strchr(const char *s, int c){ while(*s){if(*s==(char)c)return(char*)s; s++;} return NULL; }
char *strstr(const char *hay, const char *needle){
    if(!*needle) return (char*)hay;
    for(;*hay;hay++){
        const char *h=hay,*n=needle;
        while(*h&&*n&&*h==*n){h++;n++;}
        if(!*n) return (char*)hay;
    }
    return NULL;
}

static char *strtok_ptr=NULL;
char *strtok(char *str, const char *delim){
    if(str)strtok_ptr=str;
    if(!strtok_ptr)return NULL;
    while(*strtok_ptr&&strchr(delim,*strtok_ptr))strtok_ptr++;
    if(!*strtok_ptr)return NULL;
    char *start=strtok_ptr;
    while(*strtok_ptr&&!strchr(delim,*strtok_ptr))strtok_ptr++;
    if(*strtok_ptr)*strtok_ptr++=0;
    return start;
}
/* v0.7.12 FIX — two out-of-bounds reads of the digit table.
   (a) `val = -val` is undefined for INT_MIN and leaves it negative, so
       `val % base` is negative and indexes before the string literal.
   (b) A negative value with base != 10 skipped the sign handling entirely and
       hit the same negative modulus.  vga_printf("%x", someNegativeInt) was
       enough to trigger it.
   Work in unsigned throughout. */
void itoa(int val, char *buf, int base){
    char tmp[36]; int i=0,neg=0;
    uint32_t v;
    if(base<2||base>16){ buf[0]=0; return; }
    if(val<0&&base==10){ neg=1; v=(uint32_t)(-(int64_t)val); }
    else                 v=(uint32_t)val;
    if(v==0){buf[0]='0';buf[1]=0;return;}
    while(v){tmp[i++]="0123456789abcdef"[v%(uint32_t)base];v/=(uint32_t)base;}
    if(neg)tmp[i++]='-';
    int j=0; while(i--)buf[j++]=tmp[i]; buf[j]=0;
}
void utoa(uint32_t val, char *buf, int base){
    char tmp[36]; int i=0;
    if(base<2||base>16){ buf[0]=0; return; }
    if(val==0){buf[0]='0';buf[1]=0;return;}
    while(val){tmp[i++]="0123456789abcdef"[val%base];val/=base;}
    int j=0; while(i--)buf[j++]=tmp[i]; buf[j]=0;
}
int atoi(const char *s){
    int n=0,neg=0; if(*s=='-'){neg=1;s++;} while(*s>='0'&&*s<='9')n=n*10+(*s++)-'0'; return neg?-n:n;
}
void strupr(char *s){ while(*s){if(*s>='a'&&*s<='z')*s-=32; s++;} }
void strlwr(char *s){ while(*s){if(*s>='A'&&*s<='Z')*s+=32; s++;} }
int strstartwith(const char *s, const char *prefix){ return strncmp(s,prefix,strlen(prefix))==0; }
