/* expand.c - brace, tilde, parameter, command and arithmetic expansion,
 * word splitting and pathname expansion.
 *
 * Expansion carries a flag byte next to every character it produces.  A
 * character that came out of quotes is marked XF_QUOTED and is then invisible
 * to field splitting and to globbing, which is the whole reason quoting works.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

/* --------------------------------------------------------------- XBuf glue */

void xbuf_init(XBuf *x)
{
	buf_init(&x->s);
	buf_init(&x->f);
}

void xbuf_free(XBuf *x)
{
	buf_free(&x->s);
	buf_free(&x->f);
}

void xbuf_putc(XBuf *x, int c, unsigned flag)
{
	buf_putc(&x->s, c);
	buf_putc(&x->f, (char)flag);
}

void xbuf_put(XBuf *x, const char *s, size_t n, unsigned flag)
{
	size_t i;

	for (i = 0; i < n; i++)
		xbuf_putc(x, (unsigned char)s[i], flag);
}

void xbuf_puts(XBuf *x, const char *s, unsigned flag)
{
	xbuf_put(x, s, strlen(s), flag);
}

/* Process substitutions opened while expanding the current command. */
static int psub_fds[32];
static int psub_count;

/* Counts "$@"-style list expansions so that an empty one produces no field
 * at all, while a plain "" still produces one empty field. */
static unsigned long atlist_count;

void expand_close_psubs(void);
void expand_close_psubs(void)
{
	int i;

	for (i = 0; i < psub_count; i++)
		close(psub_fds[i]);
	psub_count = 0;
}

/* ---------------------------------------------------------- ANSI-C quoting */

static void ansi_c_expand(const char *s, size_t len, XBuf *out, unsigned flag)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (s[i] != '\\' || i + 1 >= len) {
			xbuf_putc(out, (unsigned char)s[i], flag);
			continue;
		}
		i++;
		switch (s[i]) {
		case 'a': xbuf_putc(out, '\a', flag); break;
		case 'b': xbuf_putc(out, '\b', flag); break;
		case 'e':
		case 'E': xbuf_putc(out, 0x1b, flag); break;
		case 'f': xbuf_putc(out, '\f', flag); break;
		case 'n': xbuf_putc(out, '\n', flag); break;
		case 'r': xbuf_putc(out, '\r', flag); break;
		case 't': xbuf_putc(out, '\t', flag); break;
		case 'v': xbuf_putc(out, '\v', flag); break;
		case '\\': xbuf_putc(out, '\\', flag); break;
		case '\'': xbuf_putc(out, '\'', flag); break;
		case '"': xbuf_putc(out, '"', flag); break;
		case '?': xbuf_putc(out, '?', flag); break;
		case 'x': {
			int v = 0, n = 0;
			while (n < 2 && i + 1 < len && isxdigit((unsigned char)s[i + 1])) {
				int c = tolower((unsigned char)s[++i]);
				v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
				n++;
			}
			xbuf_putc(out, v, flag);
			break;
		}
		case 'u':
		case 'U': {
			int want = s[i] == 'u' ? 4 : 8;
			unsigned long v = 0;
			int n = 0;
			while (n < want && i + 1 < len && isxdigit((unsigned char)s[i + 1])) {
				int c = tolower((unsigned char)s[++i]);
				v = v * 16 + (unsigned long)(isdigit(c) ? c - '0' : c - 'a' + 10);
				n++;
			}
			/* UTF-8 encode */
			if (v < 0x80) {
				xbuf_putc(out, (int)v, flag);
			} else if (v < 0x800) {
				xbuf_putc(out, (int)(0xc0 | (v >> 6)), flag);
				xbuf_putc(out, (int)(0x80 | (v & 0x3f)), flag);
			} else if (v < 0x10000) {
				xbuf_putc(out, (int)(0xe0 | (v >> 12)), flag);
				xbuf_putc(out, (int)(0x80 | ((v >> 6) & 0x3f)), flag);
				xbuf_putc(out, (int)(0x80 | (v & 0x3f)), flag);
			} else {
				xbuf_putc(out, (int)(0xf0 | (v >> 18)), flag);
				xbuf_putc(out, (int)(0x80 | ((v >> 12) & 0x3f)), flag);
				xbuf_putc(out, (int)(0x80 | ((v >> 6) & 0x3f)), flag);
				xbuf_putc(out, (int)(0x80 | (v & 0x3f)), flag);
			}
			break;
		}
		case 'c':
			if (i + 1 < len) {
				int c = (unsigned char)s[++i];
				xbuf_putc(out, toupper(c) ^ 0x40, flag);
			}
			break;
		default:
			if (s[i] >= '0' && s[i] <= '7') {
				int v = 0, n = 0;
				while (n < 3 && i < len && s[i] >= '0' && s[i] <= '7') {
					v = v * 8 + (s[i] - '0');
					i++;
					n++;
				}
				i--;
				xbuf_putc(out, v, flag);
			} else {
				xbuf_putc(out, '\\', flag);
				xbuf_putc(out, (unsigned char)s[i], flag);
			}
			break;
		}
	}
}

/* ------------------------------------------------------- brace expansion */

/* Find the matching close brace, skipping quotes and nested braces. */
static const char *find_close_brace(const char *s)
{
	int depth = 0;

	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			continue;
		}
		if (*s == '\'') {
			s++;
			while (*s && *s != '\'')
				s++;
			if (!*s)
				return NULL;
			continue;
		}
		if (*s == '"') {
			s++;
			while (*s && *s != '"') {
				if (*s == '\\' && s[1])
					s++;
				s++;
			}
			if (!*s)
				return NULL;
			continue;
		}
		if (*s == '$' && s[1] == '(') {
			int d = 0;
			s++;
			for (; *s; s++) {
				if (*s == '(')
					d++;
				else if (*s == ')' && --d == 0)
					break;
			}
			if (!*s)
				return NULL;
			continue;
		}
		if (*s == '{')
			depth++;
		else if (*s == '}') {
			if (depth == 0)
				return s;
			depth--;
		}
	}
	return NULL;
}

