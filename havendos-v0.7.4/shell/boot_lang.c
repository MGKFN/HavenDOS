/*
 * boot_lang.c — TechHaven H Language v0.2.0 (HavenCode)
 * HavenDOS v0.7.0
 *
 * New in v0.2.0:
 *   readfile path var   — read file contents into a string variable
 *   writefile path val  — write a string value to a file
 *   exists path var     — set var to 1 if file exists, 0 otherwise
 *
 * BOOT is a simple imperative language for HavenDOS scripting.
 *
 * Syntax overview:
 *   var x = 10
 *   var name = "hello"
 *   print "text"
 *   println x
 *   x = x + 1
 *   if x > 5 then ... end
 *   while x < 10 do ... end
 *   input x
 *   rem this is a comment
 *
 * Types: integer, string (stored in same slot, int flag distinguishes)
 * Operators: + - * / % == != < > <= >=
 * Built-ins: print, println, input, clear, wait, abs, len
 */

#include "../include/string.h"
#include "../include/types.h"
#include "../include/vga.h"

extern void sleep_ms(uint32_t);
extern int  keyboard_waitchar(void);
extern int  vfs_read(const char*, char*, uint32_t);
extern int  vfs_write(const char*, const char*, uint32_t);
extern int  vfs_exists(const char*);
extern void utoa(uint32_t, char*, int);
extern void itoa(int, char*, int);

/* ================================================================
   LEXER
   ================================================================ */

typedef enum {
    TK_EOF=0, TK_NEWLINE,
    TK_NUM, TK_STR, TK_IDENT,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PERCENT,
    TK_EQ, TK_NEQ, TK_LT, TK_GT, TK_LE, TK_GE,
    TK_ASSIGN,
    /* keywords */
    TK_VAR, TK_PRINT, TK_PRINTLN, TK_INPUT,
    TK_IF, TK_THEN, TK_ELSE, TK_END,
    TK_WHILE, TK_DO,
    TK_REM, TK_CLEAR, TK_WAIT,
    TK_AND, TK_OR, TK_NOT,
    TK_ABS, TK_LEN, TK_STR_KW,
    TK_STOP,
    TK_READFILE, TK_WRITEFILE, TK_EXISTS,
} tok_t;

#define BOOT_SRC_MAX  8192
#define BOOT_VARS     32
#define BOOT_STRPOOL  4096
#define BOOT_STRVAL   64

static char g_src[BOOT_SRC_MAX];
static int  g_src_len;
static int  g_pos;           /* current char position */
static int  g_line;

/* current token */
static tok_t g_tok;
static int   g_tok_int;
static char  g_tok_str[BOOT_STRVAL];

/* string pool for runtime string values */
static char  g_strpool[BOOT_STRPOOL];
static int   g_strpool_pos = 0;

/* variable storage */
typedef struct {
    char name[24];
    int  is_str;   /* 0=int, 1=string */
    int  ival;
    char sval[BOOT_STRVAL];
} boot_var_t;

static boot_var_t g_vars[BOOT_VARS];
static int        g_var_count = 0;

/* error state */
static int  g_error = 0;
static char g_errmsg[80];

static void boot_err(const char *msg){
    g_error = 1;
    strncpy(g_errmsg, msg, 79);
}

/* ── Lexer helpers ─────────────────────────────────────────────── */

static char lc(void){ return g_pos < g_src_len ? g_src[g_pos] : 0; }
static void adv(void){ if(g_pos < g_src_len) g_pos++; }

