/* grep.c - a GNU-compatible grep on the shell's own regex engine.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../color.h"
#include "../regex.h"
#include "gnu.h"

typedef struct {
	Rx **rx;
	size_t nrx;
	Vec fixed;      /* -F patterns */
	int fixed_mode;
	int invert;
	int ignore_case;
	int word_match;
	int line_match;
	int count_only;
	int files_with;
	int files_without;
	int quiet;
	int line_numbers;
	int with_filename;
	int no_filename;
	int only_matching;
	int recursive;
	int no_messages;
	int byte_offset;
	int null_out;
	long max_count;
	long after;
	long before;
	Vec include;
	Vec exclude;
	Vec exclude_dir;
	const char *label;
	int colour;      /* COLOR_AUTO / ALWAYS / NEVER */
	int colour_on;   /* the decision, once made */
	/* GREP_COLORS, in the same names GNU uses */
	char *c_ms, *c_mc, *c_sl, *c_cx, *c_fn, *c_ln, *c_se;
} Grep;

/* GNU writes these as bare SGR bodies, so they are stored that way. */
static const char *grep_colour(const char *body)
{
	static char buf[64];

	if (!body || !*body)
		return "";
	snprintf(buf, sizeof buf, "\033[%sm", body);
	return buf;
}

static void grep_paint(Grep *g, const char *body, FILE *out)
{
	if (g->colour_on && body && *body)
		fputs(grep_colour(body), out);
}

static void grep_reset(Grep *g, FILE *out)
{
	if (g->colour_on)
		fputs("\033[m", out);
}

/* GREP_COLORS="ms=01;31:fn=35:ln=32:se=36" */
static void grep_read_colors(Grep *g)
{
	/* The built-in tools run inside the shell, so the shell's own variables
	 * are the environment as far as they are concerned. */
	const char *spec = var_get("GREP_COLORS");
	char *copy, *save = NULL, *item;

	g->c_ms = xstrdup("01;31");
	g->c_mc = xstrdup("01;31");
	g->c_sl = xstrdup("");
	g->c_cx = xstrdup("");
	g->c_fn = xstrdup("35");
	g->c_ln = xstrdup("32");
	g->c_se = xstrdup("36");
	if (!spec || !*spec)
		return;
	copy = xstrdup(spec);
	for (item = strtok_r(copy, ":", &save); item; item = strtok_r(NULL, ":", &save)) {
		char *eq = strchr(item, '=');
		char **slot = NULL;

		if (!eq)
			continue;
		*eq = '\0';
		if (strcmp(item, "ms") == 0)
			slot = &g->c_ms;
		else if (strcmp(item, "mc") == 0)
			slot = &g->c_mc;
		else if (strcmp(item, "sl") == 0)
			slot = &g->c_sl;
		else if (strcmp(item, "cx") == 0)
			slot = &g->c_cx;
		else if (strcmp(item, "fn") == 0)
			slot = &g->c_fn;
		else if (strcmp(item, "ln") == 0)
			slot = &g->c_ln;
		else if (strcmp(item, "se") == 0)
			slot = &g->c_se;
		if (slot) {
			free(*slot);
			*slot = xstrdup(eq + 1);
		}
	}
	free(copy);
}

