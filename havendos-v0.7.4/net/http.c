/*
 * http.c — HTTP/1.1 GET client for HavenDOS v0.6.8
 *
 * Supports:
 *   - HTTP (port 80) only — no TLS
 *   - GET requests
 *   - Content-Length and chunked Transfer-Encoding
 *   - Streaming body to memory buffer or directly to a FAT16 file
 *   - Basic progress callback
 *
 * API:
 *   int http_get(const char *url, http_resp_t *resp)
 *     Fills resp->body/body_len if resp->file_path is NULL.
 *     Writes to file if resp->file_path is set.
 *     Returns HTTP status code or <0 on error.
 */

#include "../include/http.h"
#include "../include/tls.h"
#include "../include/tcp.h"
#include "../include/dns.h"
#include "../include/net.h"
#include "../include/string.h"
#include "../include/types.h"

extern int  vfs_write(const char*, const char*, uint32_t);
extern void sleep_ms(uint32_t);
extern uint32_t rtc_read_seconds_approx(void);

#define HTTP_RECV_CHUNK  1024
static int g_use_tls = 0;   /* set per-request */
#define HTTP_MAX_HEADER  4096
#define HTTP_BODY_MAX    (512*1024)   /* 512KB max in-memory body */

/* ── URL parser ─────────────────────────────────────────────────── */
typedef struct {
    char host[128];
    char path[256];
    uint16_t port;
    int is_https;
} parsed_url_t;

static int parse_url(const char *url, parsed_url_t *out)
{
    out->port = 80;
    out->path[0] = '/'; out->path[1] = 0;
    out->is_https = 0;

    const char *p = url;
    /* strip http:// or https:// */
    if(p[0]=='h'&&p[1]=='t'&&p[2]=='t'&&p[3]=='p'){
        p+=4;
        if(p[0]=='s'){ out->is_https=1; out->port=443; p++; }
        if(p[0]==':'&&p[1]=='/'&&p[2]=='/') p+=3;
        else return -1;
    }
    /* host */
    int hi=0;
    while(*p && *p!='/' && *p!=':' && hi<127) out->host[hi++]=*p++;
    out->host[hi]=0;
    if(!hi) return -1;
    /* optional port */
    if(*p==':'){
        p++; out->port=0;
        while(*p>='0'&&*p<='9') out->port=(uint16_t)(out->port*10+(*p++)-'0');
    }
    /* path */
    if(*p=='/'){
        int pi=0;
        while(*p && pi<255) out->path[pi++]=*p++;
        out->path[pi]=0;
    }
    return 0;
}

/* ── Send HTTP GET request ──────────────────────────────────────── */
static int send_request(int fd, const char *host, const char *path)
{
    char req[512];
    int len=0;
    /* GET line */
    const char *m="GET "; for(;*m;m++) req[len++]=*m;
    for(const char *s=path;*s;s++) req[len++]=*s;
    const char *v=" HTTP/1.1\r\nHost: ";
    for(;*v;v++) req[len++]=*v;
    for(const char *s=host;*s;s++) req[len++]=*s;
    const char *c="\r\nConnection: close\r\nUser-Agent: HavenDOS/0.7.0\r\n\r\n";
    for(;*c;c++) req[len++]=*c;
    return tcp_send(fd, req, (uint16_t)len);
}

/* ── Parse response headers ─────────────────────────────────────── */
static int parse_headers(char *hbuf, int hlen,
                         int *status_out,
                         int *content_len_out,
                         int *chunked_out)
{
    *status_out = 0; *content_len_out = -1; *chunked_out = 0;
    hbuf[hlen] = 0;

    /* Status line: HTTP/1.x NNN ... */
    char *p = hbuf;
    while(*p && *p != ' ') p++;
    if(!*p) return -1;
    p++;
    *status_out = 0;
    while(*p>='0'&&*p<='9') *status_out=(*status_out)*10+(*p++)-'0';

    /* Scan header lines */
    while(*p){
        while(*p && (*p=='\r'||*p=='\n')) p++;
        if(!*p) break;
        char *line=p;
        while(*p && *p!='\r'&&*p!='\n') p++;
        char save=*p; *p=0;
        /* Content-Length */
        if(line[0]=='C'||line[0]=='c'){
            if(strncmp(line,"Content-Length:",15)==0||
               strncmp(line,"content-length:",15)==0){
                char *v=line+15; while(*v==' ')v++;
                *content_len_out=0;
                while(*v>='0'&&*v<='9') *content_len_out=(*content_len_out)*10+(*v++)-'0';
            }
        }
        /* Transfer-Encoding: chunked */
        if(line[0]=='T'||line[0]=='t'){
            if(strncmp(line,"Transfer-Encoding:",18)==0||
               strncmp(line,"transfer-encoding:",18)==0){
                char *v=line+18; while(*v==' ')v++;
                if(strncmp(v,"chunked",7)==0) *chunked_out=1;
            }
        }
        *p=save;
    }
    return 0;
}