/* Split "a,b{c,d},e" on top level commas. */
static int split_alternatives(const char *s, const char *end, Vec *out)
{
	const char *start = s;
	int depth = 0, found = 0;

	for (; s < end; s++) {
		if (*s == '\\' && s + 1 < end) {
			s++;
			continue;
		}
		if (*s == '\'') {
			s++;
			while (s < end && *s != '\'')
				s++;
			continue;
		}
		if (*s == '"') {
			s++;
			while (s < end && *s != '"') {
				if (*s == '\\' && s + 1 < end)
					s++;
				s++;
			}
			continue;
		}
		if (*s == '{')
			depth++;
		else if (*s == '}')
			depth--;
		else if (*s == ',' && depth == 0) {
			vec_push(out, xstrndup(start, (size_t)(s - start)));
			start = s + 1;
			found = 1;
		}
	}
	vec_push(out, xstrndup(start, (size_t)(end - start)));
	return found;
}

/* {1..10..2} and {a..z} */
static int expand_sequence(const char *s, const char *end, Vec *out)
{
	char lo[64], hi[64];
	const char *dots1, *dots2, *p;
	long step = 1, a, b;
	int width = 0;

	dots1 = NULL;
	for (p = s; p + 1 < end; p++) {
		if (p[0] == '.' && p[1] == '.') {
			dots1 = p;
			break;
		}
	}
	if (!dots1)
		return 0;
	dots2 = NULL;
	for (p = dots1 + 2; p + 1 < end; p++) {
		if (p[0] == '.' && p[1] == '.') {
			dots2 = p;
			break;
		}
	}
	if ((size_t)(dots1 - s) >= sizeof lo)
		return 0;
	memcpy(lo, s, (size_t)(dots1 - s));
	lo[dots1 - s] = '\0';
	{
		const char *hend = dots2 ? dots2 : end;
		if ((size_t)(hend - (dots1 + 2)) >= sizeof hi)
			return 0;
		memcpy(hi, dots1 + 2, (size_t)(hend - (dots1 + 2)));
		hi[hend - (dots1 + 2)] = '\0';
	}
	if (dots2) {
		char stepbuf[64];
		if ((size_t)(end - (dots2 + 2)) >= sizeof stepbuf)
			return 0;
		memcpy(stepbuf, dots2 + 2, (size_t)(end - (dots2 + 2)));
		stepbuf[end - (dots2 + 2)] = '\0';
		step = strtol(stepbuf, NULL, 10);
		if (step == 0)
			step = 1;
	}
	if (!*lo || !*hi)
		return 0;

	/* character range */
	if (!isdigit((unsigned char)lo[0]) && lo[1] == '\0' && hi[1] == '\0' &&
	    !isdigit((unsigned char)hi[0])) {
		int c = lo[0], e = hi[0];
		long inc = step < 0 ? -step : step;

		if (c <= e)
			for (; c <= e; c += (int)inc)
				vec_push(out, xasprintf("%c", c));
		else
			for (; c >= e; c -= (int)inc)
				vec_push(out, xasprintf("%c", c));
		return 1;
	}

	{
		char *endp;
		a = strtol(lo, &endp, 10);
		if (*endp)
			return 0;
		b = strtol(hi, &endp, 10);
		if (*endp)
			return 0;
	}
	if ((lo[0] == '0' && lo[1]) || (hi[0] == '0' && hi[1]) ||
	    (lo[0] == '-' && lo[1] == '0' && lo[2])) {
		size_t la = strlen(lo), lb = strlen(hi);
		width = (int)(la > lb ? la : lb);
	}
	if (step < 0)
		step = -step;
	if (a <= b) {
		for (; a <= b; a += step)
			vec_push(out, width ? xasprintf("%0*ld", width, a) : xasprintf("%ld", a));
	} else {
		for (; a >= b; a -= step)
			vec_push(out, width ? xasprintf("%0*ld", width, a) : xasprintf("%ld", a));
	}
	return 1;
}

static void brace_expand(const char *s, Vec *out)
{
	const char *p = s;
	const char *open = NULL;

	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == '\'') {
			p++;
			while (*p && *p != '\'')
				p++;
			if (!*p)
				break;
			continue;
		}
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1])
					p++;
				p++;
			}
			if (!*p)
				break;
			continue;
		}
		if (*p == '$' && p[1] == '{') {
			const char *e;
			p += 1;
			e = find_close_brace(p + 1);
			if (!e)
				break;
			p = e;
			continue;
		}
		if (*p == '{') {
			open = p;
			break;
		}
	}
	if (!open) {
		vec_pushs(out, s);
		return;
	}
	{
		const char *close = find_close_brace(open + 1);
		Vec alts;
		size_t i;
		char *prefix;
		const char *suffix;

		if (!close) {
			vec_pushs(out, s);
			return;
		}
		vec_init(&alts);
		if (!expand_sequence(open + 1, close, &alts)) {
			vec_clear(&alts);
			if (!split_alternatives(open + 1, close, &alts)) {
				/* a single word in braces is not a brace expansion */
				vec_free(&alts);
				{
					char *head = xstrndup(s, (size_t)(close + 1 - s));
					Vec rest;
					vec_init(&rest);
					brace_expand(close + 1, &rest);
					for (i = 0; i < rest.len; i++)
						vec_push(out, xasprintf("%s%s", head, rest.v[i]));
					vec_free(&rest);
					free(head);
				}
				return;
			}
		}
		prefix = xstrndup(s, (size_t)(open - s));
		suffix = close + 1;
		for (i = 0; i < alts.len; i++) {
			char *joined = xasprintf("%s%s%s", prefix, alts.v[i], suffix);
			brace_expand(joined, out);
			free(joined);
		}
		free(prefix);
		vec_free(&alts);
	}
}

/* ------------------------------------------------------- tilde expansion */

static char *tilde_expand(const char *s, size_t *consumed)
{
	size_t i = 1;
	char *name;
	const char *val;

	while (s[i] && s[i] != '/' && s[i] != ':')
		i++;
	*consumed = i;
	if (i == 1) {
		val = var_get("HOME");
		return xstrdup(val ? val : "");
	}
	name = xstrndup(s + 1, i - 1);
	if (strcmp(name, "+") == 0) {
		val = var_get("PWD");
		free(name);
		return xstrdup(val ? val : "");
	}
	if (strcmp(name, "-") == 0) {
		val = var_get("OLDPWD");
		free(name);
		return xstrdup(val ? val : "");
	}
	{
		struct passwd *pw = getpwnam(name);
		if (pw) {
			free(name);
			return xstrdup(pw->pw_dir);
		}
	}
	free(name);
	*consumed = 0; /* not a user: leave the text alone */
	return NULL;
}

