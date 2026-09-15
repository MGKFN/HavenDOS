#include "../include/string.h"
#include "../include/types.h"

void *memset(void *dst, int c, size_t n){
    uint8_t *p=dst; while(n--)*p++=c; return dst;
}
void *memcpy(void *dst, const void *src, size_t n){
    uint8_t *d=dst; const uint8_t *s=src; while(n--)*d++=*s++; return dst;
}
int memcmp(const void *a, const void *b, size_t n){
    const uint8_t *p=a,*q=b; while(n--){if(*p!=*q)return *p-*q; p++;q++;} return 0;
}
size_t strlen(const char *s){ size_t n=0; while(*s++)n++; return n; }
int strcmp(const char *a, const char *b){
    while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n){
    while(n--&&*a&&*a==*b){a++;b++;} if(!n)return 0; return (unsigned char)*a-(unsigned char)*b;
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
void itoa(int val, char *buf, int base){
    char tmp[32]; int i=0,neg=0;
    if(val<0&&base==10){neg=1;val=-val;}
    if(val==0){buf[0]='0';buf[1]=0;return;}
    while(val){tmp[i++]="0123456789abcdef"[val%base];val/=base;}
    if(neg)tmp[i++]='-';
    int j=0; while(i--)buf[j++]=tmp[i]; buf[j]=0;
}
void utoa(uint32_t val, char *buf, int base){
    char tmp[32]; int i=0;
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