static int line_matches(Grep *g, const char *line, size_t len, RxMatch *m, size_t from)
{
	size_t i;

	if (g->fixed_mode) {
		for (i = 0; i < g->fixed.len; i++) {
			const char *pat = g->fixed.v[i];
			size_t plen = strlen(pat);
			size_t j;

			if (g->line_match) {
				int hit = g->ignore_case ? (len == plen &&
							    strncasecmp(line, pat, len) == 0)
							 : (len == plen &&
							    memcmp(line, pat, len) == 0);
				if (hit) {
					if (m) {
						m->start = 0;
						m->end = (long)len;
						m->ngroups = 0;
					}
					return 1;
				}
				continue;
			}
			if (plen == 0) {
				if (m) {
					m->start = (long)from;
					m->end = (long)from;
					m->ngroups = 0;
				}
				return 1;
			}
			for (j = from; j + plen <= len; j++) {
				int hit = g->ignore_case
						  ? strncasecmp(line + j, pat, plen) == 0
						  : memcmp(line + j, pat, plen) == 0;
				if (!hit)
					continue;
				if (g->word_match) {
					int before = j > 0 && (isalnum((unsigned char)line[j - 1]) ||
							       line[j - 1] == '_');
					int after = j + plen < len &&
						    (isalnum((unsigned char)line[j + plen]) ||
						     line[j + plen] == '_');
					if (before || after)
						continue;
				}
				if (m) {
					m->start = (long)j;
					m->end = (long)(j + plen);
					m->ngroups = 0;
				}
				return 1;
			}
		}
		return 0;
	}

	for (i = 0; i < g->nrx; i++) {
		RxMatch local;
		size_t start = from;

		while (rx_search(g->rx[i], line, len, start, &local)) {
			if (g->line_match &&
			    !(local.start == 0 && local.end == (long)len)) {
				if ((size_t)local.start + 1 > len)
					break;
				start = (size_t)local.start + 1;
				continue;
			}
			if (g->word_match) {
				int before = local.start > 0 &&
					     (isalnum((unsigned char)line[local.start - 1]) ||
					      line[local.start - 1] == '_');
				int after = (size_t)local.end < len &&
					    (isalnum((unsigned char)line[local.end]) ||
					     line[local.end] == '_');
				if (before || after) {
					if ((size_t)local.start + 1 > len)
						break;
					start = (size_t)local.start + 1;
					continue;
				}
			}
			if (m)
				*m = local;
			return 1;
		}
	}
	return 0;
}

/* Paint every match on the line; used for selected lines only, because GNU
 * does not highlight the lines that -v let through. */
static void print_body(Grep *g, const char *line, size_t len, int is_context)
{
	size_t from = 0;
	const char *body = is_context ? g->c_cx : g->c_sl;

	if (!g->colour_on || g->invert || is_context) {
		grep_paint(g, body, stdout);
		fwrite(line, 1, len, stdout);
		if (g->colour_on && body && *body)
			grep_reset(g, stdout);
		return;
	}
	while (from < len) {
		RxMatch m;

		if (!line_matches(g, line, len, &m, from))
			break;
		if (m.end <= m.start) {
			from = (size_t)m.start + 1;
			continue;
		}
		fwrite(line + from, 1, (size_t)m.start - from, stdout);
		grep_paint(g, g->c_ms, stdout);
		fwrite(line + m.start, 1, (size_t)(m.end - m.start), stdout);
		grep_reset(g, stdout);
		from = (size_t)m.end;
	}
	if (from < len)
		fwrite(line + from, 1, len - from, stdout);
}

static void print_line(Grep *g, const char *name, long lineno, long offset,
		       const char *line, size_t len, int is_context)
{
	char sep = is_context ? '-' : ':';

	if (g->with_filename && !g->no_filename) {
		grep_paint(g, g->c_fn, stdout);
		fputs(name, stdout);
		grep_reset(g, stdout);
		if (g->null_out) {
			putchar('\0');
		} else {
			grep_paint(g, g->c_se, stdout);
			putchar(sep);
			grep_reset(g, stdout);
		}
	}
	if (g->line_numbers) {
		grep_paint(g, g->c_ln, stdout);
		printf("%ld", lineno);
		grep_reset(g, stdout);
		grep_paint(g, g->c_se, stdout);
		putchar(sep);
		grep_reset(g, stdout);
	}
	if (g->byte_offset) {
		grep_paint(g, g->c_ln, stdout);
		printf("%ld", offset);
		grep_reset(g, stdout);
		grep_paint(g, g->c_se, stdout);
		putchar(sep);
		grep_reset(g, stdout);
	}
	print_body(g, line, len, is_context);
	putchar('\n');
}

