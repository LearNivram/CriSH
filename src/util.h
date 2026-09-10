/* util.h - allocation, growable buffers and string vectors.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_UTIL_H
#define CRISH_UTIL_H

#include <stdarg.h>
#include <stddef.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t size);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
char *xasprintf(const char *fmt, ...);

/* Growable byte buffer.  Always NUL terminated, never counts the NUL in len. */
typedef struct {
	char *b;
	size_t len;
	size_t cap;
} Buf;

void buf_init(Buf *buf);
void buf_free(Buf *buf);
void buf_reset(Buf *buf);
void buf_reserve(Buf *buf, size_t extra);
void buf_putc(Buf *buf, int c);
void buf_put(Buf *buf, const char *s, size_t n);
void buf_puts(Buf *buf, const char *s);
void buf_printf(Buf *buf, const char *fmt, ...);
void buf_vprintf(Buf *buf, const char *fmt, va_list ap);
/* Detach the buffer; caller owns the returned string, the Buf is reset. */
char *buf_take(Buf *buf);

/* Growable vector of owned strings. */
typedef struct {
	char **v;
	size_t len;
	size_t cap;
} Vec;

void vec_init(Vec *vec);
void vec_free(Vec *vec);
void vec_clear(Vec *vec);
void vec_push(Vec *vec, char *owned);
void vec_pushs(Vec *vec, const char *copy);
void vec_insert(Vec *vec, size_t at, char *owned);
char *vec_remove(Vec *vec, size_t at);
/* NUL terminated view for execve(); valid until the next push. */
char **vec_argv(Vec *vec);
void vec_sort(Vec *vec);
Vec vec_clone(const Vec *vec);

int str_eq(const char *a, const char *b);
int str_prefix(const char *s, const char *prefix);
int str_suffix(const char *s, const char *suffix);
/* Case insensitive compare of NUL terminated ASCII. */
int str_casecmp(const char *a, const char *b);
char *str_trim(char *s);
/* Split on a single delimiter; empty fields are kept. */
void str_split(const char *s, int delim, Vec *out);
/* Join with sep; returns owned string. */
char *str_join(const Vec *vec, const char *sep);

/* Names that may be used as shell variables/functions. */
int is_name_char(int c, int first);
int is_valid_name(const char *s);

/* Quote s so that reading it back through the shell yields s. */
char *shell_quote(const char *s);

#endif /* CRISH_UTIL_H */
