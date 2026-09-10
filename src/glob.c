/* glob.c - shell pattern matching and pathname expansion.
 *
 * Supports the POSIX set (* ? [...] with ranges and [:classes:]), the ksh
 * extended patterns behind `shopt -s extglob`, and ** behind `globstar`.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

static int chr_eq(int a, int b, int nocase)
{
	if (a == b)
		return 1;
	return nocase && tolower(a) == tolower(b);
}

/* Skip over a bracket expression; returns the char after the closing ]. */
static const char *bracket_end(const char *p)
{
	const char *q = p + 1;

	if (*q == '!' || *q == '^')
		q++;
	if (*q == ']')
		q++;
	while (*q && *q != ']') {
		if (*q == '[' && q[1] == ':') {
			const char *c = strstr(q, ":]");
			if (!c)
				return NULL;
			q = c + 2;
			continue;
		}
		if (*q == '\\' && q[1])
			q++;
		q++;
	}
	return *q == ']' ? q + 1 : NULL;
}

static int class_match(const char *name, size_t len, int c)
{
	static const struct {
		const char *name;
		int (*fn)(int);
	} classes[] = { { "alpha", isalpha }, { "digit", isdigit }, { "alnum", isalnum },
			{ "upper", isupper }, { "lower", islower }, { "space", isspace },
			{ "blank", isblank }, { "punct", ispunct }, { "print", isprint },
			{ "graph", isgraph }, { "cntrl", iscntrl }, { "xdigit", isxdigit },
			{ NULL, NULL } };
	int i;

	for (i = 0; classes[i].name; i++)
		if (strlen(classes[i].name) == len && strncmp(classes[i].name, name, len) == 0)
			return classes[i].fn(c) != 0;
	return 0;
}

/* Match one character against [ ... ]; *pp is advanced past the bracket. */
static int bracket_match(const char **pp, int c, int nocase)
{
	const char *p = *pp + 1;
	int negate = 0, matched = 0;
	const char *end = bracket_end(*pp);

	if (!end) {
		/* an unterminated [ is a literal [ */
		*pp = *pp + 1;
		return c == '[';
	}
	*pp = end;

	if (*p == '!' || *p == '^') {
		negate = 1;
		p++;
	}
	if (*p == ']') {
		if (chr_eq(c, ']', nocase))
			matched = 1;
		p++;
	}
	while (p < end - 1 && *p != ']') {
		if (*p == '[' && p[1] == ':') {
			const char *close = strstr(p, ":]");
			if (close) {
				if (class_match(p + 2, (size_t)(close - p - 2), c))
					matched = 1;
				p = close + 2;
				continue;
			}
		}
		{
			int lo;

			if (*p == '\\' && p[1])
				p++;
			lo = (unsigned char)*p;
			if (p + 2 < end - 1 && p[1] == '-' && p[2] != ']') {
				int hi;
				p += 2;
				if (*p == '\\' && p[1])
					p++;
				hi = (unsigned char)*p;
				if ((c >= lo && c <= hi) ||
				    (nocase && tolower(c) >= tolower(lo) &&
				     tolower(c) <= tolower(hi)))
					matched = 1;
			} else if (chr_eq(c, lo, nocase)) {
				matched = 1;
			}
			p++;
		}
	}
	return negate ? !matched : matched;
}

static int match_here(const char *p, const char *s, int nocase);

/* Find the end of an extended pattern group starting at '(' . */
static const char *group_end(const char *p)
{
	int depth = 0;

	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '[') {
			const char *e = bracket_end(p);
			if (e) {
				p = e - 1;
				continue;
			}
		}
		if (*p == '(')
			depth++;
		else if (*p == ')') {
			if (--depth == 0)
				return p + 1;
		}
	}
	return NULL;
}

/* Split "a|b|c" inside a group into alternatives. */
static void group_alts(const char *start, const char *end, Vec *out)
{
	const char *p = start, *from = start;
	int depth = 0;

	for (; p < end; p++) {
		if (*p == '\\' && p + 1 < end) {
			p++;
			continue;
		}
		if (*p == '(')
			depth++;
		else if (*p == ')')
			depth--;
		else if (*p == '|' && depth == 0) {
			vec_push(out, xstrndup(from, (size_t)(p - from)));
			from = p + 1;
		}
	}
	vec_push(out, xstrndup(from, (size_t)(end - from)));
}

