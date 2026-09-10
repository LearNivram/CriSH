/* coreutils.c - the small GNU utilities that a pipeline leans on.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "gnu.h"

extern char **environ;

/* --------------------------------------------------------------------- cat */

int gnu_cat(int argc, char **argv)
{
	int number = 0, number_nonblank = 0, squeeze = 0;
	int show_ends = 0, show_tabs = 0, show_nonprint = 0;
	int i, rc = 0;
	long lineno = 0;
	int blank_run = 0;
	Vec files;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1] && strcmp(a, "--") != 0) {
			const char *p;

			if (gnu_long_opt(a, "number", NULL)) {
				number = 1;
				continue;
			}
			if (gnu_long_opt(a, "squeeze-blank", NULL)) {
				squeeze = 1;
				continue;
			}
			if (gnu_long_opt(a, "show-all", NULL)) {
				show_ends = show_tabs = show_nonprint = 1;
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: cat [-nbsAETuv] [file ...]\n");
				vec_free(&files);
				return 0;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'n': number = 1; break;
				case 'b': number_nonblank = 1; break;
				case 's': squeeze = 1; break;
				case 'E': show_ends = 1; break;
				case 'T': show_tabs = 1; break;
				case 'v': show_nonprint = 1; break;
				case 'A': show_ends = show_tabs = show_nonprint = 1; break;
				case 'e': show_ends = show_nonprint = 1; break;
				case 't': show_tabs = show_nonprint = 1; break;
				case 'u': break;
				default:
					gnu_error("cat", "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		if (strcmp(a, "--") == 0)
			continue;
		vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	{
		size_t k;
		int plain = !number && !number_nonblank && !squeeze && !show_ends &&
			    !show_tabs && !show_nonprint;

		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open("cat", files.v[k], &is_stdin);
			Buf line;

			if (!f) {
				rc = 1;
				continue;
			}
			if (plain) {
				char chunk[65536];
				size_t n;

				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					fwrite(chunk, 1, n, stdout);
				gnu_close(f, is_stdin);
				continue;
			}
			buf_init(&line);
			while (gnu_getdelim(&line, f, '\n') >= 0) {
				int is_blank = line.len == 0 ||
					       (line.len == 1 && line.b[0] == '\n');
				size_t j;

				if (squeeze && is_blank) {
					if (blank_run)
						continue;
					blank_run = 1;
				} else {
					blank_run = 0;
				}
				if (number_nonblank) {
					if (!is_blank)
						printf("%6ld\t", ++lineno);
				} else if (number) {
					printf("%6ld\t", ++lineno);
				}
				for (j = 0; j < line.len; j++) {
					unsigned char c = (unsigned char)line.b[j];

					if (c == '\n') {
						if (show_ends)
							putchar('$');
						putchar('\n');
						continue;
					}
					if (c == '\t' && show_tabs) {
						fputs("^I", stdout);
						continue;
					}
					if (show_nonprint && c != '\t') {
						if (c < 32) {
							putchar('^');
							putchar(c + 64);
							continue;
						}
						if (c == 127) {
							fputs("^?", stdout);
							continue;
						}
						if (c >= 128) {
							fputs("M-", stdout);
							if (c - 128 < 32) {
								putchar('^');
								putchar((int)c - 128 + 64);
							} else {
								putchar((int)c - 128);
							}
							continue;
						}
					}
					putchar(c);
				}
			}
			buf_free(&line);
			gnu_close(f, is_stdin);
		}
	}
	vec_free(&files);
	return rc;
}

/* -------------------------------------------------------------------- head */

static long parse_count(const char *s, int *from_start)
{
	char *end;
	long v;

	if (from_start)
		*from_start = 0;
	if (*s == '+') {
		if (from_start)
			*from_start = 1;
		s++;
	}
	v = strtol(s, &end, 10);
	switch (*end) {
	case 'b': v *= 512; break;
	case 'k':
	case 'K': v *= 1024; break;
	case 'm':
	case 'M': v *= 1024L * 1024; break;
	case 'g':
	case 'G': v *= 1024L * 1024 * 1024; break;
	default: break;
	}
	return v;
}

int gnu_head(int argc, char **argv)
{
	long count = 10;
	int bytes = 0, quiet = 0, verbose = 0;
	int i, rc = 0;
	Vec files;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			if (gnu_long_opt(a, "lines", &val)) {
				count = parse_count(val ? val : argv[++i], NULL);
				continue;
			}
			if (gnu_long_opt(a, "bytes", &val)) {
				bytes = 1;
				count = parse_count(val ? val : argv[++i], NULL);
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL) || gnu_long_opt(a, "silent", NULL)) {
				quiet = 1;
				continue;
			}
			if (gnu_long_opt(a, "verbose", NULL)) {
				verbose = 1;
				continue;
			}
			if (a[1] == 'n' || a[1] == 'c') {
				bytes = a[1] == 'c';
				if (a[2])
					count = parse_count(a + 2, NULL);
				else if (i + 1 < argc)
					count = parse_count(argv[++i], NULL);
				continue;
			}
			if (a[1] == 'q') {
				quiet = 1;
				continue;
			}
			if (a[1] == 'v') {
				verbose = 1;
				continue;
			}
			if (isdigit((unsigned char)a[1])) { /* the historical -5 form */
				count = parse_count(a + 1, NULL);
				continue;
			}
		}
		vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	{
		size_t k;

		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open("head", files.v[k], &is_stdin);

			if (!f) {
				rc = 1;
				continue;
			}
			if ((files.len > 1 && !quiet) || verbose)
				printf("%s==> %s <==\n", k ? "\n" : "", files.v[k]);

			if (bytes) {
				long n = count;
				int c;

				if (count >= 0) {
					while (n-- > 0 && (c = getc(f)) != EOF)
						putchar(c);
				} else {
					/* -c -N: all but the last N bytes */
					Buf all;
					char chunk[65536];
					size_t got;

					buf_init(&all);
					while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
						buf_put(&all, chunk, got);
					if (all.len > (size_t)(-count))
						fwrite(all.b, 1, all.len - (size_t)(-count),
						       stdout);
					buf_free(&all);
				}
			} else if (count >= 0) {
				Buf line;
				long n = 0;

				buf_init(&line);
				while (n < count && gnu_getdelim(&line, f, '\n') >= 0) {
					fwrite(line.b, 1, line.len, stdout);
					n++;
				}
				buf_free(&line);
			} else {
				/* GNU head -n -N: all but the last N lines */
				Vec lines;
				Buf line;
				size_t j, keep;

				vec_init(&lines);
				buf_init(&line);
				while (gnu_getdelim(&line, f, '\n') >= 0)
					vec_push(&lines, xstrndup(line.b, line.len));
				buf_free(&line);
				keep = lines.len > (size_t)(-count)
					       ? lines.len - (size_t)(-count)
					       : 0;
				for (j = 0; j < keep; j++)
					fputs(lines.v[j], stdout);
				vec_free(&lines);
			}
			gnu_close(f, is_stdin);
		}
	}
	vec_free(&files);
	return rc;
}