static int is_alpha(char c){ return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'; }
static int is_digit(char c){ return c>='0'&&c<='9'; }
static int is_alnum(char c){ return is_alpha(c)||is_digit(c); }

static void skip_ws(void){
    while(lc()==' '||lc()=='\t') adv();
}

static void next_token(void){
    if(g_error){ g_tok=TK_EOF; return; }
restart:
    skip_ws();
    char c = lc();
    if(!c){ g_tok=TK_EOF; return; }

    /* newline */
    if(c=='\n'||c=='\r'){
        while(lc()=='\n'||lc()=='\r'){ if(lc()=='\n') g_line++; adv(); }
        g_tok=TK_NEWLINE; return;
    }

    /* comment: rem ... or # ... */
    if(c=='#'){ while(lc()&&lc()!='\n') adv(); goto restart; }

    /* string literal */
    if(c=='"'){
        adv(); int i=0;
        while(lc()&&lc()!='"'&&i<BOOT_STRVAL-1){
            g_tok_str[i++]=lc(); adv();
        }
        g_tok_str[i]=0;
        if(lc()=='"') adv();
        g_tok=TK_STR; return;
    }

    /* number */
    if(is_digit(c)||(c=='-'&&is_digit(g_pos+1<g_src_len?g_src[g_pos+1]:0))){
        int neg=0;
        if(c=='-'){ neg=1; adv(); }
        int v=0;
        while(is_digit(lc())){ v=v*10+(lc()-'0'); adv(); }
        g_tok_int = neg?-v:v;
        g_tok=TK_NUM; return;
    }

    /* operators */
    if(c=='+'){ adv(); g_tok=TK_PLUS;  return; }
    if(c=='-'){ adv(); g_tok=TK_MINUS; return; }
    if(c=='*'){ adv(); g_tok=TK_STAR;  return; }
    if(c=='/'){ adv(); g_tok=TK_SLASH; return; }
    if(c=='%'){ adv(); g_tok=TK_PERCENT; return; }
    if(c=='='){ adv();
        if(lc()=='='){ adv(); g_tok=TK_EQ; }
        else g_tok=TK_ASSIGN;
        return;
    }
    if(c=='!'){ adv();
        if(lc()=='='){ adv(); g_tok=TK_NEQ; }
        else boot_err("expected != "); return;
    }
    if(c=='<'){ adv();
        if(lc()=='='){ adv(); g_tok=TK_LE; }
        else g_tok=TK_LT;
        return;
    }
    if(c=='>'){ adv();
        if(lc()=='='){ adv(); g_tok=TK_GE; }
        else g_tok=TK_GT;
        return;
    }

    /* identifier / keyword */
    if(is_alpha(c)){
        int i=0;
        while(is_alnum(lc())&&i<23){ g_tok_str[i++]=lc(); adv(); }
        g_tok_str[i]=0;
        /* keyword table */
        if(strcmp(g_tok_str,"var")==0)     { g_tok=TK_VAR;     return; }
        if(strcmp(g_tok_str,"print")==0)   { g_tok=TK_PRINT;   return; }
        if(strcmp(g_tok_str,"println")==0) { g_tok=TK_PRINTLN; return; }
        if(strcmp(g_tok_str,"input")==0)   { g_tok=TK_INPUT;   return; }
        if(strcmp(g_tok_str,"if")==0)      { g_tok=TK_IF;      return; }
        if(strcmp(g_tok_str,"then")==0)    { g_tok=TK_THEN;    return; }
        if(strcmp(g_tok_str,"else")==0)    { g_tok=TK_ELSE;    return; }
        if(strcmp(g_tok_str,"end")==0)     { g_tok=TK_END;     return; }
        if(strcmp(g_tok_str,"while")==0)   { g_tok=TK_WHILE;   return; }
        if(strcmp(g_tok_str,"do")==0)      { g_tok=TK_DO;      return; }
        if(strcmp(g_tok_str,"rem")==0)     { while(lc()&&lc()!='\n')adv(); g_tok=TK_NEWLINE; return; }
        if(strcmp(g_tok_str,"clear")==0)   { g_tok=TK_CLEAR;   return; }
        if(strcmp(g_tok_str,"wait")==0)    { g_tok=TK_WAIT;    return; }
        if(strcmp(g_tok_str,"and")==0)     { g_tok=TK_AND;     return; }
        if(strcmp(g_tok_str,"or")==0)      { g_tok=TK_OR;      return; }
        if(strcmp(g_tok_str,"not")==0)     { g_tok=TK_NOT;     return; }
        if(strcmp(g_tok_str,"abs")==0)     { g_tok=TK_ABS;     return; }
        if(strcmp(g_tok_str,"len")==0)     { g_tok=TK_LEN;     return; }
        if(strcmp(g_tok_str,"str")==0)     { g_tok=TK_STR_KW;  return; }
        if(strcmp(g_tok_str,"stop")==0)     { g_tok=TK_STOP;     return; }
        if(strcmp(g_tok_str,"readfile")==0) { g_tok=TK_READFILE;  return; }
        if(strcmp(g_tok_str,"writefile")==0){ g_tok=TK_WRITEFILE; return; }
        if(strcmp(g_tok_str,"exists")==0)   { g_tok=TK_EXISTS;    return; }
        g_tok=TK_IDENT; return;
    }

    /* unknown — skip */
    adv(); goto restart;
}

/* ── Variable lookup/set ──────────────────────────────────────── */

static boot_var_t *var_find(const char *name){
    for(int i=0;i<g_var_count;i++)
        if(strcmp(g_vars[i].name,name)==0) return &g_vars[i];
    return 0;
}

static boot_var_t *var_get_or_create(const char *name){
    boot_var_t *v = var_find(name);
    if(v) return v;
    if(g_var_count>=BOOT_VARS){ boot_err("too many variables"); return 0; }
    v = &g_vars[g_var_count++];
    strncpy(v->name,name,23); v->name[23]=0;
    v->is_str=0; v->ival=0; v->sval[0]=0;
    return v;
}

/* ================================================================
   PARSER / EVALUATOR  (single-pass recursive descent)
   ================================================================ */

/* forward decls */
static void parse_stmts(int stop_on_else);
static int  parse_expr_int(void);
static void parse_expr_str(char *out, int maxlen);
static int  parse_cond(void);

/* skip newlines between tokens */
static void skip_nl(void){
    while(g_tok==TK_NEWLINE && !g_error) next_token();
}

/* ── Expression: int value ─────────────────────────────────────── */

static int parse_primary_int(void){
    if(g_tok==TK_NUM){ int v=g_tok_int; next_token(); return v; }
    if(g_tok==TK_MINUS){ next_token(); return -parse_primary_int(); }
    if(g_tok==TK_ABS){
        next_token();
        int v=parse_primary_int();
        return v<0?-v:v;
    }
    if(g_tok==TK_LEN){
        next_token();
        /* len needs a string var */
        if(g_tok==TK_IDENT){
            boot_var_t *v=var_find(g_tok_str);
            next_token();
            if(v&&v->is_str) return (int)strlen(v->sval);
            return 0;
        }
        return 0;
    }
    if(g_tok==TK_IDENT){
        boot_var_t *v=var_find(g_tok_str);
        next_token();
        if(v&&!v->is_str) return v->ival;
        /* string var used in int context: return length */
        if(v&&v->is_str) return (int)strlen(v->sval);
        return 0;
    }
    next_token(); return 0;
}

static int parse_term_int(void){
    int v=parse_primary_int();
    while(!g_error){
        if(g_tok==TK_STAR){ next_token(); int r=parse_primary_int(); v*=r; }
        else if(g_tok==TK_SLASH){ next_token(); int r=parse_primary_int(); v=r?v/r:0; }
        else if(g_tok==TK_PERCENT){ next_token(); int r=parse_primary_int(); v=r?v%r:0; }
        else break;
    }
    return v;
}

static int parse_expr_int(void){
    int v=parse_term_int();
    while(!g_error){
        if(g_tok==TK_PLUS){ next_token(); v+=parse_term_int(); }
        else if(g_tok==TK_MINUS){ next_token(); v-=parse_term_int(); }
        else break;
    }
    return v;
}

/* ── Expression: string value ──────────────────────────────────── */

static void parse_expr_str(char *out, int maxlen){
    out[0]=0;
    if(g_tok==TK_STR){
        strncpy(out,g_tok_str,maxlen-1); out[maxlen-1]=0;
        next_token(); return;
    }
    if(g_tok==TK_IDENT){
        boot_var_t *v=var_find(g_tok_str);
        next_token();
        if(v&&v->is_str){ strncpy(out,v->sval,maxlen-1); out[maxlen-1]=0; return; }
        if(v&&!v->is_str){ itoa(v->ival,out,10); return; }
        return;
    }
    if(g_tok==TK_STR_KW){
        /* str(expr) — int to string */
        next_token();
        int v=parse_expr_int();
        itoa(v,out,10); return;
    }
    /* fallback: evaluate as int and convert */
    int v=parse_expr_int();
    itoa(v,out,10);
}

/* ── Condition (returns 1=true, 0=false) ───────────────────────── */

static int parse_cond(void){
    /* Check for NOT */
    if(g_tok==TK_NOT){ next_token(); return !parse_cond(); }

    /* Determine if lhs is string or int by peeking */
    int lhs_int=0; char lhs_str[BOOT_STRVAL]="";
    int is_str_cmp=0;

    /* Check if it starts with a string var or literal */
    if(g_tok==TK_STR){ parse_expr_str(lhs_str,BOOT_STRVAL); is_str_cmp=1; }
    else if(g_tok==TK_IDENT){
        boot_var_t *v=var_find(g_tok_str);
        if(v&&v->is_str){ parse_expr_str(lhs_str,BOOT_STRVAL); is_str_cmp=1; }
        else lhs_int=parse_expr_int();
    } else lhs_int=parse_expr_int();

    tok_t op=g_tok; next_token();

    int result=0;
    if(is_str_cmp){
        char rhs[BOOT_STRVAL]=""; parse_expr_str(rhs,BOOT_STRVAL);
        int cmp=strcmp(lhs_str,rhs);
        if(op==TK_EQ)  result=(cmp==0);
        else if(op==TK_NEQ) result=(cmp!=0);
        else if(op==TK_LT)  result=(cmp<0);
        else if(op==TK_GT)  result=(cmp>0);
        else if(op==TK_LE)  result=(cmp<=0);
        else if(op==TK_GE)  result=(cmp>=0);
        else result=(lhs_str[0]!=0); /* no op — truthy if non-empty */
    } else {
        int rhs=parse_expr_int();
        if(op==TK_EQ)  result=(lhs_int==rhs);
        else if(op==TK_NEQ) result=(lhs_int!=rhs);
        else if(op==TK_LT)  result=(lhs_int<rhs);
        else if(op==TK_GT)  result=(lhs_int>rhs);
        else if(op==TK_LE)  result=(lhs_int<=rhs);
        else if(op==TK_GE)  result=(lhs_int>=rhs);
        else result=(lhs_int!=0); /* no op — truthy if non-zero */
    }

    /* optional AND / OR chaining */
    while(!g_error){
        if(g_tok==TK_AND){ next_token(); int r2=parse_cond(); result=result&&r2; }
        else if(g_tok==TK_OR){ next_token(); int r2=parse_cond(); result=result||r2; }
        else break;
    }
    return result;
}

/* ── Skip a block until matching end/else ──────────────────────── */

static void skip_block(int stop_on_else){
    int depth=1;
    while(!g_error && g_tok!=TK_EOF){
        if(g_tok==TK_IF||g_tok==TK_WHILE) depth++;
        if(g_tok==TK_END){ depth--; if(depth==0){ next_token(); return; } }
        if(depth==1&&stop_on_else&&g_tok==TK_ELSE) return;
        next_token();
    }
}

/* ── Input helper ──────────────────────────────────────────────── */

static void boot_input_line(char *buf, int max){
    int pos=0; buf[0]=0;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_puts("? ");
    while(1){
        int c=keyboard_waitchar();
        if(c=='\n'||c=='\r'){ vga_puts("\n"); break; }
        if((c=='\b'||c==127)&&pos>0){
            pos--; buf[pos]=0;
            vga_puts("\b \b");
        } else if(c>=32&&c<127&&pos<max-1){
            buf[pos++]=c; buf[pos]=0;
            char ch[2]={c,0}; vga_puts(ch);
        }
    }
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── Statement parser ──────────────────────────────────────────── */

static void parse_stmts(int stop_on_else){
    while(!g_error && g_tok!=TK_EOF){
        skip_nl();
        if(g_tok==TK_EOF || g_tok==TK_END) return;
        if(stop_on_else && g_tok==TK_ELSE) return;

        /* var name = expr */
        if(g_tok==TK_VAR){
            next_token();
            if(g_tok!=TK_IDENT){ boot_err("expected var name"); return; }
            char name[24]; strncpy(name,g_tok_str,23); next_token();
            if(g_tok!=TK_ASSIGN){ boot_err("expected ="); return; }
            next_token();
            boot_var_t *v=var_get_or_create(name);
            if(!v) return;
            if(g_tok==TK_STR){ v->is_str=1; strncpy(v->sval,g_tok_str,BOOT_STRVAL-1); next_token(); }
            else if(g_tok==TK_IDENT){
                boot_var_t *src=var_find(g_tok_str);
                if(src&&src->is_str){ v->is_str=1; strncpy(v->sval,src->sval,BOOT_STRVAL-1); next_token(); }
                else { v->is_str=0; v->ival=parse_expr_int(); }
            }
            else { v->is_str=0; v->ival=parse_expr_int(); }
            continue;
        }

        /* name = expr  (assignment without var) */
        if(g_tok==TK_IDENT){
            char name[24]; strncpy(name,g_tok_str,23);
            int saved_pos=g_pos; tok_t saved_tok=g_tok;
            (void)saved_pos; (void)saved_tok;
            next_token();
            if(g_tok==TK_ASSIGN){
                next_token();
                boot_var_t *v=var_get_or_create(name);
                if(!v) return;
                if(g_tok==TK_STR){ v->is_str=1; strncpy(v->sval,g_tok_str,BOOT_STRVAL-1); next_token(); }
                else if(g_tok==TK_IDENT){
                    boot_var_t *src=var_find(g_tok_str);
                    if(src&&src->is_str){ v->is_str=1; strncpy(v->sval,src->sval,BOOT_STRVAL-1); next_token(); }
                    else { v->is_str=0; v->ival=parse_expr_int(); }
                }
                else { v->is_str=0; v->ival=parse_expr_int(); }
                continue;
            }
            /* Not an assignment — treat as standalone expression (ignore result) */
            continue;
        }

        /* print expr */
        if(g_tok==TK_PRINT||g_tok==TK_PRINTLN){
            int nl=(g_tok==TK_PRINTLN); next_token();
            if(g_tok==TK_STR){
                vga_puts(g_tok_str); next_token();
            } else if(g_tok==TK_IDENT){
                boot_var_t *v=var_find(g_tok_str);
                next_token();
                if(v&&v->is_str) vga_puts(v->sval);
                else if(v){ char tmp[20]; itoa(v->ival,tmp,10); vga_puts(tmp); }
            } else {
                int val=parse_expr_int();
                char tmp[20]; itoa(val,tmp,10); vga_puts(tmp);
            }
            /* concatenation with + */
            while(g_tok==TK_PLUS&&!g_error){
                next_token();
                if(g_tok==TK_STR){ vga_puts(g_tok_str); next_token(); }
                else if(g_tok==TK_IDENT){
                    boot_var_t *v=var_find(g_tok_str);
                    next_token();
                    if(v&&v->is_str) vga_puts(v->sval);
                    else if(v){ char tmp[20]; itoa(v->ival,tmp,10); vga_puts(tmp); }
                } else {
                    int val=parse_expr_int();
                    char tmp[20]; itoa(val,tmp,10); vga_puts(tmp);
                }
            }
            if(nl) vga_puts("\n");
            continue;
        }

        /* input varname */
        if(g_tok==TK_INPUT){
            next_token();
            if(g_tok!=TK_IDENT){ boot_err("expected var name after input"); return; }
            char name[24]; strncpy(name,g_tok_str,23); next_token();
            boot_var_t *v=var_get_or_create(name);
            if(!v) return;
            char ibuf[64]; boot_input_line(ibuf,64);
            /* try int first */
            int isnum=1;
            for(int i=0;ibuf[i];i++) if(!is_digit(ibuf[i])&&!(i==0&&ibuf[i]=='-')) isnum=0;
            if(isnum&&ibuf[0]){ v->is_str=0; v->ival=0; int neg=0; char *p=ibuf; if(*p=='-'){neg=1;p++;} while(*p) v->ival=v->ival*10+(*p++-'0'); if(neg)v->ival=-v->ival; }
            else { v->is_str=1; strncpy(v->sval,ibuf,BOOT_STRVAL-1); }
            continue;
        }

        /* clear */
        if(g_tok==TK_CLEAR){ next_token(); vga_clear(); continue; }

        /* wait N  (milliseconds) */
        if(g_tok==TK_WAIT){ next_token(); int ms=parse_expr_int(); sleep_ms((uint32_t)ms); continue; }

        /* stop */
        if(g_tok==TK_STOP){ g_tok=TK_EOF; return; }

        /* if cond then ... [else ...] end */
        if(g_tok==TK_IF){
            next_token();
            int cond=parse_cond();
            skip_nl();
            if(g_tok==TK_THEN) next_token();
            skip_nl();
            if(cond){
                parse_stmts(1); /* stop on else */
                if(g_tok==TK_ELSE){ next_token(); skip_block(0); }
                else if(g_tok==TK_END) next_token();
            } else {
                skip_block(1); /* skip to else or end */
                if(g_tok==TK_ELSE){ next_token(); skip_nl(); parse_stmts(0); if(g_tok==TK_END) next_token(); }
            }
            continue;
        }

        /* while cond do ... end */
        if(g_tok==TK_WHILE){
            int loop_pos=g_pos; int loop_line=g_line;
            next_token();
            int iter=0;
            while(!g_error){
                int cond=parse_cond();
                skip_nl();
                if(g_tok==TK_DO) next_token();
                skip_nl();
                if(!cond){ skip_block(0); break; }
                parse_stmts(0);
                if(g_tok==TK_END) next_token();
                /* rewind to while */
                g_pos=loop_pos; g_line=loop_line;
                next_token();
                iter++;
                if(iter>10000){ boot_err("infinite loop guard"); break; }
            }
            continue;
        }

        /* rem comment — already handled in lexer as TK_NEWLINE */
        if(g_tok==TK_NEWLINE){ next_token(); continue; }

        /* readfile <path-expr> <var>
           Reads the contents of a file into a string variable.
           Example:  readfile "HOME/notes.txt" content
           Sets content="" if the file cannot be read. */
        if(g_tok==TK_READFILE){
            next_token();
            char path[64]; path[0]=0;
            /* path can be a string literal or variable */
            if(g_tok==TK_STR){
                strncpy(path, g_tok_str, 63); path[63]=0;
                next_token();
            } else if(g_tok==TK_IDENT){
                boot_var_t *pv=var_find(g_tok_str);
                if(pv&&pv->is_str) strncpy(path,pv->sval,63);
                next_token();
            }
            /* next token must be variable name to store result */
            if(g_tok!=TK_IDENT){ boot_err("readfile: expected variable name"); continue; }
            boot_var_t *v=var_get_or_create(g_tok_str);
            next_token();
            if(!v) continue;
            static char rfbuf[BOOT_STRVAL];
            int rn = path[0] ? vfs_read(path, rfbuf, BOOT_STRVAL-1) : -1;
            v->is_str=1;
            if(rn>0){ rfbuf[rn]=0; strncpy(v->sval, rfbuf, BOOT_STRVAL-1); }
            else     { v->sval[0]=0; }
            continue;
        }

        /* writefile <path-expr> <value-expr>
           Writes a string value to a file (overwrites).
           Example:  writefile "HOME/log.txt" "hello world"
           Sets up a 0/1 success status in a future var if needed; for now
           it silently writes and continues. */
        if(g_tok==TK_WRITEFILE){
            next_token();
            char path[64]; path[0]=0;
            if(g_tok==TK_STR){
                strncpy(path, g_tok_str, 63); path[63]=0;
                next_token();
            } else if(g_tok==TK_IDENT){
                boot_var_t *pv=var_find(g_tok_str);
                if(pv&&pv->is_str) strncpy(path,pv->sval,63);
                next_token();
            }
            /* parse the value expression */
            boot_var_t result;
            result.is_str=0; result.ival=0; result.sval[0]=0;
            if(g_tok==TK_STR){
                result.is_str=1; strncpy(result.sval,g_tok_str,BOOT_STRVAL-1);
                next_token();
            } else {
                result.ival=parse_expr_int();
                char tmp[16]; itoa(result.ival,tmp,10);
                result.is_str=1; strncpy(result.sval,tmp,BOOT_STRVAL-1);
            }
            if(path[0]){
                const char *wdata = result.sval;
                vfs_write(path, wdata, (uint32_t)strlen(wdata));
            }
            continue;
        }

        /* exists <path-expr> <var>
           Sets var to 1 if file exists, 0 otherwise.
           Example:  exists "HOME/save.txt" found
                     if found == 1 then ... end */
        if(g_tok==TK_EXISTS){
            next_token();
            char path[64]; path[0]=0;
            if(g_tok==TK_STR){
                strncpy(path, g_tok_str, 63); path[63]=0;
                next_token();
            } else if(g_tok==TK_IDENT){
                boot_var_t *pv=var_find(g_tok_str);
                if(pv&&pv->is_str) strncpy(path,pv->sval,63);
                next_token();
            }
            if(g_tok!=TK_IDENT){ boot_err("exists: expected variable name"); continue; }
            boot_var_t *v=var_get_or_create(g_tok_str);
            next_token();
            if(!v) continue;
            v->is_str=0;
            v->ival = (path[0] && vfs_exists(path)) ? 1 : 0;
            continue;
        }

        /* unknown token — skip */
        next_token();
    }
}

/* ================================================================
   PUBLIC API
   ================================================================ */

/* Run BOOT source from string, returns 1=ok, 0=error */
int boot_run_source(const char *src){
    /* init state */
    g_error=0; g_errmsg[0]=0;
    g_var_count=0;
    g_strpool_pos=0;
    g_line=1;

    int slen=0; while(src[slen]&&slen<BOOT_SRC_MAX-1) slen++;
    for(int i=0;i<slen;i++) g_src[i]=src[i];
    g_src[slen]=0;
    g_src_len=slen;
    g_pos=0;

    next_token();
    parse_stmts(0);

    if(g_error){
        vga_puts("\nH error: "); vga_puts(g_errmsg);
        vga_puts(" (line "); char ln[8]; utoa((uint32_t)g_line,ln,10); vga_puts(ln); vga_puts(")\n");
        return 0;
    }
    return 1;
}

/* Run BOOT source from a FAT16 file, returns 1=ok, 0=error */
int boot_run_file(const char *path){
    static char fbuf[BOOT_SRC_MAX];
    int n=vfs_read(path,fbuf,BOOT_SRC_MAX-1);
    if(n<=0){
        vga_puts("H: cannot open file: "); vga_puts(path); vga_puts("\n");
        return 0;
    }
    fbuf[n]=0;
    return boot_run_source(fbuf);
}

/* Interactive REPL */
void boot_repl(void){
    vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
    vga_puts("H Language v0.2.0  (TechHaven Studios  --  HavenCode)\n");
    vga_puts("Type H code, blank line to run. Type: help  run  exit\n");
    vga_puts("Commands: run, clear, help, quit\n\n");
    vga_set_color(VGA_WHITE,VGA_BLACK);

    static char prog[BOOT_SRC_MAX];
    int prog_len=0;

    while(1){
        /* prompt */
        vga_set_color(VGA_LIGHT_GREEN,VGA_BLACK);
        vga_puts("boot> ");
        vga_set_color(VGA_WHITE,VGA_BLACK);

        /* read one line */
        char line[128]; int lp=0; line[0]=0;
        while(1){
            int c=keyboard_waitchar();
            if(c==27){ vga_puts("\n"); goto done; }
            if(c=='\n'||c=='\r'){ line[lp]=0; vga_puts("\n"); break; }
            if((c=='\b'||c==127)&&lp>0){ lp--; line[lp]=0; vga_puts("\b \b"); }
            else if(c>=32&&c<127&&lp<126){ line[lp++]=c; line[lp]=0; char ch[2]={c,0}; vga_puts(ch); }
        }

        /* special commands */
        if(strcmp(line,"quit")==0||strcmp(line,"exit")==0) goto done;
        if(strcmp(line,"help")==0){
            vga_set_color(VGA_LIGHT_CYAN,VGA_BLACK);
            vga_puts("H Language quick reference (H / H++ / H#):\n");
            vga_puts("  var x = 10         declare integer\n");
            vga_puts("  var s = \"hello\"    declare string\n");
            vga_puts("  print x + \" world\" print (no newline)\n");
            vga_puts("  println x          print with newline\n");
            vga_puts("  input x            read from keyboard\n");
            vga_puts("  x = x + 1         assignment\n");
            vga_puts("  if x > 5 then ... else ... end\n");
            vga_puts("  while x < 10 do ... end\n");
            vga_puts("  wait 500           wait milliseconds\n");
            vga_puts("  clear              clear screen\n");
            vga_puts("  stop               stop program\n");
            vga_puts("  abs x  len s  str(n)\n");
            vga_puts("  readfile \"path\" v  read file into variable\n");
            vga_puts("  writefile \"path\" v write variable to file\n");
            vga_puts("  exists \"path\" v   1 if file exists, else 0\n");
            vga_set_color(VGA_WHITE,VGA_BLACK);
            prog_len=0; prog[0]=0;
            continue;
        }
        if(strcmp(line,"clear")==0){ prog_len=0; prog[0]=0; vga_puts("(program cleared)\n"); continue; }
        if(strcmp(line,"run")==0||line[0]==0){
            if(prog_len>0){
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                vga_puts("--- running ---\n");
                vga_set_color(VGA_WHITE,VGA_BLACK);
                boot_run_source(prog);
                vga_set_color(VGA_DARK_GREY,VGA_BLACK);
                vga_puts("--- done ---\n");
                vga_set_color(VGA_WHITE,VGA_BLACK);
                prog_len=0; prog[0]=0;
            }
            continue;
        }

        /* accumulate line into program buffer */
        int ll=(int)strlen(line);
        if(prog_len+ll+2<BOOT_SRC_MAX){
            for(int i=0;i<ll;i++) prog[prog_len++]=line[i];
            prog[prog_len++]='\n';
            prog[prog_len]=0;
        } else {
            vga_puts("(program buffer full - type run to execute)\n");
        }
    }
done:
    vga_set_color(VGA_WHITE,VGA_BLACK);
}
