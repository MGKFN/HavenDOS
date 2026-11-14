/* ════════════════════════════════════════════════════════════════════
   test_cmddb.c -- the command reference, v0.9.5.

   shell/cmddb.c is the one table `help`, `man` and the Manual app
   read. tools/check_help.py holds it to the shell's dispatcher at
   build time; this holds its lookups to what `help <word>` and
   `man <word>` promise: a command by its name or by any alias, a
   topic by its key or a synonym, and nothing for a word that is
   neither.
   ════════════════════════════════════════════════════════════════════ */
#include <stdio.h>
#include <string.h>
#include "../include/cmddb.h"
#include "../shell/cmddb.c"

static int total, fails;
static void ck(const char *what, int ok){
    total++;
    if(ok) printf("    ok   %s\n", what);
    else { fails++; printf("    FAIL %s\n", what); }
}

int main(void){
    int n = cmddb_count();
    printf("\n  lookups\n");
    ck("there are more than a hundred entries", n > 100);
    ck("ls is found by name", cmddb_find("ls") && !strcmp(cmddb_find("ls")->name, "ls"));
    ck("dir finds ls (an alias)", cmddb_find("dir") == cmddb_find("ls"));
    ck("? finds help", cmddb_find("?") == cmddb_find("help"));
    ck("every old system app name finds System",
       cmddb_find("dashboard") == cmddb_find("system") &&
       cmddb_find("htop") == cmddb_find("system") &&
       cmddb_find("sysinfo") == cmddb_find("system") &&
       cmddb_find("sysinfo-plus") == cmddb_find("system") &&
       cmddb_find("sysmon") == cmddb_find("system"));
    ck("h is the H language, not help", cmddb_find("h") && !strcmp(cmddb_find("h")->name, "h"));
    ck("boot and hplusplus find h", cmddb_find("boot") == cmddb_find("h") &&
                                    cmddb_find("hplusplus") == cmddb_find("h"));
    ck("a prefix of an alias is not the alias", cmddb_find("dashb") == 0);
    ck("an alias list's tail is not matched mid-word", cmddb_find("plus") == 0);
    ck("an empty word finds nothing", cmddb_find("") == 0 && cmddb_find(0) == 0);
    ck("a word that is nothing finds nothing", cmddb_find("pong") == 0);

    printf("\n  topics\n");
    for(int c = 0; c < CDB_NCAT; c++)
        if(cmddb_cat_find(cmddb_cats[c].key) != c){ ck("every topic key finds its topic", 0); break; }
        else if(c == CDB_NCAT - 1) ck("every topic key finds its topic", 1);
    ck("net is network", cmddb_cat_find("net") == CDB_NET);
    ck("games is apps", cmddb_cat_find("games") == CDB_APPS);
    ck("accounts is users", cmddb_cat_find("accounts") == CDB_USERS);
    ck("a command is not a topic", cmddb_cat_find("ls") == -1);
    ck("all is not a topic (help handles it)", cmddb_cat_find("all") == -1);

    printf("\n  every entry\n");
    int cats_ok = 1, sum_ok = 1, text_ok = 1, each_cat[CDB_NCAT] = {0};
    for(int i = 0; i < n; i++){
        const cmddoc_t *d = &cmddb[i];
        if(d->cat >= CDB_NCAT) cats_ok = 0; else each_cat[d->cat]++;
        if(strlen(d->summary) > CMDDB_SUMMARY_MAX) sum_ok = 0;
        if(!d->synopsis[0] || !d->desc[0] || strlen(d->desc) < 20) text_ok = 0;
        /* No trailing or doubled spaces from string-literal joins. */
        if(strstr(d->desc, "  ") || d->desc[strlen(d->desc)-1] == ' ') text_ok = 0;
    }
    ck("every entry is in a real topic", cats_ok);
    int empty = 0; for(int c = 0; c < CDB_NCAT; c++) if(!each_cat[c]) empty = 1;
    ck("no topic is empty", !empty);
    ck("every summary fits the 64-column console", sum_ok);
    ck("every description is written, with no stray spaces", text_ok);
    int first_by_name = 1;
    for(int i = 0; i < n; i++) if(cmddb_find(cmddb[i].name) != &cmddb[i]) first_by_name = 0;
    ck("every entry is what its own name finds", first_by_name);

    printf("\n%d/%d passed\n", total - fails, total);
    return fails ? 1 : 0;
}