/* ---------------------------------------------------- pattern match helpers */

/* Longest or shortest prefix of str matching pat; returns length or -1. */
static long match_prefix(const char *str, const char *pat, int longest)
{
	size_t len = strlen(str);
	long best = -1;
	size_t i;

	for (i = 0; i <= len; i++) {
		char save = ((char *)str)[i];
		((char *)str)[i] = '\0';
		if (glob_match(pat, str, sh.shopt.nocasematch)) {
			((char *)str)[i] = save;
			if (!longest)
				return (long)i;
			best = (long)i;
		} else {
			((char *)str)[i] = save;
		}
	}
	return best;
}

static long match_suffix(const char *str, const char *pat, int longest)
{
	size_t len = strlen(str);
	long best = -1;
	long i;

	for (i = (long)len; i >= 0; i--) {
		if (glob_match(pat, str + i, sh.shopt.nocasematch)) {
			if (!longest)
				return i;
			best = i;
		}
	}
	return best;
}

/* Replace matches of pat in str.  mode: 0 first, 1 all, 2 anchor head,
 * 3 anchor tail. */
static char *pattern_replace(const char *str, const char *pat, const char *rep, int mode)
{
	Buf out;
	size_t len = strlen(str);
	size_t i = 0;
	char *work = xstrdup(str);
	int replaced = 0;

	buf_init(&out);
	if (mode == 2) {
		long n = match_prefix(work, pat, 1);
		if (n >= 0) {
			buf_puts(&out, rep);
			buf_puts(&out, work + n);
		} else {
			buf_puts(&out, work);
		}
		free(work);
		return buf_take(&out);
	}
	if (mode == 3) {
		long n = match_suffix(work, pat, 1);
		if (n >= 0 && glob_match(pat, work + n, sh.shopt.nocasematch)) {
			buf_put(&out, work, (size_t)n);
			buf_puts(&out, rep);
		} else {
			buf_puts(&out, work);
		}
		free(work);
		return buf_take(&out);
	}

	while (i <= len) {
		long n;

		if (replaced && mode == 0) {
			buf_puts(&out, work + i);
			break;
		}
		n = match_prefix(work + i, pat, 1);
		if (n > 0) {
			buf_puts(&out, rep);
			i += (size_t)n;
			replaced = 1;
			continue;
		}
		if (n == 0 && !replaced && work[i] == '\0') {
			buf_puts(&out, rep);
			replaced = 1;
			break;
		}
		if (work[i] == '\0')
			break;
		buf_putc(&out, work[i]);
		i++;
	}
	free(work);
	return buf_take(&out);
}

/* -------------------------------------------------------- the main walker */

static void expand_into(const char *s, XBuf *out, int in_dquote);

static char *substitute_command(const char *body)
{
	int status = 0;
	char *result = capture_string(body, &status);

	sh.last_status = status;
	return result;
}

/* <(cmd) and >(cmd): run cmd with one end of a pipe, hand back /dev/fd/N. */
static char *process_substitution(const char *body, int writing)
{
	int fds[2];
	pid_t pid;

	if (pipe(fds) < 0) {
		shell_error("process substitution: %s", strerror(errno));
		return xstrdup("/dev/null");
	}
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		shell_error("process substitution: %s", strerror(errno));
		return xstrdup("/dev/null");
	}
	if (pid == 0) {
		char *err = NULL;
		Node *n;

		sh.subshell++;
		traps_reset_in_child();
		if (writing) {
			close(fds[1]);
			dup2(fds[0], 0);
			close(fds[0]);
		} else {
			close(fds[0]);
			dup2(fds[1], 1);
			close(fds[1]);
		}
		n = parse_string(body, "process substitution", &err);
		if (err) {
			shell_error("%s", err);
			_exit(2);
		}
		exec_node(n);
		node_free(n);
		_exit(sh.last_status);
	}
	if (writing) {
		close(fds[0]);
		if (psub_count < (int)(sizeof psub_fds / sizeof psub_fds[0]))
			psub_fds[psub_count++] = fds[1];
		return xasprintf("/dev/fd/%d", fds[1]);
	}
	close(fds[1]);
	if (psub_count < (int)(sizeof psub_fds / sizeof psub_fds[0]))
		psub_fds[psub_count++] = fds[0];
	return xasprintf("/dev/fd/%d", fds[0]);
}

/* Emit a value, honouring quoting. */
static void emit(XBuf *out, const char *val, int quoted)
{
	if (!val)
		return;
	xbuf_puts(out, val, quoted ? XF_QUOTED : 0);
}

static void emit_list(XBuf *out, Vec *items, int quoted, int star)
{
	size_t i;
	const char *ifs = var_get("IFS");
	char sep[2];

	sep[0] = ifs && *ifs ? ifs[0] : ' ';
	sep[1] = '\0';

	if (quoted && !star)
		atlist_count++;

	for (i = 0; i < items->len; i++) {
		if (i) {
			if (quoted && star)
				xbuf_puts(out, sep, XF_QUOTED);
			else if (quoted)
				xbuf_putc(out, '\0', XF_SPLIT);
			else
				xbuf_putc(out, ' ', 0);
		}
		if (quoted && !star)
			xbuf_putc(out, '\0', XF_EMPTY);
		emit(out, items->v[i], quoted);
	}
}

/* Read a NAME[subscript] reference from s; advances *pp. */
static char *read_ref(const char **pp, char **subscript)
{
	const char *p = *pp;
	const char *start = p;
	char *name;

	*subscript = NULL;
	while (is_name_char((unsigned char)*p, p == start))
		p++;
	if (p == start)
		return NULL;
	name = xstrndup(start, (size_t)(p - start));
	if (*p == '[') {
		int depth = 1;
		const char *sub = ++p;
		while (*p && depth) {
			if (*p == '[')
				depth++;
			else if (*p == ']' && --depth == 0)
				break;
			p++;
		}
		*subscript = xstrndup(sub, (size_t)(p - sub));
		if (*p == ']')
			p++;
	}
	*pp = p;
	return name;
}