/* HTTPS-aware receive — calls tls_recv or tcp_recv based on g_use_tls */
static int http_recv(int fd, uint8_t *buf, uint16_t len, uint32_t timeout){
    return g_use_tls ? tls_recv(fd, buf, len, timeout)
                     : tcp_recv(fd, buf, len, timeout);
}

/* ── Read chunked body ───────────────────────────────────────────── */
static int read_chunked(int fd, uint8_t *body, int maxlen,
                         const char *filepath,
                         void(*progress)(int,int))
{
    static uint8_t rbuf[HTTP_RECV_CHUNK+16];
    int total=0;
    /* temp file accumulator if writing to disk */
    static uint8_t filebuf[HTTP_BODY_MAX];
    int filelen=0;

    while(1){
        /* Read chunk size line (hex) */
        char szline[16]; int sl=0;
        while(sl<15){
            int c=http_recv(fd,(uint8_t*)szline+sl,1,3000);
            if(c<=0) break;
            if(szline[sl]=='\n'){ szline[sl]=0; break; }
            if(szline[sl]!='\r') sl++;
        }
        /* parse hex size */
        int csz=0;
        for(int i=0;szline[i];i++){
            char c=szline[i];
            if(c>='0'&&c<='9') csz=csz*16+(c-'0');
            else if(c>='a'&&c<='f') csz=csz*16+(c-'a'+10);
            else if(c>='A'&&c<='F') csz=csz*16+(c-'A'+10);
            else break;
        }
        if(csz==0) break;   /* last chunk */

        /* Read csz bytes */
        int rem=csz;
        while(rem>0){
            int want=rem<HTTP_RECV_CHUNK?rem:HTTP_RECV_CHUNK;
            int got=http_recv(fd,rbuf,(uint16_t)want,5000);
            if(got<=0) break;
            if(filepath){
                if(filelen+got<HTTP_BODY_MAX){
                    memcpy(filebuf+filelen,rbuf,got); filelen+=got;
                }
            } else {
                if(body && total+got<maxlen){
                    memcpy(body+total,rbuf,got);
                }
            }
            total+=got; rem-=got;
            if(progress) progress(total,0);
        }
        /* skip trailing CRLF */
        http_recv(fd,rbuf,2,1000);
    }
    if(filepath && filelen>0)
        vfs_write(filepath,(const char*)filebuf,(uint32_t)filelen);
    return total;
}

/* ── Read fixed-length body ─────────────────────────────────────── */
static int read_body(int fd, int content_len,
                      uint8_t *body, int maxlen,
                      const char *filepath,
                      void(*progress)(int,int))
{
    static uint8_t rbuf[HTTP_RECV_CHUNK];
    static uint8_t filebuf[HTTP_BODY_MAX];
    int total=0, filelen=0;
    int limit = (content_len>0) ? content_len : HTTP_BODY_MAX;

    while(!tcp_eof(0) && total<limit){
        int want=HTTP_RECV_CHUNK;
        if(content_len>0 && limit-total<want) want=limit-total;
        int got=http_recv(fd,rbuf,(uint16_t)want,5000);
        if(got<=0) break;
        if(filepath){
            if(filelen+got<HTTP_BODY_MAX){
                memcpy(filebuf+filelen,rbuf,got); filelen+=got;
            }
        } else {
            if(body && total+got<maxlen)
                memcpy(body+total,rbuf,got);
        }
        total+=got;
        if(progress) progress(total, content_len);
    }
    if(filepath && filelen>0)
        vfs_write(filepath,(const char*)filebuf,(uint32_t)filelen);
    return total;
}

/* ── Public: http_get ───────────────────────────────────────────── */
/* Internal: depth-limited http_get to prevent redirect stack overflow */
static int http_get_internal(const char *url, http_resp_t *resp, int depth);

int http_get(const char *url, http_resp_t *resp)
{
    return http_get_internal(url, resp, 0);
}