/* -------------------------------------------------------------------- tail */

int gnu_tail(int argc, char **argv)
{
	long count = 10;
	int from_start = 0, bytes = 0, follow = 0, quiet = 0, verbose = 0;
	int i, rc = 0;
	Vec files;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1]) {
			if (gnu_long_opt(a, "lines", &val)) {
				count = parse_count(val ? val : argv[++i], &from_start);
				continue;
			}
			if (gnu_long_opt(a, "bytes", &val)) {
				bytes = 1;
				count = parse_count(val ? val : argv[++i], &from_start);
				continue;
			}
			if (gnu_long_opt(a, "follow", NULL)) {
				follow = 1;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL) || gnu_long_opt(a, "silent", NULL)) {
				quiet = 1;
				continue;
			}
			if (a[1] == 'n' || a[1] == 'c') {
				bytes = a[1] == 'c';
				if (a[2])
					count = parse_count(a + 2, &from_start);
				else if (i + 1 < argc)
					count = parse_count(argv[++i], &from_start);
				continue;
			}
			if (a[1] == 'f' || a[1] == 'F') {
				follow = 1;
				continue;
			}
			if (a[1] == 'q') {
				quiet = 1;
				continue;
			}
			if (a[1] == 'v') {
				verbose = 1;
				continue;
			}
			if (isdigit((unsigned char)a[1])) {
				count = parse_count(a + 1, &from_start);
				continue;
			}
		}
		vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	{
		size_t k;

		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open("tail", files.v[k], &is_stdin);
			Vec lines;
			Buf line;

			if (!f) {
				rc = 1;
				continue;
			}
			if ((files.len > 1 && !quiet) || verbose)
				printf("%s==> %s <==\n", k ? "\n" : "", files.v[k]);

			if (bytes) {
				Buf all;
				char chunk[65536];
				size_t got;

				buf_init(&all);
				while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
					buf_put(&all, chunk, got);
				if (from_start) {
					size_t off = count > 0 ? (size_t)(count - 1) : 0;
					if (off < all.len)
						fwrite(all.b + off, 1, all.len - off, stdout);
				} else {
					size_t want = (size_t)(count < 0 ? -count : count);
					size_t off = all.len > want ? all.len - want : 0;
					fwrite(all.b + off, 1, all.len - off, stdout);
				}
				buf_free(&all);
				gnu_close(f, is_stdin);
				continue;
			}

			vec_init(&lines);
			buf_init(&line);
			while (gnu_getdelim(&line, f, '\n') >= 0)
				vec_push(&lines, xstrndup(line.b, line.len));
			buf_free(&line);

			if (from_start) {
				size_t start = count > 0 ? (size_t)(count - 1) : 0;
				size_t j;
				for (j = start; j < lines.len; j++)
					fputs(lines.v[j], stdout);
			} else {
				size_t want = (size_t)(count < 0 ? -count : count);
				size_t start = lines.len > want ? lines.len - want : 0;
				size_t j;
				for (j = start; j < lines.len; j++)
					fputs(lines.v[j], stdout);
			}
			vec_free(&lines);

			if (follow && !is_stdin) {
				fflush(stdout);
				for (;;) {
					int c;

					clearerr(f);
					while ((c = getc(f)) != EOF)
						putchar(c);
					fflush(stdout);
					{
						struct timespec ts = { 0, 200000000 };
						nanosleep(&ts, NULL);
					}
				}
			}
			gnu_close(f, is_stdin);
		}
	}
	vec_free(&files);
	return rc;
}