/* Positional and special parameters that are not names. */
static char *special_param(int c, int *is_list)
{
	char buf[32];

	*is_list = 0;
	switch (c) {
	case '?':
		snprintf(buf, sizeof buf, "%d", sh.last_status);
		return xstrdup(buf);
	case '$':
		snprintf(buf, sizeof buf, "%ld", (long)sh.pid);
		return xstrdup(buf);
	case '!':
		snprintf(buf, sizeof buf, "%ld", (long)sh.last_bg_pid);
		return xstrdup(buf);
	case '#':
		snprintf(buf, sizeof buf, "%zu", args_count());
		return xstrdup(buf);
	case '0':
		return xstrdup(sh.script_name ? sh.script_name : "crish");
	case '-': {
		Buf b;
		buf_init(&b);
		if (sh.opt.errexit)
			buf_putc(&b, 'e');
		if (sh.opt.nounset)
			buf_putc(&b, 'u');
		if (sh.opt.xtrace)
			buf_putc(&b, 'x');
		if (sh.opt.noglob)
			buf_putc(&b, 'f');
		if (sh.interactive)
			buf_putc(&b, 'i');
		if (sh.opt.monitor)
			buf_putc(&b, 'm');
		if (sh.opt.hashall)
			buf_putc(&b, 'h');
		return buf_take(&b);
	}
	default:
		break;
	}
	return NULL;
}