static int grep_stream(Grep *g, FILE *f, const char *name, long *total)
{
	Buf line;
	long lineno = 0;
	long matched = 0;
	long offset = 0;
	Vec before_buf;
	long before_start = 0;
	long after_left = 0;
	long last_printed = 0;
	int found = 0;

	buf_init(&line);
	vec_init(&before_buf);

	while (gnu_getdelim(&line, f, '\n') >= 0) {
		size_t len = line.len;
		int hit;
		RxMatch m;

		lineno++;
		if (len && line.b[len - 1] == '\n')
			len--;
		line.b[len] = '\0';

		hit = line_matches(g, line.b, len, &m, 0);
		if (g->invert)
			hit = !hit;

		if (hit) {
			found = 1;
			matched++;
			if (g->quiet) {
				buf_free(&line);
				vec_free(&before_buf);
				return 1;
			}
			if (g->files_with || g->files_without || g->count_only) {
				if (g->max_count && matched >= g->max_count)
					break;
				offset += (long)line.len;
				continue;
			}
			if (g->before) {
				size_t k;

				for (k = 0; k < before_buf.len; k++) {
					long ln = before_start + (long)k;

					if (ln <= last_printed)
						continue;
					print_line(g, name, ln, 0, before_buf.v[k],
						   strlen(before_buf.v[k]), 1);
					last_printed = ln;
				}
				vec_clear(&before_buf);
			}
			if (g->only_matching) {
				size_t from = 0;
				RxMatch mm;

				while (from <= len && line_matches(g, line.b, len, &mm, from)) {
					if (mm.end <= mm.start) {
						from = (size_t)mm.start + 1;
						continue;
					}
					if (g->with_filename && !g->no_filename) {
						grep_paint(g, g->c_fn, stdout);
						fputs(name, stdout);
						grep_reset(g, stdout);
						grep_paint(g, g->c_se, stdout);
						putchar(':');
						grep_reset(g, stdout);
					}
					if (g->line_numbers) {
						grep_paint(g, g->c_ln, stdout);
						printf("%ld", lineno);
						grep_reset(g, stdout);
						grep_paint(g, g->c_se, stdout);
						putchar(':');
						grep_reset(g, stdout);
					}
					grep_paint(g, g->c_ms, stdout);
					fwrite(line.b + mm.start, 1,
					       (size_t)(mm.end - mm.start), stdout);
					grep_reset(g, stdout);
					putchar('\n');
					from = (size_t)mm.end;
				}
			} else {
				print_line(g, name, lineno, offset, line.b, len, 0);
			}
			last_printed = lineno;
			after_left = g->after;
			if (g->max_count && matched >= g->max_count)
				break;
		} else {
			if (after_left > 0 && !g->only_matching) {
				print_line(g, name, lineno, offset, line.b, len, 1);
				last_printed = lineno;
				after_left--;
			} else if (g->before) {
				vec_push(&before_buf, xstrndup(line.b, len));
				if ((long)before_buf.len > g->before)
					free(vec_remove(&before_buf, 0));
				before_start = lineno - (long)before_buf.len + 1;
			}
		}
		offset += (long)line.len;
	}

	buf_free(&line);
	vec_free(&before_buf);
	if (total)
		*total += matched;
	if (g->count_only) {
		if (g->with_filename && !g->no_filename)
			printf("%s:", name);
		printf("%ld\n", matched);
	}
	return found;
}

static int name_selected(Grep *g, const char *path)
{
	const char *base = strrchr(path, '/');
	size_t i;

	base = base ? base + 1 : path;
	for (i = 0; i < g->exclude.len; i++)
		if (glob_match(g->exclude.v[i], base, 0))
			return 0;
	if (!g->include.len)
		return 1;
	for (i = 0; i < g->include.len; i++)
		if (glob_match(g->include.v[i], base, 0))
			return 1;
	return 0;
}

static int grep_path(Grep *g, const char *path, long *total);

static int grep_dir(Grep *g, const char *path, long *total)
{
	DIR *d = opendir(path);
	struct dirent *e;
	Vec names;
	size_t k;
	int found = 0;

	if (!d) {
		if (!g->no_messages)
			gnu_file_error("grep", path);
		return 0;
	}
	vec_init(&names);
	while ((e = readdir(d))) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		vec_pushs(&names, e->d_name);
	}
	closedir(d);
	vec_sort(&names);
	for (k = 0; k < names.len; k++) {
		char *full = xasprintf("%s/%s", path, names.v[k]);
		struct stat st;
		size_t j;
		int skip = 0;

		if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
			for (j = 0; j < g->exclude_dir.len; j++)
				if (glob_match(g->exclude_dir.v[j], names.v[k], 0))
					skip = 1;
		}
		if (!skip && grep_path(g, full, total))
			found = 1;
		free(full);
	}
	vec_free(&names);
	return found;
}