/* ---------------------------------------------------------------------- wc */

int gnu_wc(int argc, char **argv)
{
	int lines = 0, words = 0, bytes = 0, chars = 0, longest = 0;
	int any = 0;
	int i, rc = 0;
	Vec files;
	long tl = 0, tw = 0, tb = 0, tc = 0, tmax = 0;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1]) {
			const char *p;

			if (gnu_long_opt(a, "lines", NULL)) {
				lines = any = 1;
				continue;
			}
			if (gnu_long_opt(a, "words", NULL)) {
				words = any = 1;
				continue;
			}
			if (gnu_long_opt(a, "bytes", NULL)) {
				bytes = any = 1;
				continue;
			}
			if (gnu_long_opt(a, "chars", NULL)) {
				chars = any = 1;
				continue;
			}
			if (gnu_long_opt(a, "max-line-length", NULL)) {
				longest = any = 1;
				continue;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'l': lines = any = 1; break;
				case 'w': words = any = 1; break;
				case 'c': bytes = any = 1; break;
				case 'm': chars = any = 1; break;
				case 'L': longest = any = 1; break;
				default:
					gnu_error("wc", "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&files, a);
	}
	if (!any)
		lines = words = bytes = 1;
	if (!files.len)
		vec_pushs(&files, "-");

	{
		size_t k;

		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open("wc", files.v[k], &is_stdin);
			long nl = 0, nw = 0, nb = 0, nc = 0, maxlen = 0, cur = 0;
			int in_word = 0;
			int c;

			if (!f) {
				rc = 1;
				continue;
			}
			while ((c = getc(f)) != EOF) {
				nb++;
				if (((unsigned char)c & 0xc0) != 0x80)
					nc++;
				if (c == '\n') {
					nl++;
					if (cur > maxlen)
						maxlen = cur;
					cur = 0;
				} else {
					cur++;
				}
				if (isspace(c)) {
					in_word = 0;
				} else if (!in_word) {
					in_word = 1;
					nw++;
				}
			}
			if (cur > maxlen)
				maxlen = cur;
			gnu_close(f, is_stdin);

			if (lines)
				printf("%8ld", nl);
			if (words)
				printf("%8ld", nw);
			if (bytes)
				printf("%8ld", nb);
			if (chars)
				printf("%8ld", nc);
			if (longest)
				printf("%8ld", maxlen);
			if (!is_stdin)
				printf(" %s", files.v[k]);
			putchar('\n');

			tl += nl;
			tw += nw;
			tb += nb;
			tc += nc;
			if (maxlen > tmax)
				tmax = maxlen;
		}
		if (files.len > 1) {
			if (lines)
				printf("%8ld", tl);
			if (words)
				printf("%8ld", tw);
			if (bytes)
				printf("%8ld", tb);
			if (chars)
				printf("%8ld", tc);
			if (longest)
				printf("%8ld", tmax);
			printf(" total\n");
		}
	}
	vec_free(&files);
	return rc;
}

