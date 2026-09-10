/* cond.c - the test builtin, [ ... ] and [[ ... ]].
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "regex.h"
#include "shell.h"

/* ---------------------------------------------------------- file predicates */

static int stat_of(const char *path, struct stat *st, int follow)
{
	return follow ? stat(path, st) == 0 : lstat(path, st) == 0;
}

static int file_test(int op, const char *path)
{
	struct stat st;

	switch (op) {
	case 'e':
		return stat_of(path, &st, 1);
	case 'f':
		return stat_of(path, &st, 1) && S_ISREG(st.st_mode);
	case 'd':
		return stat_of(path, &st, 1) && S_ISDIR(st.st_mode);
	case 'b':
		return stat_of(path, &st, 1) && S_ISBLK(st.st_mode);
	case 'c':
		return stat_of(path, &st, 1) && S_ISCHR(st.st_mode);
	case 'p':
		return stat_of(path, &st, 1) && S_ISFIFO(st.st_mode);
	case 'S':
		return stat_of(path, &st, 1) && S_ISSOCK(st.st_mode);
	case 'L':
	case 'h':
		return stat_of(path, &st, 0) && S_ISLNK(st.st_mode);
	case 's':
		return stat_of(path, &st, 1) && st.st_size > 0;
	case 'r':
		return access(path, R_OK) == 0;
	case 'w':
		return access(path, W_OK) == 0;
	case 'x':
		return access(path, X_OK) == 0;
	case 'u':
		return stat_of(path, &st, 1) && (st.st_mode & S_ISUID);
	case 'g':
		return stat_of(path, &st, 1) && (st.st_mode & S_ISGID);
	case 'k':
		return stat_of(path, &st, 1) && (st.st_mode & S_ISVTX);
	case 'O':
		return stat_of(path, &st, 1) && st.st_uid == geteuid();
	case 'G':
		return stat_of(path, &st, 1) && st.st_gid == getegid();
	case 'N':
		return stat_of(path, &st, 1) && st.st_mtime > st.st_atime;
	case 't': {
		char *end;
		long fd = strtol(path, &end, 10);
		return *end == '\0' && isatty((int)fd);
	}
	default:
		return 0;
	}
}

static int is_unary_op(const char *s)
{
	if (s[0] != '-' || !s[1] || s[2])
		return 0;
	return strchr("efdbcpSLhsrwxugkOGNtnzov", s[1]) != NULL;
}

static long to_long(const char *s, int *ok)
{
	char *end;
	long v;

	while (*s == ' ' || *s == '\t')
		s++;
	v = strtol(s, &end, 10);
	while (*end == ' ' || *end == '\t')
		end++;
	*ok = end != s && *end == '\0';
	return v;
}

static int mtime_cmp(const char *a, const char *b, int newer)
{
	struct stat sa, sb;
	int oa = stat(a, &sa) == 0, ob = stat(b, &sb) == 0;

	if (!oa && !ob)
		return 0;
	if (!ob)
		return newer;
	if (!oa)
		return !newer;
	return newer ? sa.st_mtime > sb.st_mtime : sa.st_mtime < sb.st_mtime;
}

static int same_file(const char *a, const char *b)
{
	struct stat sa, sb;

	return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev &&
	       sa.st_ino == sb.st_ino;
}

/* Binary operators shared by test and [[ ]].  Returns -1 when op is not one. */
static int binary_op(const char *a, const char *op, const char *b, int extended)
{
	int ok1, ok2;

	if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
		if (extended)
			return glob_match(b, a, sh.shopt.nocasematch);
		return strcmp(a, b) == 0;
	}
	if (strcmp(op, "!=") == 0) {
		if (extended)
			return !glob_match(b, a, sh.shopt.nocasematch);
		return strcmp(a, b) != 0;
	}
	if (strcmp(op, "<") == 0)
		return strcmp(a, b) < 0;
	if (strcmp(op, ">") == 0)
		return strcmp(a, b) > 0;
	if (strcmp(op, "-eq") == 0)
		return to_long(a, &ok1) == to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-ne") == 0)
		return to_long(a, &ok1) != to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-lt") == 0)
		return to_long(a, &ok1) < to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-le") == 0)
		return to_long(a, &ok1) <= to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-gt") == 0)
		return to_long(a, &ok1) > to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-ge") == 0)
		return to_long(a, &ok1) >= to_long(b, &ok2) && ok1 && ok2;
	if (strcmp(op, "-nt") == 0)
		return mtime_cmp(a, b, 1);
	if (strcmp(op, "-ot") == 0)
		return mtime_cmp(a, b, 0);
	if (strcmp(op, "-ef") == 0)
		return same_file(a, b);
	return -1;
}

