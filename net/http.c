/*
 * http.c — HTTP/1.1 GET client for HavenDOS v0.7.11
 *
 * Supports:
 *   - HTTP (port 80) and HTTPS (port 443, TLS 1.2)
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
#include "pit.h"   /* v0.8.8.8: was a hand-copied extern */

extern int  vfs_write(const char*, const char*, uint32_t);
extern uint32_t rtc_read_seconds_approx(void);

#define HTTP_RECV_CHUNK  1024
static int g_use_tls = 0;   /* set per-request */
#define HTTP_MAX_HEADER  4096
#define HTTP_BODY_MAX    (512*1024)   /* 512KB max in-memory body */

/* v0.7.6: read_chunked() and read_body() each carried their own private
   static uint8_t filebuf[512*1024].  Only one is ever live at a time (a
   response is either chunked or length-delimited, never both), so they now
   share one staging buffer.  Saves 512 KB of .bss outright. */
/* v0.7.18 — was a permanent 512 KB BSS reservation for a download staging
   area used only while a transfer is in flight.  Taken from the physical
   allocator per request now; a machine that never downloads never pays.
   Every use is behind a NULL check because the allocation can fail. */
extern void *pmm_alloc_contig(uint32_t nblocks);
extern void  pmm_free_contig(void *p, uint32_t nblocks);
#define FILEBUF_BLOCKS (HTTP_BODY_MAX / 4096)
static uint8_t *g_filebuf = 0;

const char *http_strerror(int err)
{
    switch(err){
        case HTTP_ERR_URL:      return "bad URL";
        case HTTP_ERR_DNS:      return "DNS lookup failed";
        case HTTP_ERR_CONNECT:  return "TCP connect failed";
        case HTTP_ERR_TLS:      return "TLS handshake failed";
        case HTTP_ERR_SEND:     return "send failed";
        case HTTP_ERR_RECV:     return "receive timeout";
        case HTTP_ERR_REDIRECT: return "too many redirects";
        case HTTP_ERR_NOMEM:    return "not enough memory to stage the download";
        default:                return "unknown error";
    }
}

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
    /* v0.7.5: 640 bytes with explicit bounds.  The old 512-byte buffer had
       ~59 bytes of headroom over the worst-case path(255)+host(127) and no
       bounds check at all. */
    char req[640];
    const int cap = (int)sizeof(req);
    int len=0;
    #define PUT(str) for(const char *_q=(str);*_q&&len<cap-1;_q++) req[len++]=*_q
    /* GET line */
    PUT("GET "); PUT(path); PUT(" HTTP/1.1\r\nHost: "); PUT(host);
    PUT("\r\nConnection: close\r\nUser-Agent: HavenDOS/0.7.11\r\n\r\n");
    #undef PUT
    return tcp_send(fd, req, (uint16_t)len);
}

/* ── Parse response headers ─────────────────────────────────────── */
/* v0.7.12: header field names are case-INSENSITIVE (RFC 7230 3.2).  The old
   code tested exactly two spellings per header, so a server sending
   "Content-length:" or "TRANSFER-ENCODING:" — both legal, and both seen in the
   wild — fell through as if the header were absent: an unknown body length,
   which then hit the timeout path in read_body(). */