/* ------------------------------------------------------------- rev and tac */

int gnu_rev(int argc, char **argv)
{
	int i;
	Vec files;
	size_t k;
	int rc = 0;

	vec_init(&files);
	for (i = 1; i < argc; i++)
		if (argv[i][0] != '-' || !argv[i][1])
			vec_pushs(&files, argv[i]);
	if (!files.len)
		vec_pushs(&files, "-");

	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *f = gnu_open("rev", files.v[k], &is_stdin);
		Buf line;

		if (!f) {
			rc = 1;
			continue;
		}
		buf_init(&line);
		while (gnu_getdelim(&line, f, '\n') >= 0) {
			size_t n = line.len;
			int had_nl = n && line.b[n - 1] == '\n';

			if (had_nl)
				n--;
			while (n--)
				putchar(line.b[n]);
			putchar('\n');
		}
		buf_free(&line);
		gnu_close(f, is_stdin);
	}
	vec_free(&files);
	return rc;
}

int gnu_tac(int argc, char **argv)
{
	int i;
	Vec files, lines;
	size_t k;
	int rc = 0;
	const char *sep = "\n";

	vec_init(&files);
	vec_init(&lines);
	for (i = 1; i < argc; i++) {
		const char *val;

		if (gnu_long_opt(argv[i], "separator", &val)) {
			sep = val ? val : argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			sep = argv[++i];
			continue;
		}
		if (argv[i][0] != '-' || !argv[i][1])
			vec_pushs(&files, argv[i]);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *f = gnu_open("tac", files.v[k], &is_stdin);
		Buf line;

		if (!f) {
			rc = 1;
			continue;
		}
		buf_init(&line);
		while (gnu_getdelim(&line, f, sep[0]) >= 0)
			vec_push(&lines, xstrndup(line.b, line.len));
		buf_free(&line);
		gnu_close(f, is_stdin);
	}
	for (k = lines.len; k > 0; k--) {
		char *s = lines.v[k - 1];
		size_t len = strlen(s);

		if (len && s[len - 1] == sep[0]) {
			fwrite(s, 1, len - 1, stdout);
			fputs(sep, stdout);
		} else {
			fputs(s, stdout);
			fputs(sep, stdout);
		}
	}
	vec_free(&files);
	vec_free(&lines);
	return rc;
}

/* ---------------------------------------------------------------------- nl */

int gnu_nl(int argc, char **argv)
{
	int width = 6;
	const char *sep = "\t";
	long start = 1, incr = 1;
	char style = 't'; /* t: non-empty, a: all, n: none */
	int i, rc = 0;
	Vec files;
	size_t k;
	long n;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "-b") == 0 && i + 1 < argc) {
			style = argv[++i][0];
			continue;
		}
		if (strcmp(a, "-w") == 0 && i + 1 < argc) {
			width = atoi(argv[++i]);
			continue;
		}
		if (strcmp(a, "-s") == 0 && i + 1 < argc) {
			sep = argv[++i];
			continue;
		}
		if (strcmp(a, "-v") == 0 && i + 1 < argc) {
			start = strtol(argv[++i], NULL, 10);
			continue;
		}
		if (strcmp(a, "-i") == 0 && i + 1 < argc) {
			incr = strtol(argv[++i], NULL, 10);
			continue;
		}
		if (a[0] != '-' || !a[1])
			vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");
	n = start;

	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *f = gnu_open("nl", files.v[k], &is_stdin);
		Buf line;

		if (!f) {
			rc = 1;
			continue;
		}
		buf_init(&line);
		while (gnu_getdelim(&line, f, '\n') >= 0) {
			int empty = line.len == 0 || (line.len == 1 && line.b[0] == '\n');

			if (style == 'n' || (style == 't' && empty)) {
				printf("%*s%s", width, "", sep);
			} else {
				printf("%*ld%s", width, n, sep);
				n += incr;
			}
			fwrite(line.b, 1, line.len, stdout);
			if (!line.len || line.b[line.len - 1] != '\n')
				putchar('\n');
		}
		buf_free(&line);
		gnu_close(f, is_stdin);
	}
	vec_free(&files);
	return rc;
}

