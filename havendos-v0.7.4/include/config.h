#pragma once
#include "types.h"
void config_load(void);
void config_save(void);
int  config_get_setup_done(void);
void config_set_setup_done(int v);
int  config_get_autologin(void);
void config_set_autologin(int v, const char *user);
const char *config_get_autologin_user(void);
int  config_get_verbose_boot(void);
void config_set_verbose_boot(int v);
int  config_get_post_screen(void);
void config_set_post_screen(int v);
int  config_get_screensaver_timeout(void);
void config_set_screensaver_timeout(int v);
int  config_get_autologin_timeout(void);
void config_set_autologin_timeout(int v);
const char *config_get_hostname(void);
void config_set_hostname(const char *h);
int  config_get_acct_hidden(int idx);
void config_set_acct_hidden(int idx, int v);