static int hdr_is(const char *line, const char *name){
    while(*name){
        char a=*line++, b=*name++;
        if(a>='A'&&a<='Z') a+=32;
        if(b>='A'&&b<='Z') b+=32;
        if(a!=b) return 0;
    }
    return 1;
}

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
        if(hdr_is(line,"content-length:")){
            char *v=line+15; while(*v==' ')v++;
            *content_len_out=0;
            while(*v>='0'&&*v<='9') *content_len_out=(*content_len_out)*10+(*v++)-'0';
        }
        /* Transfer-Encoding: chunked */
        else if(hdr_is(line,"transfer-encoding:")){
            char *v=line+18; while(*v==' ')v++;
            if(hdr_is(v,"chunked")) *chunked_out=1;
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
static int http_eof(int fd){
    return g_use_tls ? tls_eof(fd) : tcp_eof(fd);
}

/* ── Read chunked body ───────────────────────────────────────────── */
/* v0.7.6 fixes in this function:
     - the chunk-size line was never NUL-terminated when it hit the 15-char
       cap, so the hex parser walked off the end of szline[]
     - `total` counted bytes that were dropped because body[] was full,
       exactly the bug that was fixed in read_body() for v0.7.5 but missed here
     - no end-of-stream check, so a server that died mid-body span the
       3-second per-byte timeout forever
     - chunk extensions (`1a3;name=value`) are now skipped explicitly       */
static int read_chunked(int fd, uint8_t *body, int maxlen,
                         const char *filepath,
                         void(*progress)(int,int))
{
    static uint8_t rbuf[HTTP_RECV_CHUNK+16];
    int total=0;
    int filelen=0;
    int guard=0;
    /* Spooling to disk needs the staging buffer; http_get refuses the request
       up front when it could not be obtained, so this is belt and braces. */
    if(!g_filebuf) filepath = 0;

    while(1){
        if(++guard > 65536) break;            /* pathological chunk storm */
        if(http_eof(fd)) break;

        /* Read the chunk-size line (hex, optionally with ;extensions) */
        char szline[24]; int sl=0; int sawnl=0;
        while(sl<(int)sizeof(szline)-1){
            int c=http_recv(fd,(uint8_t*)szline+sl,1,3000);
            if(c<=0) break;
            if(szline[sl]=='\n'){ sawnl=1; break; }
            if(szline[sl]!='\r') sl++;
        }
        szline[sl]=0;                          /* v0.7.6: always terminate */
        if(!sawnl && sl==0) break;             /* stream ended */

        /* parse hex size, stopping at ';' (chunk extension) or junk */
        int csz=0, digits=0;
        for(int i=0;szline[i];i++){
            char c=szline[i];
            if(c>='0'&&c<='9')      { csz=csz*16+(c-'0');      digits++; }
            else if(c>='a'&&c<='f') { csz=csz*16+(c-'a'+10);   digits++; }
            else if(c>='A'&&c<='F') { csz=csz*16+(c-'A'+10);   digits++; }
            else break;                        /* ';' or CR — stop */
        }
        if(!digits) break;                     /* malformed size line */
        if(csz<=0) break;                      /* last chunk (0) */
        if(csz>HTTP_BODY_MAX) break;           /* absurd — bail out */

        /* Read csz bytes of chunk data */
        int rem=csz;
        while(rem>0){
            int want=rem<HTTP_RECV_CHUNK?rem:HTTP_RECV_CHUNK;
            int got=http_recv(fd,rbuf,(uint16_t)want,5000);
            if(got<=0){ rem=0; guard=70000; break; }   /* force outer break */
            if(filepath&&filepath[0]){
                if(filelen+got<HTTP_BODY_MAX){
                    memcpy(g_filebuf+filelen,rbuf,got); filelen+=got;
                } else { rem=0; guard=70000; break; }  /* staging full */
            } else if(body){
                /* v0.7.6: only count what we actually stored */
                if(total+got<maxlen) memcpy(body+total,rbuf,got);
                else { total=maxlen>0?maxlen-1:0; rem=0; guard=70000; break; }
            }
            total+=got; rem-=got;
            if(progress) progress(total,0);
        }
        if(guard>=70000) break;

        /* skip the CRLF that terminates the chunk */
        http_recv(fd,rbuf,2,1000);
    }
    if(filepath&&filepath[0]&&filelen>0){
        vfs_write(filepath,(const char*)g_filebuf,(uint32_t)filelen);
        total=filelen;
    }
    return total;
}

/* ── Read fixed-length body ─────────────────────────────────────── */
static int read_body(int fd, int content_len,
                      uint8_t *body, int maxlen,
                      const char *filepath,
                      void(*progress)(int,int))
{
    static uint8_t rbuf[HTTP_RECV_CHUNK];
    int total=0, filelen=0;
    const int to_file = (filepath && filepath[0] && g_filebuf);

    /* v0.7.12 FIX — `content_len>0` treated an explicit "Content-Length: 0"
       exactly like a missing header, so the loop below sat there reading until
       http_recv() timed out.  Five seconds of dead air on every 204, every 304
       and every legitimately empty 200.  A declared length of zero means the
       body is complete right now. */
    if(content_len == 0) return 0;

    int limit = (content_len>0) ? content_len : HTTP_BODY_MAX;

    /* v0.7.5 FIX: was tcp_eof(0) unconditionally.  Under HTTPS the TCP FIN
       arrives while tls_recv() still has decrypted plaintext buffered, so
       the loop exited early and truncated every HTTPS body. */
    while(!http_eof(fd) && total<limit){
        int want=HTTP_RECV_CHUNK;
        if(content_len>0 && limit-total<want) want=limit-total;
        int got=http_recv(fd,rbuf,(uint16_t)want,5000);
        if(got<=0) break;
        if(to_file){
            if(filelen+got<HTTP_BODY_MAX){
                memcpy(g_filebuf+filelen,rbuf,got); filelen+=got;
            } else break;   /* v0.7.6: staging buffer full — stop cleanly */
        } else {
            /* v0.7.5 FIX: the old code did total+=got even when the body
               buffer was full and the memcpy was skipped, so body_len
               reported bytes that were never written and callers read
               uninitialised memory. */
            if(body && total+got<maxlen) memcpy(body+total,rbuf,got);
            else if(body) { total=maxlen-1; break; }
        }
        total+=got;
        if(progress) progress(total, content_len);
    }
    if(to_file && filelen>0){
        vfs_write(filepath,(const char*)g_filebuf,(uint32_t)filelen);
        total=filelen;
    }
    return total;
}

/* ── Public: http_get ───────────────────────────────────────────── */
/* Internal: depth-limited http_get to prevent redirect stack overflow */
static int http_get_internal(const char *url, http_resp_t *resp, int depth);

int http_get(const char *url, http_resp_t *resp)
{
    /* The staging buffer lives exactly as long as the request. Redirects
       recurse inside http_get_internal, so it is acquired out here once. */
    int owned = 0;
    if(!g_filebuf){
        g_filebuf = (uint8_t *)pmm_alloc_contig(FILEBUF_BLOCKS);
        owned = (g_filebuf != 0);
    }
    /* Say so rather than quietly returning a truncated or empty file. */
    if(!g_filebuf && resp && resp->file_path && resp->file_path[0])
        return HTTP_ERR_NOMEM;
    int r = http_get_internal(url, resp, 0);
    if(owned){ pmm_free_contig(g_filebuf, FILEBUF_BLOCKS); g_filebuf = 0; }
    return r;
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
        char req[640]; const int cap=(int)sizeof(req); int len=0;
        #define PUT(str) for(const char *_q=(str);*_q&&len<cap-1;_q++) req[len++]=*_q
        PUT("GET "); PUT(pu.path); PUT(" HTTP/1.1\r\nHost: "); PUT(pu.host);
        PUT("\r\nConnection: close\r\nUser-Agent: HavenDOS/0.7.11\r\n\r\n");
        #undef PUT
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

    /* Handle redirects.
       v0.7.6: 303 (See Other), 307 (Temporary) and 308 (Permanent) were not
       recognised at all — the body of the redirect page was returned as if it
       were the resource.  All five status codes are handled now. */
    if((status==301||status==302||status==303||status==307||status==308)
       && resp->follow_redirect){
        /* find Location: header */
        char *loc=hbuf;
        while(*loc){
            if(hdr_is(loc,"location:")){
                loc+=9; while(*loc==' ')loc++;
                char newurl[256]; int ui=0;
                /* v0.7.5 FIX: relative Location values ("/path") used to be
                   handed straight to parse_url(), which requires a host and
                   returned HTTP_ERR_URL.  Rebuild an absolute URL first. */
                if(*loc=='/'){
                    const char *sch = pu.is_https ? "https://" : "http://";
                    for(const char *q=sch;*q&&ui<255;q++) newurl[ui++]=*q;
                    for(const char *q=pu.host;*q&&ui<255;q++) newurl[ui++]=*q;
                }
                while(*loc&&*loc!='\r'&&*loc!='\n'&&ui<255)
                    newurl[ui++]=*loc++;
                newurl[ui]=0;
                if(pu.is_https) tls_close(fd); else tcp_close(fd);
                /* Cap redirect depth to prevent stack overflow / loops */
                if(depth >= 4) return HTTP_ERR_REDIRECT;
                if(!newurl[0])  return HTTP_ERR_URL;
                return http_get_internal(newurl, resp, depth + 1);
            }
            while(*loc&&*loc!='\n')loc++;
            if(*loc)loc++;
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
    if(resp->body && body_len >= 0 && body_len < resp->body_max)
        resp->body[body_len] = 0;   /* null-terminate */

    if(pu.is_https) tls_close(fd);
    else            tcp_close(fd);
    return status;
}