/* ?(..) @(..) *(..) +(..) !(..) */
static int extglob_match(const char *p, const char *s, int nocase)
{
	int kind = *p;
	const char *open = p + 1;
	const char *close = group_end(open);
	const char *rest;
	Vec alts;
	size_t i;
	int ok = 0;

	if (!close)
		return -1;
	rest = close;
	vec_init(&alts);
	group_alts(open + 1, close - 1, &alts);

	if (kind == '!') {
		/* matches anything the alternatives do not */
		size_t len = strlen(s);
		size_t take;

		for (take = 0; take <= len; take++) {
			char save = ((char *)s)[take];
			int hit = 0;

			((char *)s)[take] = '\0';
			for (i = 0; i < alts.len; i++)
				if (match_here(alts.v[i], s, nocase)) {
					hit = 1;
					break;
				}
			((char *)s)[take] = save;
			if (!hit && match_here(rest, s + take, nocase)) {
				ok = 1;
				break;
			}
		}
		vec_free(&alts);
		return ok;
	}

	if (kind == '?' || kind == '@') {
		if (kind == '?' && match_here(rest, s, nocase))
			ok = 1;
		for (i = 0; !ok && i < alts.len; i++) {
			size_t len = strlen(s), take;
			for (take = 0; take <= len; take++) {
				char save = ((char *)s)[take];
				int hit;
				((char *)s)[take] = '\0';
				hit = match_here(alts.v[i], s, nocase);
				((char *)s)[take] = save;
				if (hit && match_here(rest, s + take, nocase)) {
					ok = 1;
					break;
				}
			}
		}
		vec_free(&alts);
		return ok;
	}

	/* * and + : zero (or one) or more repetitions */
	if (kind == '*' && match_here(rest, s, nocase))
		ok = 1;
	if (!ok) {
		size_t len = strlen(s), take;

		for (take = 1; take <= len && !ok; take++) {
			char save = ((char *)s)[take];
			int hit = 0;

			((char *)s)[take] = '\0';
			for (i = 0; i < alts.len; i++)
				if (match_here(alts.v[i], s, nocase)) {
					hit = 1;
					break;
				}
			((char *)s)[take] = save;
			if (!hit)
				continue;
			if (match_here(rest, s + take, nocase)) {
				ok = 1;
				break;
			}
			/* try to consume another repetition of the same group */
			{
				Buf again;
				char *pat;

				buf_init(&again);
				buf_putc(&again, '*');
				buf_put(&again, open, (size_t)(close - open));
				buf_puts(&again, rest);
				pat = buf_take(&again);
				if (match_here(pat, s + take, nocase))
					ok = 1;
				free(pat);
			}
		}
	}
	vec_free(&alts);
	return ok;
}

static int match_here(const char *p, const char *s, int nocase)
{
	while (*p) {
		if (sh.shopt.extglob && strchr("?*+@!", *p) && p[1] == '(') {
			int r = extglob_match(p, s, nocase);
			if (r >= 0)
				return r;
		}
		switch (*p) {
		case '*': {
			const char *q;
			while (p[1] == '*')
				p++;
			p++;
			if (!*p)
				return 1;
			for (q = s;; q++) {
				if (match_here(p, q, nocase))
					return 1;
				if (!*q)
					return 0;
			}
		}
		case '?':
			if (!*s)
				return 0;
			p++;
			s++;
			continue;
		case '[': {
			const char *save = p;
			if (!*s)
				return 0;
			if (!bracket_match(&p, (unsigned char)*s, nocase)) {
				if (p == save + 1) /* literal [ */
					return 0;
				return 0;
			}
			s++;
			continue;
		}
		case '\\':
			if (p[1]) {
				if (!chr_eq((unsigned char)*s, (unsigned char)p[1], 0))
					return 0;
				p += 2;
				s++;
				continue;
			}
			/* fall through */
		default:
			if (!chr_eq((unsigned char)*s, (unsigned char)*p, nocase))
				return 0;
			p++;
			s++;
			continue;
		}
	}
	return *s == '\0';
}

int glob_match(const char *pattern, const char *string, int nocase)
{
	if (!pattern || !string)
		return 0;
	return match_here(pattern, string, nocase);
}

/* ----------------------------------------------------- pathname expansion */

static int seg_has_meta(const char *s)
{
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			continue;
		}
		if (*s == '*' || *s == '?' || *s == '[')
			return 1;
		if (sh.shopt.extglob && strchr("?*+@!", *s) && s[1] == '(')
			return 1;
	}
	return 0;
}

static char *unescape(const char *s)
{
	Buf b;

	buf_init(&b);
	while (*s) {
		if (*s == '\\' && s[1]) {
			buf_putc(&b, s[1]);
			s += 2;
			continue;
		}
		buf_putc(&b, *s++);
	}
	return buf_take(&b);
}

static int is_dir(const char *path)
{
	struct stat st;

	return stat(*path ? path : ".", &st) == 0 && S_ISDIR(st.st_mode);
}

static void walk(const char *prefix, char **segs, size_t nseg, size_t i, Vec *out);