static int grep_path(Grep *g, const char *path, long *total)
{
	struct stat st;
	FILE *f;
	int found;

	if (strcmp(path, "-") == 0)
		return grep_stream(g, stdin, g->label ? g->label : "(standard input)", total);

	if (stat(path, &st) != 0) {
		if (!g->no_messages)
			gnu_file_error("grep", path);
		return -1;
	}
	if (S_ISDIR(st.st_mode)) {
		if (g->recursive)
			return grep_dir(g, path, total);
		if (!g->no_messages)
			gnu_error("grep", "%s: Is a directory", path);
		return 0;
	}
	if (!name_selected(g, path))
		return 0;
	f = fopen(path, "rb");
	if (!f) {
		if (!g->no_messages)
			gnu_file_error("grep", path);
		return -1;
	}
	found = grep_stream(g, f, path, total);
	fclose(f);
	if (found && g->files_with)
		printf("%s\n", path);
	else if (!found && g->files_without)
		printf("%s\n", path);
	return found;
}

static void add_pattern_lines(Vec *out, const char *text)
{
	const char *p = text;

	for (;;) {
		const char *nl = strchr(p, '\n');

		if (!nl) {
			vec_pushs(out, p);
			return;
		}
		vec_push(out, xstrndup(p, (size_t)(nl - p)));
		p = nl + 1;
		if (!*p)
			return;
	}
}