static int http_get_internal(const char *url, http_resp_t *resp, int depth)
{
    parsed_url_t pu;
    if(parse_url(url, &pu) < 0) return HTTP_ERR_URL;

    /* DNS resolve */
    uint32_t ip = dns_resolve(pu.host);
    if(!ip) return HTTP_ERR_DNS;

    /* TCP connect */
    int fd = tcp_connect(ip, pu.port);
    if(fd < 0) return HTTP_ERR_CONNECT;

    /* TLS handshake for HTTPS */
    g_use_tls = pu.is_https;
    if(pu.is_https){
        /* Use millisecond counter as seed — no RTC needed */
        static uint32_t tls_seed = 0xA1B2C3D4u;
        tls_seed ^= (tls_seed<<7)^(tls_seed>>5)^(uint32_t)(size_t)pu.host;
        int tls_err = tls_connect(fd, pu.host, tls_seed);
        if(tls_err < 0){
            tls_close(fd);
            return HTTP_ERR_TLS;
        }
    }

    /* Send request */
    if(pu.is_https){
        /* Build request string then send via TLS */
        char req[512]; int len=0;
        const char *m="GET "; for(;*m;m++) req[len++]=*m;
        for(const char *s=pu.path;*s;s++) req[len++]=*s;
        const char *v=" HTTP/1.1\r\nHost: ";
        for(;*v;v++) req[len++]=*v;
        for(const char *s=pu.host;*s;s++) req[len++]=*s;
        const char *c="\r\nConnection: close\r\nUser-Agent: HavenDOS/0.7.0\r\n\r\n";
        for(;*c;c++) req[len++]=*c;
        if(tls_send(fd,(const uint8_t*)req,(uint16_t)len)<0){
            tls_close(fd); return HTTP_ERR_SEND;
        }
    } else if(send_request(fd, pu.host, pu.path) < 0){
        tcp_close(fd); return HTTP_ERR_SEND;
    }

    /* Read response headers into buffer */
    static char hbuf[HTTP_MAX_HEADER];
    int hlen=0;
    /* Read until we see \r\n\r\n */
    static uint8_t tbuf[1];
    int prev3=0, prev2=0, prev1=0;
    while(hlen < HTTP_MAX_HEADER-1){
        int r=pu.is_https ? tls_recv(fd,tbuf,1,10000)
                           : tcp_recv(fd,tbuf,1,10000);
        if(r<=0) break;
        hbuf[hlen++]=(char)tbuf[0];
        /* Detect end of headers */
        if(prev3=='\r'&&prev2=='\n'&&prev1=='\r'&&tbuf[0]=='\n') break;
        prev3=prev2; prev2=prev1; prev1=tbuf[0];
    }
    hbuf[hlen]=0;

    int status=0, content_len=-1, chunked=0;
    parse_headers(hbuf, hlen, &status, &content_len, &chunked);

    resp->status = status;
    resp->body_len = 0;

    /* Handle redirects (301/302) — one hop */
    if((status==301||status==302) && resp->follow_redirect){
        /* find Location: header */
        char *loc=hbuf;
        while(*loc){
            if(strncmp(loc,"Location:",9)==0||strncmp(loc,"location:",9)==0){
                loc+=9; while(*loc==' ')loc++;
                char newurl[256]; int ui=0;
                while(*loc&&*loc!='\r'&&*loc!='\n'&&ui<255)
                    newurl[ui++]=*loc++;
                newurl[ui]=0;
                tcp_close(fd);
                /* BUG FIX: cap redirect depth at 3 to prevent stack overflow */
                if(depth >= 3) return HTTP_ERR_URL;
                /* Follow redirect — HTTPS now supported via TLS */
                return http_get_internal(newurl, resp, depth + 1);
            }
            while(*loc&&*loc!='\n')loc++; if(*loc)loc++;
        }
    }

    /* Read body */
    int body_len=0;
    if(chunked)
        body_len=read_chunked(fd, resp->body, resp->body_max,
                              resp->file_path, resp->progress);
    else
        body_len=read_body(fd, content_len,
                           resp->body, resp->body_max,
                           resp->file_path, resp->progress);

    resp->body_len = body_len;
    if(resp->body && body_len < resp->body_max)
        resp->body[body_len] = 0;   /* null-terminate */

    if(pu.is_https) tls_close(fd);
    else            tcp_close(fd);
    return status;
}
