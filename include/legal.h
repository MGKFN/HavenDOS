#pragma once
/* v0.7.8: shown by the first-boot setup wizard, not the installer. */
int legal_show_document(const char *title, const char *sub,
                        const char **lines, int nlines, int step, int total);
int legal_show_tos(void);
int legal_show_privacy(void);
