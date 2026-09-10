/* util.c - allocation, growable buffers and string vectors.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void oom(void)
{
	static const char msg[] = "crish: out of memory\n";
	ssize_t ignored = write(2, msg, sizeof msg - 1);
	(void)ignored;
	_exit(2);
}

void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		oom();
	return p;
}

void *xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);
	if (!p)
		oom();
	return p;
}

void *xrealloc(void *p, size_t n)
{
	void *q = realloc(p, n ? n : 1);
	if (!q)
		oom();
	return q;
}

char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = xmalloc(n);
	memcpy(p, s, n);
	return p;
}

char *xstrndup(const char *s, size_t n)
{
	char *p = xmalloc(n + 1);
	memcpy(p, s, n);
	p[n] = '\0';
	return p;
}

char *xasprintf(const char *fmt, ...)
{
	Buf b;
	va_list ap;

	buf_init(&b);
	va_start(ap, fmt);
	buf_vprintf(&b, fmt, ap);
	va_end(ap);
	return buf_take(&b);
}

void buf_init(Buf *buf)
{
	buf->b = NULL;
	buf->len = 0;
	buf->cap = 0;
}

void buf_free(Buf *buf)
{
	free(buf->b);
	buf_init(buf);
}

void buf_reset(Buf *buf)
{
	buf->len = 0;
	if (buf->b)
		buf->b[0] = '\0';
}

void buf_reserve(Buf *buf, size_t extra)
{
	size_t want = buf->len + extra + 1;
	size_t cap;

	if (want <= buf->cap)
		return;
	cap = buf->cap ? buf->cap : 32;
	while (cap < want)
		cap *= 2;
	buf->b = xrealloc(buf->b, cap);
	buf->cap = cap;
}

void buf_putc(Buf *buf, int c)
{
	buf_reserve(buf, 1);
	buf->b[buf->len++] = (char)c;
	buf->b[buf->len] = '\0';
}

void buf_put(Buf *buf, const char *s, size_t n)
{
	if (!n)
		return;
	buf_reserve(buf, n);
	memcpy(buf->b + buf->len, s, n);
	buf->len += n;
	buf->b[buf->len] = '\0';
}

void buf_puts(Buf *buf, const char *s)
{
	buf_put(buf, s, strlen(s));
}

void buf_vprintf(Buf *buf, const char *fmt, va_list ap)
{
	va_list copy;
	int n;

	va_copy(copy, ap);
	n = vsnprintf(NULL, 0, fmt, copy);
	va_end(copy);
	if (n <= 0)
		return;
	buf_reserve(buf, (size_t)n);
	vsnprintf(buf->b + buf->len, (size_t)n + 1, fmt, ap);
	buf->len += (size_t)n;
}

void buf_printf(Buf *buf, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	buf_vprintf(buf, fmt, ap);
	va_end(ap);
}

char *buf_take(Buf *buf)
{
	char *s = buf->b ? buf->b : xstrdup("");
	buf_init(buf);
	return s;
}

void vec_init(Vec *vec)
{
	vec->v = NULL;
	vec->len = 0;
	vec->cap = 0;
}

void vec_clear(Vec *vec)
{
	size_t i;

	for (i = 0; i < vec->len; i++)
		free(vec->v[i]);
	vec->len = 0;
}

void vec_free(Vec *vec)
{
	vec_clear(vec);
	free(vec->v);
	vec_init(vec);
}

static void vec_grow(Vec *vec, size_t extra)
{
	size_t want = vec->len + extra + 1;
	size_t cap;

	if (want <= vec->cap)
		return;
	cap = vec->cap ? vec->cap : 8;
	while (cap < want)
		cap *= 2;
	vec->v = xrealloc(vec->v, cap * sizeof *vec->v);
	vec->cap = cap;
}

void vec_push(Vec *vec, char *owned)
{
	vec_grow(vec, 1);
	vec->v[vec->len++] = owned;
	vec->v[vec->len] = NULL;
}

void vec_pushs(Vec *vec, const char *copy)
{
	vec_push(vec, xstrdup(copy));
}

void vec_insert(Vec *vec, size_t at, char *owned)
{
	if (at > vec->len)
		at = vec->len;
	vec_grow(vec, 1);
	memmove(vec->v + at + 1, vec->v + at, (vec->len - at) * sizeof *vec->v);
	vec->v[at] = owned;
	vec->len++;
	vec->v[vec->len] = NULL;
}

char *vec_remove(Vec *vec, size_t at)
{
	char *s;

	if (at >= vec->len)
		return NULL;
	s = vec->v[at];
	memmove(vec->v + at, vec->v + at + 1, (vec->len - at - 1) * sizeof *vec->v);
	vec->len--;
	vec->v[vec->len] = NULL;
	return s;
}

char **vec_argv(Vec *vec)
{
	vec_grow(vec, 1);
	vec->v[vec->len] = NULL;
	return vec->v;
}

static int cmp_str(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

void vec_sort(Vec *vec)
{
	if (vec->len > 1)
		qsort(vec->v, vec->len, sizeof *vec->v, cmp_str);
}

Vec vec_clone(const Vec *vec)
{
	Vec out;
	size_t i;

	vec_init(&out);
	for (i = 0; i < vec->len; i++)
		vec_pushs(&out, vec->v[i]);
	return out;
}

int str_eq(const char *a, const char *b)
{
	return a && b && strcmp(a, b) == 0;
}

int str_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

int str_suffix(const char *s, const char *suffix)
{
	size_t ls = strlen(s), lx = strlen(suffix);

	return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

int str_casecmp(const char *a, const char *b)
{
	while (*a && *b) {
		int x = tolower((unsigned char)*a++);
		int y = tolower((unsigned char)*b++);
		if (x != y)
			return x - y;
	}
	return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

char *str_trim(char *s)
{
	char *end;

	while (*s && isspace((unsigned char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((unsigned char)end[-1]))
		end--;
	*end = '\0';
	return s;
}

void str_split(const char *s, int delim, Vec *out)
{
	const char *p = s;

	for (;;) {
		const char *q = strchr(p, delim);
		if (!q) {
			vec_pushs(out, p);
			return;
		}
		vec_push(out, xstrndup(p, (size_t)(q - p)));
		p = q + 1;
	}
}

char *str_join(const Vec *vec, const char *sep)
{
	Buf b;
	size_t i;

	buf_init(&b);
	for (i = 0; i < vec->len; i++) {
		if (i)
			buf_puts(&b, sep);
		buf_puts(&b, vec->v[i]);
	}
	return buf_take(&b);
}

int is_name_char(int c, int first)
{
	if (c == '_' || isalpha(c))
		return 1;
	return !first && isdigit(c);
}

int is_valid_name(const char *s)
{
	size_t i;

	if (!s || !*s)
		return 0;
	for (i = 0; s[i]; i++)
		if (!is_name_char((unsigned char)s[i], i == 0))
			return 0;
	return 1;
}

char *shell_quote(const char *s)
{
	Buf b;
	const char *p;
	int plain = *s != '\0';

	for (p = s; *p; p++) {
		if (!(isalnum((unsigned char)*p) || strchr("_-./=:,@+%^", *p))) {
			plain = 0;
			break;
		}
	}
	if (plain)
		return xstrdup(s);

	buf_init(&b);
	buf_putc(&b, '\'');
	for (p = s; *p; p++) {
		if (*p == '\'')
			buf_puts(&b, "'\\''");
		else
			buf_putc(&b, *p);
	}
	buf_putc(&b, '\'');
	return buf_take(&b);
}
