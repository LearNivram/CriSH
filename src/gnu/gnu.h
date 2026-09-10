/* gnu.h - shared plumbing for the built-in GNU-compatible utilities.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_GNU_H
#define CRISH_GNU_H

#include <stdio.h>

#include "../shell.h"

/* Every tool reports errors the way GNU does: "prog: message". */
void gnu_error(const char *prog, const char *fmt, ...);
/* "prog: file: strerror(errno)" */
int gnu_file_error(const char *prog, const char *file);

/* Open a named file, or stdin for "-" and NULL. */
FILE *gnu_open(const char *prog, const char *path, int *is_stdin);
void gnu_close(FILE *f, int is_stdin);

/* Read one delimiter-terminated record.  Returns the length, or -1 at EOF.
 * The delimiter is kept in the buffer; *buf is reused between calls. */
long gnu_getdelim(Buf *buf, FILE *f, int delim);

/* Long-option matching that accepts unambiguous abbreviations the way GNU
 * does: --col matches --color when nothing else does. */
int gnu_long_opt(const char *arg, const char *name, const char **value);

/* Individual tools. */
int gnu_grep(int argc, char **argv);
int gnu_ls(int argc, char **argv);
int gnu_sed(int argc, char **argv);
int gnu_awk(int argc, char **argv);
int gnu_sort(int argc, char **argv);
int gnu_uniq(int argc, char **argv);
int gnu_cut(int argc, char **argv);
int gnu_tr(int argc, char **argv);
int gnu_head(int argc, char **argv);
int gnu_tail(int argc, char **argv);
int gnu_wc(int argc, char **argv);
int gnu_cat(int argc, char **argv);
int gnu_tee(int argc, char **argv);
int gnu_rev(int argc, char **argv);
int gnu_tac(int argc, char **argv);
int gnu_nl(int argc, char **argv);
int gnu_paste(int argc, char **argv);
int gnu_seq(int argc, char **argv);
int gnu_shuf(int argc, char **argv);
int gnu_xargs(int argc, char **argv);
int gnu_basename(int argc, char **argv);
int gnu_dirname(int argc, char **argv);
int gnu_env(int argc, char **argv);
int gnu_sleep(int argc, char **argv);
int gnu_yes(int argc, char **argv);
int gnu_expr(int argc, char **argv);
int gnu_truncate(int argc, char **argv);
int gnu_date(int argc, char **argv);
int gnu_timeout(int argc, char **argv);
int gnu_sha(int argc, char **argv);
int gnu_base64(int argc, char **argv);
int gnu_stat(int argc, char **argv);
int gnu_readlink(int argc, char **argv);
int gnu_realpath(int argc, char **argv);
int gnu_mktemp(int argc, char **argv);

/* sha256 of a file, as lowercase hex; used by the self-updater too. */
char *sha256_file(const char *path);

#endif /* CRISH_GNU_H */