/* Everything inside ${ ... }. */
static void expand_braced(const char *body, XBuf *out, int in_dquote)
{
	const char *p = body;
	char *name = NULL, *sub = NULL;
	int want_length = 0, indirect = 0;
	const char *val = NULL;
	char *owned = NULL;
	int is_at = 0, is_star = 0;
	int quoted = in_dquote;

	if (*p == '#' && p[1]) {
		want_length = 1;
		p++;
	} else if (*p == '!' && p[1]) {
		indirect = 1;
		p++;
	}

	/* ${!prefix*} and ${!prefix@}: names beginning with prefix */
	if (indirect) {
		size_t plen = strlen(p);
		if (plen > 1 && (p[plen - 1] == '*' || p[plen - 1] == '@')) {
			char *prefix = xstrndup(p, plen - 1);
			Vec all, hits;
			size_t i;

			vec_init(&all);
			vec_init(&hits);
			vars_all(&all);
			for (i = 0; i < all.len; i++)
				if (str_prefix(all.v[i], prefix))
					vec_pushs(&hits, all.v[i]);
			emit_list(out, &hits, quoted, p[plen - 1] == '*');
			vec_free(&all);
			vec_free(&hits);
			free(prefix);
			return;
		}
	}

	if (*p == '@' || *p == '*') {
		is_at = *p == '@';
		is_star = *p == '*';
		name = xstrdup(is_at ? "@" : "*");
		p++;
	} else if (!is_name_char((unsigned char)*p, 1)) {
		if (isdigit((unsigned char)*p)) {
			const char *start = p;
			while (isdigit((unsigned char)*p))
				p++;
			name = xstrndup(start, (size_t)(p - start));
		} else {
			name = xstrndup(p, 1);
			p++;
		}
	} else {
		name = read_ref(&p, &sub);
	}

	if (!name) {
		shell_error("bad substitution: ${%s}", body);
		return;
	}

	/* ${!a[@]} asks for the keys, so it is not an indirect reference. */
	if (indirect && sub && (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0)) {
		/* fall through to the array branch below */
	} else if (indirect) {
		const char *target = var_get(name);
		if (target && *target) {
			char *inner = xstrdup(target);
			free(name);
			name = inner;
			free(sub);
			sub = NULL;
			{
				const char *q = name;
				char *s2;
				char *n2 = read_ref(&q, &s2);
				if (n2) {
					free(name);
					name = n2;
					sub = s2;
				}
			}
		} else {
			free(name);
			name = xstrdup("");
		}
	}

	if (strcmp(name, "@") == 0 || strcmp(name, "*") == 0) {
		is_at = strcmp(name, "@") == 0;
		is_star = !is_at;
	}

	/* ${a[@]} / ${a[*]} */
	if (sub && (strcmp(sub, "@") == 0 || strcmp(sub, "*") == 0)) {
		int star = strcmp(sub, "*") == 0;
		Vec vals;

		vec_init(&vals);
		if (want_length) {
			char buf[32];
			snprintf(buf, sizeof buf, "%zu", var_array_count(name));
			emit(out, buf, quoted);
			vec_free(&vals);
			goto done;
		}
		if (indirect) {
			var_array_keys(name, &vals);
		} else {
			var_array_values(name, &vals);
		}
		/* an operator may follow, but the common case is a bare list */
		if (!*p) {
			emit_list(out, &vals, quoted, star);
			vec_free(&vals);
			goto done;
		}
		/* apply the operator to each element */
		{
			Vec res;
			size_t i;

			vec_init(&res);
			for (i = 0; i < vals.len; i++) {
				Buf piece;
				char *sub_expr;
				XBuf tmp;

				buf_init(&piece);
				buf_printf(&piece, "__crish_elem%s", p);
				var_set("__crish_elem", vals.v[i], 0);
				sub_expr = buf_take(&piece);
				xbuf_init(&tmp);
				expand_braced(sub_expr, &tmp, 1);
				vec_push(&res, xstrndup(tmp.s.b ? tmp.s.b : "", tmp.s.len));
				xbuf_free(&tmp);
				free(sub_expr);
			}
			var_unset("__crish_elem");
			emit_list(out, &res, quoted, star);
			vec_free(&res);
			vec_free(&vals);
			goto done;
		}
	}

	if (is_at || is_star) {
		Vec vals;
		size_t i;

		vec_init(&vals);
		for (i = 1; i <= args_count(); i++)
			vec_pushs(&vals, args_get(i));
		if (want_length) {
			char buf[32];
			snprintf(buf, sizeof buf, "%zu", args_count());
			emit(out, buf, quoted);
		} else if (!*p) {
			emit_list(out, &vals, quoted, is_star);
		} else {
			Vec res;
			vec_init(&res);
			for (i = 0; i < vals.len; i++) {
				char *sub_expr = xasprintf("__crish_elem%s", p);
				XBuf tmp;
				var_set("__crish_elem", vals.v[i], 0);
				xbuf_init(&tmp);
				expand_braced(sub_expr, &tmp, 1);
				vec_push(&res, xstrndup(tmp.s.b ? tmp.s.b : "", tmp.s.len));
				xbuf_free(&tmp);
				free(sub_expr);
			}
			var_unset("__crish_elem");
			emit_list(out, &res, quoted, is_star);
			vec_free(&res);
		}
		vec_free(&vals);
		goto done;
	}

	if (sub) {
		char *idx = expand_to_string(sub);
		val = var_array_get(name, idx);
		free(idx);
	} else if (isdigit((unsigned char)name[0])) {
		val = args_get((size_t)strtoul(name, NULL, 10));
		if (strcmp(name, "0") == 0)
			val = sh.script_name ? sh.script_name : "crish";
	} else if (!is_name_char((unsigned char)name[0], 1)) {
		int list;
		owned = special_param((unsigned char)name[0], &list);
		val = owned;
	} else {
		val = var_get(name);
	}

	if (want_length) {
		char buf[32];
		snprintf(buf, sizeof buf, "%zu", val ? strlen(val) : 0);
		emit(out, buf, quoted);
		goto done;
	}

	if (!*p) {
		if (!val && sh.opt.nounset && is_valid_name(name)) {
			shell_error("%s: unbound variable", name);
			if (!sh.interactive)
				exit(1);
		}
		emit(out, val, quoted);
		goto done;
	}

	/* Operators */
	{
		int colon = 0;
		char op;
		const char *arg;

		if (*p == ':' && p[1] && !strchr("-=?+", p[1])) {
			/* ${v:offset:length} */
			long off, l = -1;
			const char *colon2 = NULL;
			int depth = 0;
			const char *q;

			for (q = p + 1; *q; q++) {
				if (*q == '(' || *q == '[')
					depth++;
				else if (*q == ')' || *q == ']')
					depth--;
				else if (*q == ':' && depth == 0) {
					colon2 = q;
					break;
				}
			}
			{
				char *offs = colon2 ? xstrndup(p + 1, (size_t)(colon2 - p - 1))
						    : xstrdup(p + 1);
				int ok = 1;
				off = arith_eval(offs, &ok);
				free(offs);
				if (colon2) {
					char *lens = xstrdup(colon2 + 1);
					l = arith_eval(lens, &ok);
					free(lens);
				}
			}
			{
				size_t vlen = val ? strlen(val) : 0;
				long start = off;
				if (start < 0)
					start = (long)vlen + start;
				if (start < 0)
					start = 0;
				if ((size_t)start > vlen)
					start = (long)vlen;
				{
					long avail = (long)vlen - start;
					long take = colon2 ? l : avail;
					if (colon2 && take < 0)
						take = avail + take;
					if (take < 0)
						take = 0;
					if (take > avail)
						take = avail;
					if (val)
						xbuf_put(out, val + start, (size_t)take,
							 quoted ? XF_QUOTED : 0);
				}
			}
			goto done;
		}

		if (*p == ':') {
			colon = 1;
			p++;
		}
		op = *p;
		arg = p + 1;

		switch (op) {
		case '-': {
			int empty = !val || (colon && !*val);
			if (empty) {
				XBuf tmp;
				xbuf_init(&tmp);
				expand_into(arg, &tmp, in_dquote);
				buf_put(&out->s, tmp.s.b ? tmp.s.b : "", tmp.s.len);
				buf_put(&out->f, tmp.f.b ? tmp.f.b : "", tmp.f.len);
				xbuf_free(&tmp);
			} else {
				emit(out, val, quoted);
			}
			break;
		}
		case '=': {
			int empty = !val || (colon && !*val);
			if (empty) {
				char *fresh = expand_to_string(arg);
				if (sub) {
					char *idx = expand_to_string(sub);
					var_array_set(name, idx, fresh, 0);
					free(idx);
				} else {
					var_set(name, fresh, 0);
				}
				emit(out, fresh, quoted);
				free(fresh);
			} else {
				emit(out, val, quoted);
			}
			break;
		}
		case '?': {
			int empty = !val || (colon && !*val);
			if (empty) {
				char *msg = *arg ? expand_to_string(arg) : NULL;
				shell_error("%s: %s", name,
					    msg && *msg ? msg : "parameter null or not set");
				free(msg);
				if (!sh.interactive) {
					sh.exit_requested = 1;
					sh.exit_code = 1;
				}
				sh.last_status = 1;
			} else {
				emit(out, val, quoted);
			}
			break;
		}
		case '+': {
			int empty = !val || (colon && !*val);
			if (!empty) {
				XBuf tmp;
				xbuf_init(&tmp);
				expand_into(arg, &tmp, in_dquote);
				buf_put(&out->s, tmp.s.b ? tmp.s.b : "", tmp.s.len);
				buf_put(&out->f, tmp.f.b ? tmp.f.b : "", tmp.f.len);
				xbuf_free(&tmp);
			}
			break;
		}
		case '#':
		case '%': {
			int longest = arg[0] == op;
			char *pat = expand_unsplit(longest ? arg + 1 : arg);
			const char *v = val ? val : "";
			char *tmp = xstrdup(v);

			if (op == '#') {
				long n = match_prefix(tmp, pat, longest);
				emit(out, n >= 0 ? tmp + n : tmp, quoted);
			} else {
				long n = match_suffix(tmp, pat, longest);
				if (n >= 0) {
					tmp[n] = '\0';
					emit(out, tmp, quoted);
				} else {
					emit(out, tmp, quoted);
				}
			}
			free(tmp);
			free(pat);
			break;
		}
		case '/': {
			int mode = 0;
			const char *q = arg;
			char *pat, *rep, *res;
			const char *slash;
			int depth = 0;

			if (*q == '/') {
				mode = 1;
				q++;
			} else if (*q == '#') {
				mode = 2;
				q++;
			} else if (*q == '%') {
				mode = 3;
				q++;
			}
			slash = NULL;
			{
				const char *r;
				for (r = q; *r; r++) {
					if (*r == '\\' && r[1]) {
						r++;
						continue;
					}
					if (*r == '[')
						depth++;
					else if (*r == ']')
						depth--;
					else if (*r == '/' && depth == 0) {
						slash = r;
						break;
					}
				}
			}
			pat = slash ? xstrndup(q, (size_t)(slash - q)) : xstrdup(q);
			{
				char *tmp = expand_unsplit(pat);
				free(pat);
				pat = tmp;
			}
			rep = slash ? expand_to_string(slash + 1) : xstrdup("");
			res = pattern_replace(val ? val : "", pat, rep, mode);
			emit(out, res, quoted);
			free(pat);
			free(rep);
			free(res);
			break;
		}
		case '^':
		case ',': {
			int all = arg[0] == op;
			const char *patsrc = all ? arg + 1 : arg;
			char *pat = *patsrc ? expand_unsplit(patsrc) : NULL;
			char *tmp = xstrdup(val ? val : "");
			size_t i;

			for (i = 0; tmp[i]; i++) {
				char one[2];
				one[0] = tmp[i];
				one[1] = '\0';
				if (pat && !glob_match(pat, one, 0))
					continue;
				tmp[i] = op == '^' ? (char)toupper((unsigned char)tmp[i])
						   : (char)tolower((unsigned char)tmp[i]);
				if (!all)
					break;
			}
			emit(out, tmp, quoted);
			free(tmp);
			free(pat);
			break;
		}
		case '@': {
			char t = arg[0];
			if (t == 'Q') {
				char *q = shell_quote(val ? val : "");
				emit(out, q, quoted);
				free(q);
			} else if (t == 'U' || t == 'L') {
				char *tmp = xstrdup(val ? val : "");
				char *c;
				for (c = tmp; *c; c++)
					*c = t == 'U' ? (char)toupper((unsigned char)*c)
						      : (char)tolower((unsigned char)*c);
				emit(out, tmp, quoted);
				free(tmp);
			} else if (t == 'A' || t == 'a') {
				Var *v = var_find(name);
				if (v) {
					char *d = t == 'A' ? var_render_decl(v, "declare")
							   : NULL;
					if (t == 'a') {
						Buf b;
						buf_init(&b);
						if (v->flags & V_ASSOC)
							buf_putc(&b, 'A');
						if (v->flags & V_ARRAY)
							buf_putc(&b, 'a');
						if (v->flags & V_INTEGER)
							buf_putc(&b, 'i');
						if (v->flags & V_READONLY)
							buf_putc(&b, 'r');
						if (v->flags & V_EXPORT)
							buf_putc(&b, 'x');
						d = buf_take(&b);
					}
					emit(out, d, quoted);
					free(d);
				}
			} else if (t == 'E') {
				XBuf tmp;
				xbuf_init(&tmp);
				ansi_c_expand(val ? val : "", val ? strlen(val) : 0, &tmp,
					      quoted ? XF_QUOTED : 0);
				buf_put(&out->s, tmp.s.b ? tmp.s.b : "", tmp.s.len);
				buf_put(&out->f, tmp.f.b ? tmp.f.b : "", tmp.f.len);
				xbuf_free(&tmp);
			} else if (t == 'P') {
				char *r = prompt_render(val ? val : "");
				emit(out, r, quoted);
				free(r);
			} else {
				emit(out, val, quoted);
			}
			break;
		}
		default:
			shell_error("bad substitution: ${%s}", body);
			break;
		}
	}

done:
	free(name);
	free(sub);
	free(owned);
}