/* ------------------------------------------------------------------- paste */

int gnu_paste(int argc, char **argv)
{
	const char *delims = "\t";
	int serial = 0;
	int i, rc = 0;
	Vec files;
	size_t k;

	vec_init(&files);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (gnu_long_opt(a, "delimiters", &val)) {
			delims = val ? val : argv[++i];
			continue;
		}
		if (gnu_long_opt(a, "serial", NULL)) {
			serial = 1;
			continue;
		}
		if (a[0] == '-' && a[1] && a[1] != '-') {
			const char *q;
			int consumed = 1;

			for (q = a + 1; *q; q++) {
				if (*q == 's') {
					serial = 1;
					continue;
				}
				if (*q == 'd') {
					if (q[1]) {
						delims = q + 1;
					} else if (i + 1 < argc) {
						delims = argv[++i];
					}
					q = a + strlen(a) - 1;
					continue;
				}
				if (*q == 'z')
					continue;
				consumed = 0;
				break;
			}
			if (consumed)
				continue;
		}
		vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	{
		/* interpret escapes in the delimiter list, the way GNU does */
		Buf d;
		const char *p = delims;

		buf_init(&d);
		while (*p) {
			if (*p == '\\' && p[1]) {
				p++;
				switch (*p) {
				case 'n': buf_putc(&d, '\n'); break;
				case 't': buf_putc(&d, '\t'); break;
				case '0': buf_putc(&d, '\0'); break;
				case '\\': buf_putc(&d, '\\'); break;
				default: buf_putc(&d, *p); break;
				}
				p++;
				continue;
			}
			buf_putc(&d, *p++);
		}
		delims = buf_take(&d);
	}

	if (serial) {
		for (k = 0; k < files.len; k++) {
			int is_stdin;
			FILE *f = gnu_open("paste", files.v[k], &is_stdin);
			Buf line;
			size_t di = 0;
			int first = 1;

			if (!f) {
				rc = 1;
				continue;
			}
			buf_init(&line);
			while (gnu_getdelim(&line, f, '\n') >= 0) {
				size_t n = line.len;

				if (n && line.b[n - 1] == '\n')
					n--;
				if (!first) {
					putchar(delims[di]);
					di = delims[di + 1] ? di + 1 : 0;
				}
				fwrite(line.b, 1, n, stdout);
				first = 0;
			}
			putchar('\n');
			buf_free(&line);
			gnu_close(f, is_stdin);
		}
		free((char *)delims);
		vec_free(&files);
		return rc;
	}

	{
		FILE **fps = xcalloc(files.len, sizeof *fps);
		int *is_std = xcalloc(files.len, sizeof *is_std);
		int open_count = 0;

		for (k = 0; k < files.len; k++) {
			fps[k] = gnu_open("paste", files.v[k], &is_std[k]);
			if (fps[k])
				open_count++;
			else
				rc = 1;
		}
		while (open_count) {
			Buf line;
			int any = 0;
			size_t di = 0;

			buf_init(&line);
			for (k = 0; k < files.len; k++) {
				if (k) {
					putchar(delims[di]);
					di = delims[di + 1] ? di + 1 : 0;
				}
				if (!fps[k])
					continue;
				if (gnu_getdelim(&line, fps[k], '\n') >= 0) {
					size_t n = line.len;
					if (n && line.b[n - 1] == '\n')
						n--;
					fwrite(line.b, 1, n, stdout);
					any = 1;
				} else {
					gnu_close(fps[k], is_std[k]);
					fps[k] = NULL;
					open_count--;
				}
			}
			buf_free(&line);
			putchar('\n');
			if (!any)
				break;
		}
		free(fps);
		free(is_std);
	}
	free((char *)delims);
	vec_free(&files);
	return rc;
}

/* --------------------------------------------------------------------- tee */

