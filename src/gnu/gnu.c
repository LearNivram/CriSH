/* gnu.c - the table of built-in GNU-compatible utilities and their plumbing.
 *
 * These run inside the shell process.  A four stage pipeline of them costs no
 * execve at all, and more to the point they behave the way the GNU versions
 * do rather than the way the BSD ones on a Mac do.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gnu.h"

void gnu_error(const char *prog, const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fprintf(stderr, "%s: ", prog);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

int gnu_file_error(const char *prog, const char *file)
{
	gnu_error(prog, "%s: %s", file, strerror(errno));
	return 1;
}

FILE *gnu_open(const char *prog, const char *path, int *is_stdin)
{
	FILE *f;

	if (!path || strcmp(path, "-") == 0) {
		*is_stdin = 1;
		return stdin;
	}
	*is_stdin = 0;
	f = fopen(path, "rb");
	if (!f)
		gnu_file_error(prog, path);
	return f;
}

void gnu_close(FILE *f, int is_stdin)
{
	if (f && !is_stdin)
		fclose(f);
}

long gnu_getdelim(Buf *buf, FILE *f, int delim)
{
	int c;

	buf_reset(buf);
	while ((c = getc(f)) != EOF) {
		buf_putc(buf, c);
		if (c == delim)
			return (long)buf->len;
	}
	return buf->len ? (long)buf->len : -1;
}

int gnu_long_opt(const char *arg, const char *name, const char **value)
{
	size_t nlen;
	const char *eq;

	if (arg[0] != '-' || arg[1] != '-')
		return 0;
	arg += 2;
	eq = strchr(arg, '=');
	nlen = eq ? (size_t)(eq - arg) : strlen(arg);
	if (nlen == 0)
		return 0;
	if (strncmp(arg, name, nlen) != 0)
		return 0;
	if (value)
		*value = eq ? eq + 1 : NULL;
	return 1;
}

/* -------------------------------------------------------------- the table */

static const GnuTool tools[] = {
	{ "awk", gnu_awk, "pattern scanning and processing" },
	{ "base64", gnu_base64, "base64 encode or decode (-d, -w)" },
	{ "basename", gnu_basename, "strip directory and suffix (-a, -s, -z)" },
	{ "cat", gnu_cat, "concatenate files (-n, -b, -s, -A, -E, -T, -v)" },
	{ "cut", gnu_cut, "select fields or characters (-d, -f, -c, -b, --complement)" },
	{ "date", gnu_date, "print or convert dates (-d, -u, -r, --iso-8601, %N, %s)" },
	{ "dirname", gnu_dirname, "strip the last path component" },
	{ "env", gnu_env, "run a command in a modified environment (-i, -u)" },
	{ "expr", gnu_expr, "evaluate an expression" },
	{ "grep", gnu_grep, "search for a pattern (-E, -F, -P, -r, -o, -A/-B/-C, -w)" },
	{ "head", gnu_head, "first lines or bytes (-n, -c, negative counts)" },
	{ "md5sum", gnu_sha, "MD5 checksums (-c, --tag)" },
	{ "mktemp", gnu_mktemp, "make a temporary file or directory (-d, -t, --suffix)" },
	{ "nl", gnu_nl, "number lines (-b, -w, -s, -v)" },
	{ "paste", gnu_paste, "merge lines of files (-d, -s)" },
	{ "readlink", gnu_readlink, "resolve a symlink (-f, -e, -m)" },
	{ "realpath", gnu_realpath, "canonicalise a path (--relative-to, -m, -s)" },
	{ "rev", gnu_rev, "reverse each line" },
	{ "sed", gnu_sed, "stream editor (-i with GNU semantics, -n, -E, -e, -f)" },
	{ "seq", gnu_seq, "print a sequence of numbers (-w, -s, -f)" },
	{ "sha1sum", gnu_sha, "SHA-1 checksums (-c, --tag)" },
	{ "sha256sum", gnu_sha, "SHA-256 checksums (-c, --tag)" },
	{ "sha512sum", gnu_sha, "SHA-512 checksums (-c, --tag)" },
	{ "shuf", gnu_shuf, "shuffle lines (-n, -e, -i, -r)" },
	{ "sleep", gnu_sleep, "sleep, with s/m/h/d suffixes" },
	{ "sort", gnu_sort, "sort lines (-n, -k, -t, -u, -r, -V, -h, -s, -c)" },
	{ "stat", gnu_stat, "file status with a GNU -c format" },
	{ "tac", gnu_tac, "print lines in reverse" },
	{ "tail", gnu_tail, "last lines or bytes (-n +N, -f, -c)" },
	{ "tee", gnu_tee, "copy standard input to files (-a)" },
	{ "timeout", gnu_timeout, "run a command with a time limit" },
	{ "tr", gnu_tr, "translate or delete characters (-d, -s, -c, classes)" },
	{ "truncate", gnu_truncate, "shrink or extend a file (-s)" },
	{ "uniq", gnu_uniq, "report or omit repeated lines (-c, -d, -u, -i, -f, -s)" },
	{ "wc", gnu_wc, "count lines, words and bytes (-l, -w, -c, -m, -L)" },
	{ "xargs", gnu_xargs, "build command lines (-0, -n, -I, -P, -r)" },
	{ "yes", gnu_yes, "repeat a string forever" },
};

const GnuTool *gnu_find(const char *name)
{
	size_t lo = 0, hi = sizeof tools / sizeof tools[0];

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;
		int c = strcmp(tools[mid].name, name);

		if (c == 0)
			return &tools[mid];
		if (c < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

const GnuTool *gnu_table(size_t *count)
{
	*count = sizeof tools / sizeof tools[0];
	return tools;
}