static int unary_eval(const char *op, const char *arg)
{
	switch (op[1]) {
	case 'n':
		return *arg != '\0';
	case 'z':
		return *arg == '\0';
	case 'o': {
		/* -o option: shell option is on */
		if (strcmp(arg, "errexit") == 0)
			return sh.opt.errexit;
		if (strcmp(arg, "nounset") == 0)
			return sh.opt.nounset;
		if (strcmp(arg, "xtrace") == 0)
			return sh.opt.xtrace;
		if (strcmp(arg, "pipefail") == 0)
			return sh.opt.pipefail;
		if (strcmp(arg, "noglob") == 0)
			return sh.opt.noglob;
		return 0;
	}
	case 'v': {
		/* -v NAME: the variable is set */
		char *name = xstrdup(arg);
		char *bracket = strchr(name, '[');
		int r;

		if (bracket) {
			char *close = strrchr(bracket, ']');
			*bracket = '\0';
			if (close)
				*close = '\0';
			r = var_array_get(name, bracket + 1) != NULL;
		} else {
			r = var_get(name) != NULL;
		}
		free(name);
		return r;
	}
	default:
		return file_test(op[1], arg);
	}
}

/* --------------------------------------------------------- test and [ ... ] */

typedef struct {
	char **argv;
	int argc;
	int pos;
	int err;
} Tx;

static int t_or(Tx *t);

static const char *t_peek(Tx *t)
{
	return t->pos < t->argc ? t->argv[t->pos] : NULL;
}

static int t_primary(Tx *t)
{
	const char *w = t_peek(t);

	if (!w) {
		t->err = 1;
		return 0;
	}
	if (strcmp(w, "!") == 0) {
		t->pos++;
		return !t_primary(t);
	}
	if (strcmp(w, "(") == 0) {
		int v;
		t->pos++;
		v = t_or(t);
		if (!t_peek(t) || strcmp(t_peek(t), ")") != 0) {
			t->err = 1;
			return 0;
		}
		t->pos++;
		return v;
	}
	/* binary form: word OP word */
	if (t->pos + 2 < t->argc) {
		int r = binary_op(t->argv[t->pos], t->argv[t->pos + 1], t->argv[t->pos + 2], 0);
		if (r >= 0) {
			t->pos += 3;
			return r;
		}
	}
	if (is_unary_op(w) && t->pos + 1 < t->argc) {
		int r = unary_eval(w, t->argv[t->pos + 1]);
		t->pos += 2;
		return r;
	}
	t->pos++;
	return *w != '\0';
}

static int t_and(Tx *t)
{
	int v = t_primary(t);

	while (t_peek(t) && strcmp(t_peek(t), "-a") == 0) {
		t->pos++;
		v = t_primary(t) && v;
	}
	return v;
}

static int t_or(Tx *t)
{
	int v = t_and(t);

	while (t_peek(t) && strcmp(t_peek(t), "-o") == 0) {
		t->pos++;
		v = t_and(t) || v;
	}
	return v;
}

int builtin_test(int argc, char **argv);
int builtin_test(int argc, char **argv)
{
	Tx t;
	int v;

	/* `[ ... ]` must end in ] */
	if (strcmp(argv[0], "[") == 0) {
		if (argc < 2 || strcmp(argv[argc - 1], "]") != 0) {
			shell_error("[: missing `]'");
			return 2;
		}
		argc--;
	}
	t.argv = argv + 1;
	t.argc = argc - 1;
	t.pos = 0;
	t.err = 0;

	if (t.argc == 0)
		return 1;
	v = t_or(&t);
	if (t.err || t.pos != t.argc) {
		shell_error("%s: too many arguments", argv[0]);
		return 2;
	}
	return v ? 0 : 1;
}

/* ------------------------------------------------------------- [[ ... ]] */

typedef struct {
	Vec *toks;
	size_t pos;
	int err;
} Cd;

static int c_or(Cd *c);

static const char *c_peek(Cd *c)
{
	return c->pos < c->toks->len ? c->toks->v[c->pos] : NULL;
}