/* Collect the entries of prefix that match seg. */
static void match_dir(const char *prefix, const char *seg, char **segs, size_t nseg,
		      size_t i, Vec *out)
{
	DIR *d = opendir(*prefix ? prefix : ".");
	struct dirent *e;
	Vec hits;
	size_t k;

	if (!d)
		return;
	vec_init(&hits);
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.' && !sh.shopt.dotglob && seg[0] != '.')
			continue;
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (!glob_match(seg, e->d_name, sh.shopt.nocaseglob))
			continue;
		vec_pushs(&hits, e->d_name);
	}
	closedir(d);
	vec_sort(&hits);
	for (k = 0; k < hits.len; k++) {
		char *joined = xasprintf("%s%s", prefix, hits.v[k]);
		walk(joined, segs, nseg, i + 1, out);
		free(joined);
	}
	vec_free(&hits);
}

/* ** : this directory and, recursively, every subdirectory. */
static void globstar(const char *prefix, char **segs, size_t nseg, size_t i, Vec *out)
{
	DIR *d;
	struct dirent *e;
	Vec subs;
	size_t k;

	walk(prefix, segs, nseg, i + 1, out);

	d = opendir(*prefix ? prefix : ".");
	if (!d)
		return;
	vec_init(&subs);
	while ((e = readdir(d))) {
		char *path;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (e->d_name[0] == '.' && !sh.shopt.dotglob)
			continue;
		path = xasprintf("%s%s", prefix, e->d_name);
		if (is_dir(path))
			vec_push(&subs, path);
		else
			free(path);
	}
	closedir(d);
	vec_sort(&subs);
	for (k = 0; k < subs.len; k++) {
		char *with_slash = xasprintf("%s/", subs.v[k]);
		globstar(with_slash, segs, nseg, i, out);
		free(with_slash);
	}
	vec_free(&subs);
}

static void walk(const char *prefix, char **segs, size_t nseg, size_t i, Vec *out)
{
	if (i >= nseg) {
		struct stat st;
		if (lstat(*prefix ? prefix : ".", &st) == 0 || stat(prefix, &st) == 0)
			vec_pushs(out, prefix);
		return;
	}
	if (!*segs[i]) { /* an empty segment is a slash */
		char *joined = xasprintf("%s/", prefix);
		if (i + 1 >= nseg) {
			if (is_dir(joined))
				vec_push(out, joined);
			else
				free(joined);
			return;
		}
		walk(joined, segs, nseg, i + 1, out);
		free(joined);
		return;
	}
	if (i > 0 || *prefix) {
		if (!is_dir(prefix) && *prefix)
			return;
	}
	{
		char *dirprefix = *prefix && !str_suffix(prefix, "/") ? xasprintf("%s/", prefix)
								     : xstrdup(prefix);

		if (sh.shopt.globstar && strcmp(segs[i], "**") == 0) {
			globstar(dirprefix, segs, nseg, i, out);
		} else if (seg_has_meta(segs[i])) {
			match_dir(dirprefix, segs[i], segs, nseg, i, out);
		} else {
			char *lit = unescape(segs[i]);
			char *joined = xasprintf("%s%s", dirprefix, lit);
			struct stat st;

			if (i + 1 >= nseg) {
				if (lstat(joined, &st) == 0 || stat(joined, &st) == 0)
					vec_pushs(out, joined);
			} else if (is_dir(joined)) {
				walk(joined, segs, nseg, i + 1, out);
			}
			free(lit);
			free(joined);
		}
		free(dirprefix);
	}
}

/* Split on unescaped slashes. */
static void split_path(const char *pattern, Vec *segs)
{
	Buf cur;
	const char *p = pattern;

	buf_init(&cur);
	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			buf_putc(&cur, *p);
			buf_putc(&cur, p[1]);
			p++;
			continue;
		}
		if (*p == '/') {
			vec_push(segs, buf_take(&cur));
			buf_init(&cur);
			continue;
		}
		buf_putc(&cur, *p);
	}
	vec_push(segs, buf_take(&cur));
}

size_t glob_expand(const char *pattern, Vec *out)
{
	Vec segs, results;
	size_t before = out->len;
	size_t i;
	int absolute = pattern[0] == '/';

	vec_init(&segs);
	vec_init(&results);
	split_path(pattern, &segs);

	if (absolute) {
		/* the first segment is empty; start from "/" */
		walk("/", segs.v + 1, segs.len - 1, 0, &results);
	} else {
		walk("", segs.v, segs.len, 0, &results);
	}

	vec_sort(&results);
	for (i = 0; i < results.len; i++)
		vec_pushs(out, results.v[i]);

	vec_free(&segs);
	vec_free(&results);
	return out->len - before;
}
