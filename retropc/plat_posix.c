/* retropc/plat_posix.c -- folders and files on Linux.  v0.9.7.5.

   The RETRO folder lives beside the program, as it does on Windows,
   so a copy unpacked anywhere keeps its images and disks together.
   When that folder cannot be written -- the program installed in
   /usr/local/bin, say -- it is ~/RetroPC instead. */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "retropc.h"

const char *plat_data_dir(void){
    static char dir[PATH_MAX];
    ssize_t n;
    if(dir[0]) return dir;
    n = readlink("/proc/self/exe", dir, sizeof dir - 1);
    if(n > 0){
        char *sl;
        dir[n] = 0;
        sl = strrchr(dir, '/');
        if(sl){ *sl = 0; if(!dir[0]) strcpy(dir, "/"); }
        if(access(dir, W_OK) == 0) return dir;
    }
    snprintf(dir, sizeof dir, "%s/RetroPC", getenv("HOME") ? getenv("HOME") : ".");
    mkdir(dir, 0755);
    return dir;
}

int plat_mkdir(const char *path){
    return mkdir(path, 0755) == 0 ? 0 : -1;
}

int plat_list(const char *dir, void (*cb)(const char *, uint64_t, void *), void *ctx){
    DIR *d = opendir(dir);
    struct dirent *e;
    int n = 0;
    if(!d) return -1;
    while((e = readdir(d))){
        char p[PATH_MAX];
        struct stat st;
        if(e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if(stat(p, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        cb(e->d_name, (uint64_t)st.st_size, ctx);
        n++;
    }
    closedir(d);
    return n;
}

char plat_sep(void){ return '/'; }