static void expand_dollar(const char **pp, XBuf *out, int in_dquote)
{
	const char *p = *pp;
	unsigned flag = in_dquote ? XF_QUOTED : 0;

	p++; /* past '$' */

	if (*p == '{') {
		const char *close = find_close_brace(p + 1);
		char *body;

		if (!close) {
			xbuf_putc(out, '$', flag);
			*pp = p;
			return;
		}
		body = xstrndup(p + 1, (size_t)(close - p - 1));
		expand_braced(body, out, in_dquote);
		free(body);
		*pp = close + 1;
		return;
	}

	if (*p == '(' && p[1] == '(') {
		/* $(( ... )) */
		int depth = 0;
		const char *q = p;
		const char *end = NULL;

		for (; *q; q++) {
			if (*q == '(')
				depth++;
			else if (*q == ')') {
				depth--;
				if (depth == 0) {
					end = q;
					break;
				}
			}
		}
		if (end && end[-1] == ')') {
			char *expr = xstrndup(p + 2, (size_t)(end - p - 3));
			int ok = 1;
			long v = arith_eval(expr, &ok);
			char buf[32];

			snprintf(buf, sizeof buf, "%ld", v);
			xbuf_puts(out, buf, flag);
			free(expr);
			*pp = end + 1;
			return;
		}
	}

	if (*p == '(') {
		int depth = 0;
		const char *q = p;
		const char *end = NULL;
		char *body, *result;

		for (; *q; q++) {
			if (*q == '\'') {
				q++;
				while (*q && *q != '\'')
					q++;
				if (!*q)
					break;
				continue;
			}
			if (*q == '"') {
				q++;
				while (*q && *q != '"') {
					if (*q == '\\' && q[1])
						q++;
					q++;
				}
				if (!*q)
					break;
				continue;
			}
			if (*q == '(')
				depth++;
			else if (*q == ')') {
				depth--;
				if (depth == 0) {
					end = q;
					break;
				}
			}
		}
		if (!end) {
			xbuf_putc(out, '$', flag);
			*pp = p;
			return;
		}
		body = xstrndup(p + 1, (size_t)(end - p - 1));
		result = substitute_command(body);
		xbuf_puts(out, result, flag);
		free(result);
		free(body);
		*pp = end + 1;
		return;
	}

	if (*p == '\'' && !in_dquote) {
		const char *q = p + 1;
		while (*q && *q != '\'')
			q++;
		xbuf_putc(out, '\0', XF_EMPTY);
		ansi_c_expand(p + 1, (size_t)(q - p - 1), out, XF_QUOTED);
		*pp = *q ? q + 1 : q;
		return;
	}

	if (*p == '"' && !in_dquote) {
		/* $"..." is a translated string; we treat it as a plain "..." */
		const char *q = p;
		expand_into(q, out, in_dquote);
		*pp = p + strlen(p);
		return;
	}

	if (is_name_char((unsigned char)*p, 1)) {
		char *sub;
		const char *q = p;
		char *name = read_ref(&q, &sub);
		Buf body;

		buf_init(&body);
		buf_puts(&body, name);
		if (sub) {
			buf_putc(&body, '[');
			buf_puts(&body, sub);
			buf_putc(&body, ']');
		}
		{
			char *b = buf_take(&body);
			expand_braced(b, out, in_dquote);
			free(b);
		}
		free(name);
		free(sub);
		*pp = q;
		return;
	}

	if (isdigit((unsigned char)*p)) {
		char nm[2];
		nm[0] = *p;
		nm[1] = '\0';
		{
			const char *v = strcmp(nm, "0") == 0
						? (sh.script_name ? sh.script_name : "crish")
						: args_get((size_t)(*p - '0'));
			emit(out, v, in_dquote);
		}
		*pp = p + 1;
		return;
	}

	if (*p == '@' || *p == '*') {
		char body[2];
		body[0] = *p;
		body[1] = '\0';
		expand_braced(body, out, in_dquote);
		*pp = p + 1;
		return;
	}

	if (*p && strchr("?$!#-", *p)) {
		int list;
		char *v = special_param((unsigned char)*p, &list);
		emit(out, v, in_dquote);
		free(v);
		*pp = p + 1;
		return;
	}

	xbuf_putc(out, '$', flag);
	*pp = p;
}

