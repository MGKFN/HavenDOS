#pragma once
/* ════════════════════════════════════════════════════════════════════
   include/cmddb.h -- the one command reference.  v0.9.5.

   `help` and `man` used to be two hand-kept lists in two files: a
   category table in shell/help.c and 46 man pages in shell/man.c.
   They drifted apart and away from the shell. man said `ls` read
   "RAMFS", `help` paged with a fixed 20-line counter on a screen that
   can be 60 rows, `man` drew on an 80x25 canvas at fixed coordinates
   and cut every page off at row 22, and about a hundred commands had
   no man page at all.

   Now there is one table (shell/cmddb.c).  `help` prints from it,
   `man` renders from it, the desktop's Manual app reads it, and
   tools/check_help.py fails the build when it and exec_cmd() disagree
   in either direction.
   ════════════════════════════════════════════════════════════════════ */
#include "types.h"

enum {
    CDB_FILES = 0,   /* files and folders                         */
    CDB_TEXT,        /* text and data: filters, encoders          */
    CDB_SHELL,       /* the shell itself and scripting            */
    CDB_SYSTEM,      /* the running system, display, power        */
    CDB_DISKS,       /* disks, install, update, packages          */
    CDB_HW,          /* hardware and drivers                      */
    CDB_NET,         /* network                                   */
    CDB_USERS,       /* accounts and security                     */
    CDB_APPS,        /* apps, games and toys                      */
    CDB_NCAT
};

typedef struct {
    const char *name;      /* what you type                               */
    const char *aliases;   /* other names that do the same, space-separated,
                              or NULL                                     */
    uint8_t     cat;       /* CDB_*                                       */
    const char *synopsis;  /* one form per line                           */
    const char *summary;   /* one line, at most CMDDB_SUMMARY_MAX chars   */
    const char *desc;      /* wrapped at render; blank line = paragraph   */
    const char *examples;  /* "command\twhat it does" per line, or NULL   */
    const char *see;       /* related commands, space-separated, or NULL  */
} cmddoc_t;

typedef struct {
    const char *key;       /* `help files`                                */
    const char *title;     /* "Files and folders"                         */
    const char *blurb;     /* one line for the index                      */
} cmdcat_t;

/* Checked by tools/check_help.py so `help <topic>` never wraps a line
   on the narrowest console HavenDOS draws (64 columns). */
#define CMDDB_SUMMARY_MAX 44

extern const cmddoc_t cmddb[];      /* ends with name == NULL */
extern const cmdcat_t cmddb_cats[]; /* CDB_NCAT entries        */

int             cmddb_count(void);
/* By name or by any alias; NULL if neither. */
const cmddoc_t *cmddb_find(const char *word);
/* `help <word>`: a category key, or -1. Accepts a few plurals/synonyms. */
int             cmddb_cat_find(const char *word);
/* 1 if `word` is listed in `d`'s aliases. */
int             cmddb_is_alias(const cmddoc_t *d, const char *word);