int gnu_tee(int argc, char **argv)
{
	int append = 0;
	int i;
	Vec names;
	FILE **fps;
	size_t k;
	int c;
	int rc = 0;

	vec_init(&names);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (gnu_long_opt(a, "append", NULL) || strcmp(a, "-a") == 0) {
			append = 1;
			continue;
		}
		if (strcmp(a, "-i") == 0)
			continue;
		if (a[0] == '-' && a[1] && a[1] != '-')
			continue;
		vec_pushs(&names, a);
	}
	fps = xcalloc(names.len ? names.len : 1, sizeof *fps);
	for (k = 0; k < names.len; k++) {
		fps[k] = fopen(names.v[k], append ? "ab" : "wb");
		if (!fps[k]) {
			gnu_file_error("tee", names.v[k]);
			rc = 1;
		}
	}
	while ((c = getchar()) != EOF) {
		putchar(c);
		for (k = 0; k < names.len; k++)
			if (fps[k])
				putc(c, fps[k]);
	}
	for (k = 0; k < names.len; k++)
		if (fps[k])
			fclose(fps[k]);
	free(fps);
	vec_free(&names);
	fflush(stdout);
	return rc;
}

/* --------------------------------------------------------------------- seq */

int gnu_seq(int argc, char **argv)
{
	double first = 1, incr = 1, last;
	const char *sep = "\n";
	const char *format = NULL;
	int equal_width = 0;
	int i;
	Vec nums;

	vec_init(&nums);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (gnu_long_opt(a, "separator", &val)) {
			sep = val ? val : argv[++i];
			continue;
		}
		if (gnu_long_opt(a, "format", &val)) {
			format = val ? val : argv[++i];
			continue;
		}
		if (gnu_long_opt(a, "equal-width", NULL)) {
			equal_width = 1;
			continue;
		}
		if (a[0] == '-' && a[1] == 's' && (a[2] || i + 1 < argc)) {
			sep = a[2] ? a + 2 : argv[++i];
			continue;
		}
		if (a[0] == '-' && a[1] == 'f' && (a[2] || i + 1 < argc)) {
			format = a[2] ? a + 2 : argv[++i];
			continue;
		}
		if (strcmp(a, "-w") == 0) {
			equal_width = 1;
			continue;
		}
		vec_pushs(&nums, a);
	}
	if (!nums.len || nums.len > 3) {
		gnu_error("seq", "usage: seq [-w] [-s sep] [-f fmt] [first [incr]] last");
		vec_free(&nums);
		return 1;
	}
	if (nums.len == 1) {
		last = strtod(nums.v[0], NULL);
	} else if (nums.len == 2) {
		first = strtod(nums.v[0], NULL);
		last = strtod(nums.v[1], NULL);
	} else {
		first = strtod(nums.v[0], NULL);
		incr = strtod(nums.v[1], NULL);
		last = strtod(nums.v[2], NULL);
	}
	if (incr == 0) {
		gnu_error("seq", "increment must not be zero");
		vec_free(&nums);
		return 1;
	}

	{
		/* work out how many decimals and how wide the widest value is */
		int decimals = 0;
		size_t k;
		char buf[64];
		int width = 0;
		double v;
		int printed = 0;

		for (k = 0; k < nums.len; k++) {
			const char *dot = strchr(nums.v[k], '.');
			if (dot) {
				int d = (int)strlen(dot + 1);
				if (d > decimals)
					decimals = d;
			}
		}
		if (equal_width) {
			for (v = first; incr > 0 ? v <= last + 1e-9 : v >= last - 1e-9;
			     v += incr) {
				int n = snprintf(buf, sizeof buf, "%.*f", decimals, v);
				if (n > width)
					width = n;
			}
		}
		for (v = first; incr > 0 ? v <= last + 1e-9 : v >= last - 1e-9; v += incr) {
			if (printed)
				fputs(sep, stdout);
			if (format)
				printf(format, v);
			else if (equal_width)
				printf("%0*.*f", width, decimals, v);
			else
				printf("%.*f", decimals, v);
			printed = 1;
		}
		if (printed)
			putchar('\n');
	}
	vec_free(&nums);
	return 0;
}

/* --------------------------------------------------------------------- yes */