static void set_rematch(const char *subject, RxMatch *m)
{
	char idx[16];
	int g;

	var_array_clear("BASH_REMATCH");
	var_declare("BASH_REMATCH", V_ARRAY);
	for (g = 0; g <= m->ngroups && g < RX_MAX_GROUPS; g++) {
		char *piece;

		if (m->group[g].start < 0 || m->group[g].end < m->group[g].start)
			piece = xstrdup("");
		else
			piece = xstrndup(subject + m->group[g].start,
					 (size_t)(m->group[g].end - m->group[g].start));
		snprintf(idx, sizeof idx, "%d", g);
		var_array_set("BASH_REMATCH", idx, piece, 0);
		free(piece);
	}
}

static int c_primary(Cd *c)
{
	const char *w = c_peek(c);

	if (!w) {
		c->err = 1;
		return 0;
	}
	if (strcmp(w, "!") == 0) {
		c->pos++;
		return !c_primary(c);
	}
	if (strcmp(w, "(") == 0) {
		int v;
		c->pos++;
		v = c_or(c);
		if (!c_peek(c) || strcmp(c_peek(c), ")") != 0) {
			c->err = 1;
			return 0;
		}
		c->pos++;
		return v;
	}
	if (is_unary_op(w) && c->pos + 1 < c->toks->len) {
		char *arg = expand_unsplit(c->toks->v[c->pos + 1]);
		int r = unary_eval(w, arg);
		free(arg);
		c->pos += 2;
		return r;
	}
	if (c->pos + 2 < c->toks->len) {
		const char *op = c->toks->v[c->pos + 1];

		if (strcmp(op, "=~") == 0) {
			char *lhs = expand_unsplit(c->toks->v[c->pos]);
			char *pat = expand_regex(c->toks->v[c->pos + 2]);
			const char *err = NULL;
			Rx *rx = rx_compile(pat, RX_ERE,
					    sh.shopt.nocasematch ? RX_ICASE : 0, &err);
			int r = 0;

			if (!rx) {
				shell_error("[[: %s: %s", pat, err ? err : "bad pattern");
				c->err = 2;
			} else {
				RxMatch m;
				r = rx_search(rx, lhs, strlen(lhs), 0, &m);
				if (r)
					set_rematch(lhs, &m);
				rx_free(rx);
			}
			free(lhs);
			free(pat);
			c->pos += 3;
			return r;
		}
		if (strcmp(op, "==") == 0 || strcmp(op, "=") == 0 || strcmp(op, "!=") == 0) {
			char *lhs = expand_unsplit(c->toks->v[c->pos]);
			char *pat = expand_pattern(c->toks->v[c->pos + 2]);
			int r = glob_match(pat, lhs, sh.shopt.nocasematch);

			if (strcmp(op, "!=") == 0)
				r = !r;
			free(lhs);
			free(pat);
			c->pos += 3;
			return r;
		}
		{
			char *lhs = expand_unsplit(c->toks->v[c->pos]);
			char *rhs = expand_unsplit(c->toks->v[c->pos + 2]);
			int r = binary_op(lhs, op, rhs, 1);

			free(lhs);
			free(rhs);
			if (r >= 0) {
				c->pos += 3;
				return r;
			}
		}
	}
	{
		char *v = expand_unsplit(w);
		int r = *v != '\0';

		free(v);
		c->pos++;
		return r;
	}
}

static int c_and(Cd *c)
{
	int v = c_primary(c);

	while (c_peek(c) && strcmp(c_peek(c), "&&") == 0) {
		c->pos++;
		if (!v) {
			/* still consume the right hand side */
			c_primary(c);
			continue;
		}
		v = c_primary(c);
	}
	return v;
}

static int c_or(Cd *c)
{
	int v = c_and(c);

	while (c_peek(c) && strcmp(c_peek(c), "||") == 0) {
		c->pos++;
		if (v) {
			c_and(c);
			continue;
		}
		v = c_and(c);
	}
	return v;
}

int cond_eval(Vec *tokens);
int cond_eval(Vec *tokens)
{
	Cd c;
	int v;

	c.toks = tokens;
	c.pos = 0;
	c.err = 0;
	if (!tokens->len) {
		shell_error("[[: empty conditional expression");
		return 2;
	}
	v = c_or(&c);
	if (c.err)
		return 2;
	if (c.pos != tokens->len) {
		shell_error("[[: syntax error near `%s'",
			    c.pos < tokens->len ? tokens->v[c.pos] : "]]");
		return 2;
	}
	return v ? 0 : 1;
}