static void expand_into(const char *s, XBuf *out, int in_dquote)
{
	const char *p = s;

	while (*p) {
		unsigned flag = in_dquote ? XF_QUOTED : 0;

		if (*p == '\\') {
			if (!p[1]) {
				xbuf_putc(out, '\\', flag);
				p++;
				continue;
			}
			if (in_dquote) {
				if (strchr("$`\"\\\n", p[1])) {
					if (p[1] != '\n')
						xbuf_putc(out, p[1], XF_QUOTED);
					p += 2;
				} else {
					xbuf_putc(out, '\\', XF_QUOTED);
					xbuf_putc(out, p[1], XF_QUOTED);
					p += 2;
				}
			} else {
				if (p[1] == '\n') {
					p += 2;
					continue;
				}
				xbuf_putc(out, p[1], XF_QUOTED);
				p += 2;
			}
			continue;
		}
		if (*p == '\'' && !in_dquote) {
			const char *q = ++p;
			while (*q && *q != '\'')
				q++;
			xbuf_putc(out, '\0', XF_EMPTY);
			xbuf_put(out, p, (size_t)(q - p), XF_QUOTED);
			p = *q ? q + 1 : q;
			continue;
		}
		if (*p == '"' && !in_dquote) {
			const char *q = p + 1;
			const char *start = q;
			char *body;
			size_t before_len;
			unsigned long before_at;

			while (*q && *q != '"') {
				if (*q == '\\' && q[1]) {
					q += 2;
					continue;
				}
				if (*q == '$' && q[1] == '{') {
					const char *close = find_close_brace(q + 2);

					if (!close)
						break;
					q = close + 1;
					continue;
				}
				if (*q == '$' && q[1] == '(') {
					int d = 0;
					q++;
					for (; *q; q++) {
						if (*q == '(')
							d++;
						else if (*q == ')' && --d == 0)
							break;
					}
					if (!*q)
						break;
					q++;
					continue;
				}
				if (*q == '`') {
					q++;
					while (*q && *q != '`') {
						if (*q == '\\' && q[1])
							q++;
						q++;
					}
					if (!*q)
						break;
					q++;
					continue;
				}
				q++;
			}
			body = xstrndup(start, (size_t)(q - start));
			before_len = out->s.len;
			before_at = atlist_count;
			expand_into(body, out, 1);
			/* "" is one empty field, but "$@" with no arguments is none */
			if (out->s.len == before_len && atlist_count == before_at)
				xbuf_putc(out, '\0', XF_EMPTY);
			free(body);
			p = *q ? q + 1 : q;
			continue;
		}
		if (*p == '$') {
			expand_dollar(&p, out, in_dquote);
			continue;
		}
		if (*p == '`') {
			const char *q = p + 1;
			Buf body;
			char *result;

			buf_init(&body);
			while (*q && *q != '`') {
				if (*q == '\\' && (q[1] == '`' || q[1] == '$' || q[1] == '\\')) {
					buf_putc(&body, q[1]);
					q += 2;
					continue;
				}
				buf_putc(&body, *q++);
			}
			{
				char *b = buf_take(&body);
				result = substitute_command(b);
				free(b);
			}
			xbuf_puts(out, result, flag);
			free(result);
			p = *q ? q + 1 : q;
			continue;
		}
		if ((*p == '<' || *p == '>') && p[1] == '(' && !in_dquote) {
			int depth = 0;
			const char *q = p + 1;
			const char *end = NULL;

			for (; *q; q++) {
				if (*q == '(')
					depth++;
				else if (*q == ')' && --depth == 0) {
					end = q;
					break;
				}
			}
			if (end) {
				char *body = xstrndup(p + 2, (size_t)(end - p - 2));
				char *path = process_substitution(body, *p == '>');
				xbuf_puts(out, path, 0);
				free(path);
				free(body);
				p = end + 1;
				continue;
			}
		}
		xbuf_putc(out, *p, flag);
		p++;
	}
}

/* ---------------------------------------------------- splitting and globbing */

static int in_ifs(int c, const char *ifs)
{
	return c && ifs && strchr(ifs, c) != NULL;
}

static int ifs_white(int c, const char *ifs)
{
	return (c == ' ' || c == '\t' || c == '\n') && in_ifs(c, ifs);
}

/* Turn one expanded run into fields, honouring the flag bytes. */
static void split_fields(XBuf *x, Vec *out, int do_split)
{
	const char *s = x->s.b ? x->s.b : "";
	const unsigned char *f = (const unsigned char *)(x->f.b ? x->f.b : "");
	size_t len = x->s.len;
	const char *ifs = var_get("IFS");
	Buf field;
	int have_field = 0;
	size_t i;

	if (!ifs)
		ifs = " \t\n";

	buf_init(&field);
	for (i = 0; i < len; i++) {
		unsigned flag = f[i];

		if (flag & XF_EMPTY) {
			have_field = 1;
			continue;
		}
		if (flag & XF_SPLIT) {
			vec_push(out, buf_take(&field));
			buf_init(&field);
			have_field = 1;
			continue;
		}
		if (!(flag & XF_QUOTED) && do_split && in_ifs((unsigned char)s[i], ifs)) {
			int white = ifs_white((unsigned char)s[i], ifs);

			if (field.len || have_field) {
				vec_push(out, buf_take(&field));
				buf_init(&field);
				have_field = 0;
			}
			if (white) {
				while (i + 1 < len && !(f[i + 1] & XF_QUOTED) &&
				       ifs_white((unsigned char)s[i + 1], ifs))
					i++;
			} else {
				/* a non-whitespace IFS char delimits exactly one field */
				while (i + 1 < len && !(f[i + 1] & XF_QUOTED) &&
				       ifs_white((unsigned char)s[i + 1], ifs))
					i++;
				have_field = i + 1 < len;
			}
			continue;
		}
		if (flag & XF_QUOTED) {
			/* protect glob metacharacters that came out of quotes */
			if (strchr("*?[]\\", s[i]))
				buf_putc(&field, '\\');
			have_field = 1;
		}
		buf_putc(&field, s[i]);
	}
	if (field.len || have_field)
		vec_push(out, buf_take(&field));
	else
		buf_free(&field);
}