int gnu_yes(int argc, char **argv)
{
	Buf line;
	int i;

	buf_init(&line);
	if (argc < 2) {
		buf_puts(&line, "y");
	} else {
		for (i = 1; i < argc; i++) {
			if (i > 1)
				buf_putc(&line, ' ');
			buf_puts(&line, argv[i]);
		}
	}
	buf_putc(&line, '\n');
	for (;;) {
		if (fwrite(line.b, 1, line.len, stdout) != line.len)
			break;
	}
	buf_free(&line);
	return 0;
}

/* ------------------------------------------------------- basename, dirname */

int gnu_basename(int argc, char **argv)
{
	const char *suffix = NULL;
	char *suffix_owned = NULL;
	int multiple = 0, zero = 0;
	int i;
	Vec names;
	size_t k;

	vec_init(&names);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (gnu_long_opt(a, "suffix", &val)) {
			suffix = val ? val : argv[++i];
			multiple = 1;
			continue;
		}
		if (gnu_long_opt(a, "multiple", NULL)) {
			multiple = 1;
			continue;
		}
		if (gnu_long_opt(a, "zero", NULL)) {
			zero = 1;
			continue;
		}
		if (strcmp(a, "-s") == 0 && i + 1 < argc) {
			suffix = argv[++i];
			multiple = 1;
			continue;
		}
		if (strcmp(a, "-a") == 0) {
			multiple = 1;
			continue;
		}
		if (strcmp(a, "-z") == 0) {
			zero = 1;
			continue;
		}
		vec_pushs(&names, a);
	}
	if (!names.len) {
		gnu_error("basename", "missing operand");
		vec_free(&names);
		return 1;
	}
	if (!multiple && names.len == 2) {
		suffix_owned = vec_remove(&names, 1);
		suffix = suffix_owned;
	}
	for (k = 0; k < names.len; k++) {
		char *copy = xstrdup(names.v[k]);
		size_t len = strlen(copy);
		char *base;

		while (len > 1 && copy[len - 1] == '/')
			copy[--len] = '\0';
		base = strrchr(copy, '/');
		base = base && base[1] ? base + 1 : (base && len == 1 ? copy : base ? base : copy);
		if (base != copy && *base == '/')
			base++;
		if (suffix && *suffix) {
			size_t bl = strlen(base), sl = strlen(suffix);
			if (bl > sl && strcmp(base + bl - sl, suffix) == 0)
				base[bl - sl] = '\0';
		}
		fputs(base, stdout);
		putchar(zero ? '\0' : '\n');
		free(copy);
	}
	free(suffix_owned);
	vec_free(&names);
	return 0;
}

int gnu_dirname(int argc, char **argv)
{
	int i, zero = 0;
	int any = 0;

	for (i = 1; i < argc; i++) {
		char *copy;
		char *slash;
		size_t len;

		if (gnu_long_opt(argv[i], "zero", NULL) || strcmp(argv[i], "-z") == 0) {
			zero = 1;
			continue;
		}
		if (argv[i][0] == '-' && argv[i][1])
			continue;
		copy = xstrdup(argv[i]);
		len = strlen(copy);
		while (len > 1 && copy[len - 1] == '/')
			copy[--len] = '\0';
		slash = strrchr(copy, '/');
		if (!slash)
			fputs(".", stdout);
		else if (slash == copy)
			fputs("/", stdout);
		else {
			*slash = '\0';
			fputs(copy, stdout);
		}
		putchar(zero ? '\0' : '\n');
		free(copy);
		any = 1;
	}
	if (!any) {
		gnu_error("dirname", "missing operand");
		return 1;
	}
	return 0;
}

/* --------------------------------------------------------------------- env */