int gnu_grep(int argc, char **argv)
{
	Grep g;
	Vec patterns, files;
	int mode = RX_BRE;
	int have_pattern = 0;
	int i;
	size_t k;
	long total = 0;
	int any_found = 0, any_error = 0;
	const char *prog = argv[0];

	memset(&g, 0, sizeof g);
	/* GNU grep defaults to never; almost every Linux shell aliases it to
	 * auto, so CriSH starts at auto and says so in docs/gnu.md. */
	g.colour = COLOR_AUTO;
	grep_read_colors(&g);
	vec_init(&g.fixed);
	vec_init(&g.include);
	vec_init(&g.exclude);
	vec_init(&g.exclude_dir);
	vec_init(&patterns);
	vec_init(&files);

	{
		const char *slash = strrchr(prog, '/');
		if (slash)
			prog = slash + 1;
	}
	if (strcmp(prog, "egrep") == 0)
		mode = RX_ERE;
	else if (strcmp(prog, "fgrep") == 0)
		g.fixed_mode = 1;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (strcmp(a, "--") == 0) {
				i++;
				break;
			}
			if (gnu_long_opt(a, "extended-regexp", NULL)) {
				mode = RX_ERE;
				continue;
			}
			if (gnu_long_opt(a, "fixed-strings", NULL)) {
				g.fixed_mode = 1;
				continue;
			}
			if (gnu_long_opt(a, "perl-regexp", NULL)) {
				mode = RX_PCRE;
				continue;
			}
			if (gnu_long_opt(a, "basic-regexp", NULL)) {
				mode = RX_BRE;
				continue;
			}
			if (gnu_long_opt(a, "regexp", &val)) {
				add_pattern_lines(&patterns, val ? val : argv[++i]);
				have_pattern = 1;
				continue;
			}
			if (gnu_long_opt(a, "file", &val)) {
				const char *path = val ? val : argv[++i];
				FILE *pf = fopen(path, "r");
				Buf line;

				if (!pf) {
					gnu_file_error("grep", path);
					return 2;
				}
				buf_init(&line);
				while (gnu_getdelim(&line, pf, '\n') >= 0) {
					size_t n = line.len;
					if (n && line.b[n - 1] == '\n')
						n--;
					vec_push(&patterns, xstrndup(line.b, n));
				}
				buf_free(&line);
				fclose(pf);
				have_pattern = 1;
				continue;
			}
			if (gnu_long_opt(a, "ignore-case", NULL)) {
				g.ignore_case = 1;
				continue;
			}
			if (gnu_long_opt(a, "invert-match", NULL)) {
				g.invert = 1;
				continue;
			}
			if (gnu_long_opt(a, "word-regexp", NULL)) {
				g.word_match = 1;
				continue;
			}
			if (gnu_long_opt(a, "line-regexp", NULL)) {
				g.line_match = 1;
				continue;
			}
			if (gnu_long_opt(a, "count", NULL)) {
				g.count_only = 1;
				continue;
			}
			if (gnu_long_opt(a, "files-with-matches", NULL)) {
				g.files_with = 1;
				continue;
			}
			if (gnu_long_opt(a, "files-without-match", NULL)) {
				g.files_without = 1;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL) || gnu_long_opt(a, "silent", NULL)) {
				g.quiet = 1;
				continue;
			}
			if (gnu_long_opt(a, "line-number", NULL)) {
				g.line_numbers = 1;
				continue;
			}
			if (gnu_long_opt(a, "with-filename", NULL)) {
				g.with_filename = 1;
				continue;
			}
			if (gnu_long_opt(a, "no-filename", NULL)) {
				g.no_filename = 1;
				continue;
			}
			if (gnu_long_opt(a, "only-matching", NULL)) {
				g.only_matching = 1;
				continue;
			}
			if (gnu_long_opt(a, "recursive", NULL) ||
			    gnu_long_opt(a, "dereference-recursive", NULL)) {
				g.recursive = 1;
				continue;
			}
			if (gnu_long_opt(a, "no-messages", NULL)) {
				g.no_messages = 1;
				continue;
			}
			if (gnu_long_opt(a, "byte-offset", NULL)) {
				g.byte_offset = 1;
				continue;
			}
			if (gnu_long_opt(a, "null", NULL)) {
				g.null_out = 1;
				continue;
			}
			if (gnu_long_opt(a, "max-count", &val)) {
				g.max_count = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "after-context", &val)) {
				g.after = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "before-context", &val)) {
				g.before = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "context", &val)) {
				g.after = g.before = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "include", &val)) {
				vec_pushs(&g.include, val ? val : argv[++i]);
				continue;
			}
			if (gnu_long_opt(a, "exclude", &val)) {
				vec_pushs(&g.exclude, val ? val : argv[++i]);
				continue;
			}
			if (gnu_long_opt(a, "exclude-dir", &val)) {
				vec_pushs(&g.exclude_dir, val ? val : argv[++i]);
				continue;
			}
			if (gnu_long_opt(a, "label", &val)) {
				g.label = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "color", &val) || gnu_long_opt(a, "colour", &val)) {
				const char *when = val ? val : "auto";

				if (strcmp(when, "never") == 0 || strcmp(when, "none") == 0)
					g.colour = COLOR_NEVER;
				else if (strcmp(when, "always") == 0 ||
					 strcmp(when, "force") == 0 ||
					 strcmp(when, "yes") == 0)
					g.colour = COLOR_ALWAYS;
				else
					g.colour = COLOR_AUTO;
				continue;
			}
			if (gnu_long_opt(a, "binary-files", &val)) {
				if (!val)
					i++;
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: grep [OPTION]... PATTERN [FILE]...\n"
				       "Supported: -E -F -P -i -v -w -x -c -l -L -q -n -h -H "
				       "-o -r -s -b -m -A -B -C -e -f -Z\n"
				       "  --include --exclude --exclude-dir --label\n");
				return 0;
			}
			gnu_error("grep", "unrecognized option '%s'", a);
			return 2;
		}
		if (a[0] == '-' && a[1]) {
			const char *p;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'E': mode = RX_ERE; break;
				case 'F': g.fixed_mode = 1; break;
				case 'P': mode = RX_PCRE; break;
				case 'G': mode = RX_BRE; break;
				case 'i':
				case 'y': g.ignore_case = 1; break;
				case 'v': g.invert = 1; break;
				case 'w': g.word_match = 1; break;
				case 'x': g.line_match = 1; break;
				case 'c': g.count_only = 1; break;
				case 'l': g.files_with = 1; break;
				case 'L': g.files_without = 1; break;
				case 'q': g.quiet = 1; break;
				case 'n': g.line_numbers = 1; break;
				case 'H': g.with_filename = 1; break;
				case 'h': g.no_filename = 1; break;
				case 'o': g.only_matching = 1; break;
				case 'r':
				case 'R': g.recursive = 1; break;
				case 's': g.no_messages = 1; break;
				case 'b': g.byte_offset = 1; break;
				case 'Z': g.null_out = 1; break;
				case 'a': break;
				case 'e':
					add_pattern_lines(&patterns,
							  p[1] ? p + 1 : argv[++i]);
					have_pattern = 1;
					p = a + strlen(a) - 1;
					break;
				case 'f': {
					const char *path = p[1] ? p + 1 : argv[++i];
					FILE *pf = fopen(path, "r");
					Buf line;

					if (!pf) {
						gnu_file_error("grep", path);
						return 2;
					}
					buf_init(&line);
					while (gnu_getdelim(&line, pf, '\n') >= 0) {
						size_t n = line.len;
						if (n && line.b[n - 1] == '\n')
							n--;
						vec_push(&patterns, xstrndup(line.b, n));
					}
					buf_free(&line);
					fclose(pf);
					have_pattern = 1;
					p = a + strlen(a) - 1;
					break;
				}
				case 'm':
					g.max_count = strtol(p[1] ? p + 1 : argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				case 'A':
					g.after = strtol(p[1] ? p + 1 : argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				case 'B':
					g.before = strtol(p[1] ? p + 1 : argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				case 'C':
					g.after = g.before =
						strtol(p[1] ? p + 1 : argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				default:
					if (isdigit((unsigned char)*p)) {
						g.after = g.before = strtol(p, NULL, 10);
						p = a + strlen(a) - 1;
						break;
					}
					gnu_error("grep", "invalid option -- '%c'", *p);
					return 2;
				}
			}
			continue;
		}
		if (!have_pattern) {
			add_pattern_lines(&patterns, a);
			have_pattern = 1;
			continue;
		}
		vec_pushs(&files, a);
	}
	for (; i < argc; i++) {
		if (!have_pattern) {
			add_pattern_lines(&patterns, argv[i]);
			have_pattern = 1;
			continue;
		}
		vec_pushs(&files, argv[i]);
	}

	if (!have_pattern) {
		gnu_error("grep", "usage: grep [OPTION]... PATTERN [FILE]...");
		return 2;
	}

	if (g.fixed_mode) {
		for (k = 0; k < patterns.len; k++)
			vec_pushs(&g.fixed, patterns.v[k]);
	} else {
		g.rx = xcalloc(patterns.len ? patterns.len : 1, sizeof *g.rx);
		for (k = 0; k < patterns.len; k++) {
			const char *err = NULL;
			int flags = g.ignore_case ? RX_ICASE : 0;

			g.rx[g.nrx] = rx_compile(patterns.v[k], mode, flags, &err);
			if (!g.rx[g.nrx]) {
				gnu_error("grep", "%s: %s", patterns.v[k],
					  err ? err : "invalid pattern");
				return 2;
			}
			g.nrx++;
		}
	}

	g.colour_on = g.colour == COLOR_ALWAYS ||
		      (g.colour == COLOR_AUTO && !var_get("NO_COLOR") && isatty(1));
	if (!files.len) {
		if (g.recursive)
			vec_pushs(&files, ".");
		else
			vec_pushs(&files, "-");
	}
	if (files.len > 1 || g.recursive)
		g.with_filename = 1;

	for (k = 0; k < files.len; k++) {
		int r = grep_path(&g, files.v[k], &total);

		if (r < 0)
			any_error = 1;
		else if (r > 0)
			any_found = 1;
		if (g.quiet && any_found)
			break;
	}

	for (k = 0; k < g.nrx; k++)
		rx_free(g.rx[k]);
	free(g.rx);
	vec_free(&g.fixed);
	vec_free(&g.include);
	vec_free(&g.exclude);
	vec_free(&g.exclude_dir);
	vec_free(&patterns);
	vec_free(&files);
	free(g.c_ms);
	free(g.c_mc);
	free(g.c_sl);
	free(g.c_cx);
	free(g.c_fn);
	free(g.c_ln);
	free(g.c_se);

	if (any_error && !any_found)
		return 2;
	return any_found ? 0 : 1;
}