char *unquote(const char *text)
{
	Buf b;
	const char *p = text;

	buf_init(&b);
	while (*p) {
		if (*p == '\\' && p[1]) {
			buf_putc(&b, p[1]);
			p += 2;
			continue;
		}
		if (*p == '\'') {
			p++;
			while (*p && *p != '\'')
				buf_putc(&b, *p++);
			if (*p)
				p++;
			continue;
		}
		if (*p == '"') {
			p++;
			while (*p && *p != '"') {
				if (*p == '\\' && p[1] && strchr("$`\"\\", p[1])) {
					buf_putc(&b, p[1]);
					p += 2;
					continue;
				}
				buf_putc(&b, *p++);
			}
			if (*p)
				p++;
			continue;
		}
		buf_putc(&b, *p++);
	}
	return buf_take(&b);
}

/* Remove the backslashes split_fields added around quoted metacharacters. */
static char *deglob(const char *s)
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

/* An unmatched [ is a literal bracket, not a pattern; without this a bare
 * `[ ... ]` test would vanish under nullglob. */
static int bracket_is_closed(const char *s)
{
	const char *p = s + 1;

	if (*p == '!' || *p == '^')
		p++;
	if (*p == ']')
		p++;
	for (; *p; p++) {
		if (*p == '\\' && p[1]) {
			p++;
			continue;
		}
		if (*p == ']')
			return 1;
	}
	return 0;
}

static int has_glob_meta(const char *s)
{
	for (; *s; s++) {
		if (*s == '\\' && s[1]) {
			s++;
			continue;
		}
		if (*s == '*' || *s == '?')
			return 1;
		if (*s == '[' && bracket_is_closed(s))
			return 1;
		if (sh.shopt.extglob && strchr("?*+@!", *s) && s[1] == '(')
			return 1;
	}
	return 0;
}

static void apply_tilde(const char *field, Buf *out)
{
	size_t consumed = 0;
	char *home;

	if (field[0] != '~') {
		buf_puts(out, field);
		return;
	}
	home = tilde_expand(field, &consumed);
	if (!home) {
		buf_puts(out, field);
		return;
	}
	buf_puts(out, home);
	buf_puts(out, field + consumed);
	free(home);
}

void expand_word(const char *text, Vec *out)
{
	Vec braced;
	size_t i;

	vec_init(&braced);
	brace_expand(text, &braced);

	for (i = 0; i < braced.len; i++) {
		XBuf x;
		Vec fields;
		size_t j;
		Buf tilded;

		buf_init(&tilded);
		apply_tilde(braced.v[i], &tilded);

		xbuf_init(&x);
		expand_into(tilded.b ? tilded.b : "", &x, 0);
		buf_free(&tilded);

		vec_init(&fields);
		split_fields(&x, &fields, 1);
		xbuf_free(&x);

		for (j = 0; j < fields.len; j++) {
			if (!sh.opt.noglob && has_glob_meta(fields.v[j])) {
				size_t before = out->len;
				glob_expand(fields.v[j], out);
				if (out->len == before) {
					if (sh.shopt.failglob) {
						shell_error("no match: %s", fields.v[j]);
						sh.last_status = 1;
					} else if (!sh.shopt.nullglob) {
						vec_push(out, deglob(fields.v[j]));
					}
				}
			} else {
				vec_push(out, deglob(fields.v[j]));
			}
		}
		vec_free(&fields);
	}
	vec_free(&braced);
}

void expand_words(const Vec *in, Vec *out)
{
	size_t i;

	for (i = 0; i < in->len; i++)
		expand_word(in->v[i], out);
}

char *expand_to_string(const char *text)
{
	XBuf x;
	char *s;

	xbuf_init(&x);
	expand_into(text ? text : "", &x, 0);
	s = xstrndup(x.s.b ? x.s.b : "", x.s.len);
	xbuf_free(&x);
	return s;
}


/* Expand for a context that matches a pattern: characters that came out of
 * quotes get escaped so they are literal to the matcher, everything else
 * keeps its special meaning.  `meta` lists the characters worth escaping. */
static char *expand_escaped(const char *text, const char *meta)
{
	XBuf x;
	Buf out;
	size_t i;
	const unsigned char *f;

	xbuf_init(&x);
	expand_into(text ? text : "", &x, 0);
	buf_init(&out);
	f = (const unsigned char *)(x.f.b ? x.f.b : "");
	for (i = 0; i < x.s.len; i++) {
		if (f[i] & XF_EMPTY)
			continue;
		if ((f[i] & XF_QUOTED) && strchr(meta, x.s.b[i]))
			buf_putc(&out, '\\');
		buf_putc(&out, x.s.b[i]);
	}
	xbuf_free(&x);
	return buf_take(&out);
}

char *expand_pattern(const char *text)
{
	return expand_escaped(text, "*?[]\\");
}

char *expand_regex(const char *text)
{
	return expand_escaped(text, ".*+?[](){}|^$\\");
}

char *expand_unsplit(const char *text)
{
	XBuf x;
	Vec fields;
	char *s;

	xbuf_init(&x);
	expand_into(text ? text : "", &x, 0);
	vec_init(&fields);
	split_fields(&x, &fields, 0);
	xbuf_free(&x);
	s = fields.len ? xstrdup(fields.v[0]) : xstrdup("");
	vec_free(&fields);
	return s;
}