int gnu_env(int argc, char **argv)
{
	int i = 1;
	int ignore_env = 0;
	Vec unset_names;
	Vec assigns;

	vec_init(&unset_names);
	vec_init(&assigns);
	for (; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (strcmp(a, "-i") == 0 || gnu_long_opt(a, "ignore-environment", NULL)) {
			ignore_env = 1;
			continue;
		}
		if (strcmp(a, "-") == 0) {
			ignore_env = 1;
			continue;
		}
		if (gnu_long_opt(a, "unset", &val)) {
			vec_pushs(&unset_names, val ? val : argv[++i]);
			continue;
		}
		if (strcmp(a, "-u") == 0 && i + 1 < argc) {
			vec_pushs(&unset_names, argv[++i]);
			continue;
		}
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		break;
	}
	for (; i < argc && strchr(argv[i], '=') &&
	       argv[i][0] != '=' ; i++)
		vec_pushs(&assigns, argv[i]);

	if (i >= argc) {
		/* just print the environment */
		char **e = vars_environ();
		size_t k;

		if (!ignore_env)
			for (; *e; e++)
				puts(*e);
		for (k = 0; k < assigns.len; k++)
			puts(assigns.v[k]);
		vec_free(&unset_names);
		vec_free(&assigns);
		return 0;
	}

	{
		pid_t pid = fork();

		if (pid < 0) {
			gnu_error("env", "%s", strerror(errno));
			vec_free(&unset_names);
			vec_free(&assigns);
			return 1;
		}
		if (pid == 0) {
			Vec cmd;
			char **envp;
			size_t k;
			char *path;

			if (ignore_env)
				environ = NULL;
			for (k = 0; k < unset_names.len; k++)
				unsetenv(unset_names.v[k]);
			for (k = 0; k < assigns.len; k++) {
				char *eq = strchr(assigns.v[k], '=');
				*eq = '\0';
				setenv(assigns.v[k], eq + 1, 1);
				*eq = '=';
			}
			vec_init(&cmd);
			for (; i < argc; i++)
				vec_pushs(&cmd, argv[i]);
			envp = environ;
			path = path_lookup(cmd.v[0]);
			if (!path) {
				gnu_error("env", "%s: No such file or directory", cmd.v[0]);
				_exit(127);
			}
			execve(path, vec_argv(&cmd), envp);
			gnu_error("env", "%s: %s", cmd.v[0], strerror(errno));
			_exit(126);
		}
		{
			int status = 0;
			waitpid(pid, &status, 0);
			vec_free(&unset_names);
			vec_free(&assigns);
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		}
	}
}

/* ------------------------------------------------------------------- sleep */

int gnu_sleep(int argc, char **argv)
{
	double total = 0;
	int i;
	int any = 0;

	for (i = 1; i < argc; i++) {
		char *end;
		double v;

		if (argv[i][0] == '-' && argv[i][1])
			continue;
		v = strtod(argv[i], &end);
		switch (*end) {
		case 's':
		case '\0': break;
		case 'm': v *= 60; break;
		case 'h': v *= 3600; break;
		case 'd': v *= 86400; break;
		default:
			gnu_error("sleep", "invalid time interval '%s'", argv[i]);
			return 1;
		}
		total += v;
		any = 1;
	}
	if (!any) {
		gnu_error("sleep", "missing operand");
		return 1;
	}
	{
		struct timespec ts;

		ts.tv_sec = (time_t)total;
		ts.tv_nsec = (long)((total - (double)ts.tv_sec) * 1e9);
		while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
			;
	}
	return 0;
}

/* ---------------------------------------------------------------- truncate */

int gnu_truncate(int argc, char **argv)
{
	const char *size = NULL;
	int i;
	int rc = 0;
	int any = 0;

	for (i = 1; i < argc; i++) {
		const char *val;

		if (gnu_long_opt(argv[i], "size", &val)) {
			size = val ? val : argv[++i];
			continue;
		}
		if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
			size = argv[++i];
			continue;
		}
		if (argv[i][0] == '-' && argv[i][1] == 's' && argv[i][2]) {
			size = argv[i] + 2;
			continue;
		}
		if (argv[i][0] == '-' && argv[i][1])
			continue;
		if (!size) {
			gnu_error("truncate", "you must specify a --size option");
			return 1;
		}
		{
			long want = parse_count(size[0] == '+' || size[0] == '-' ? size + 1 : size,
						NULL);
			struct stat st;
			long target = want;

			if (size[0] == '+' || size[0] == '-') {
				if (stat(argv[i], &st) == 0)
					target = size[0] == '+' ? st.st_size + want
								: st.st_size - want;
				if (target < 0)
					target = 0;
			}
			if (truncate(argv[i], target) != 0) {
				int fd = open(argv[i], O_WRONLY | O_CREAT, 0666);
				if (fd < 0 || ftruncate(fd, target) != 0) {
					gnu_file_error("truncate", argv[i]);
					rc = 1;
				}
				if (fd >= 0)
					close(fd);
			}
		}
		any = 1;
	}
	if (!any) {
		gnu_error("truncate", "missing file operand");
		return 1;
	}
	return rc;
}
