/* filters.c - sort, uniq, cut, tr, shuf, xargs and expr.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../regex.h"
#include "gnu.h"

/* ------------------------------------------------------------- line slurping */

static void read_lines(const char *prog, Vec *files, Vec *out, int delim)
{
	size_t k;
	Buf line;

	buf_init(&line);
	for (k = 0; k < files->len; k++) {
		int is_stdin;
		FILE *f = gnu_open(prog, files->v[k], &is_stdin);

		if (!f)
			continue;
		while (gnu_getdelim(&line, f, delim) >= 0) {
			size_t n = line.len;

			if (n && line.b[n - 1] == delim)
				n--;
			vec_push(out, xstrndup(line.b, n));
		}
		gnu_close(f, is_stdin);
	}
	buf_free(&line);
}

/* -------------------------------------------------------------------- sort */

typedef struct {
	int field_start;  /* 1-based, 0 means whole line */
	int char_start;
	int field_end;
	int char_end;
	int numeric;
	int general;
	int human;
	int version;
	int reverse;
	int fold;
	int ignore_blanks;
	int month;
	int random;
} SortKey;

typedef struct {
	SortKey keys[16];
	int nkeys;
	int global_numeric;
	int global_reverse;
	int global_fold;
	int global_version;
	int global_human;
	int global_blanks;
	int global_general;
	int stable;
	const char *sep;
} SortOpts;

static SortOpts sort_opts;

/* Extract the text a key covers from one line. */
static char *key_text(const char *line, const SortKey *k)
{
	if (!k->field_start)
		return xstrdup(line);

	{
		Vec fields;
		Buf out;
		int i;
		const char *sep = sort_opts.sep;

		vec_init(&fields);
		if (sep) {
			const char *p = line;
			for (;;) {
				const char *q = strchr(p, sep[0]);
				if (!q) {
					vec_pushs(&fields, p);
					break;
				}
				vec_push(&fields, xstrndup(p, (size_t)(q - p)));
				p = q + 1;
			}
		} else {
			/* default: fields are separated by runs of blanks, and the
			 * separator belongs to the following field */
			const char *p = line;
			while (*p) {
				const char *start = p;

				while (*p && isblank((unsigned char)*p))
					p++;
				while (*p && !isblank((unsigned char)*p))
					p++;
				vec_push(&fields, xstrndup(start, (size_t)(p - start)));
			}
		}

		buf_init(&out);
		for (i = k->field_start; i <= (k->field_end ? k->field_end : (int)fields.len);
		     i++) {
			if (i < 1 || (size_t)i > fields.len)
				continue;
			if (out.len && sep)
				buf_putc(&out, sep[0]);
			buf_puts(&out, fields.v[i - 1]);
		}
		vec_free(&fields);

		{
			char *s = buf_take(&out);
			char *result;
			size_t len = strlen(s);
			size_t from = 0, to = len;

			if (k->char_start > 1 && (size_t)(k->char_start - 1) < len)
				from = (size_t)(k->char_start - 1);
			if (k->char_end > 0 && (size_t)k->char_end < to)
				to = (size_t)k->char_end;
			if (from > to)
				from = to;
			result = xstrndup(s + from, to - from);
			free(s);
			return result;
		}
	}
}

static double human_value(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	while (*end == ' ')
		end++;
	switch (*end) {
	case 'K': case 'k': v *= 1024; break;
	case 'M': case 'm': v *= 1024.0 * 1024; break;
	case 'G': case 'g': v *= 1024.0 * 1024 * 1024; break;
	case 'T': case 't': v *= 1024.0 * 1024 * 1024 * 1024; break;
	case 'P': v *= 1024.0 * 1024 * 1024 * 1024 * 1024; break;
	default: break;
	}
	return v;
}

/* Compare like `sort -V`: digit runs compare numerically. */
static int version_compare(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			long x, y;
			char *ea, *eb;

			while (*a == '0' && isdigit((unsigned char)a[1]))
				a++;
			while (*b == '0' && isdigit((unsigned char)b[1]))
				b++;
			x = strtol(a, &ea, 10);
			y = strtol(b, &eb, 10);
			if (x != y)
				return x < y ? -1 : 1;
			a = ea;
			b = eb;
			continue;
		}
		if (*a != *b)
			return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
		a++;
		b++;
	}
	if (*a)
		return 1;
	if (*b)
		return -1;
	return 0;
}

static int month_value(const char *s)
{
	static const char *const months[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
					      "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
	int i;

	while (isblank((unsigned char)*s))
		s++;
	for (i = 0; i < 12; i++)
		if (str_casecmp(months[i], s) == 0 || strncasecmp(months[i], s, 3) == 0)
			return i + 1;
	return 0;
}

static int compare_with(const char *a, const char *b, const SortKey *k)
{
	char *ta = key_text(a, k);
	char *tb = key_text(b, k);
	const char *pa = ta, *pb = tb;
	int r;

	if (k->ignore_blanks) {
		while (isblank((unsigned char)*pa))
			pa++;
		while (isblank((unsigned char)*pb))
			pb++;
	}
	if (k->numeric || k->general) {
		double x = strtod(pa, NULL), y = strtod(pb, NULL);
		r = x < y ? -1 : x > y ? 1 : 0;
	} else if (k->human) {
		double x = human_value(pa), y = human_value(pb);
		r = x < y ? -1 : x > y ? 1 : 0;
	} else if (k->version) {
		r = version_compare(pa, pb);
	} else if (k->month) {
		int x = month_value(pa), y = month_value(pb);
		r = x < y ? -1 : x > y ? 1 : 0;
	} else if (k->fold) {
		r = str_casecmp(pa, pb);
		if (r < 0)
			r = -1;
		else if (r > 0)
			r = 1;
	} else {
		r = strcmp(pa, pb);
		if (r < 0)
			r = -1;
		else if (r > 0)
			r = 1;
	}
	free(ta);
	free(tb);
	return k->reverse ? -r : r;
}

static int sort_compare(const void *va, const void *vb)
{
	const char *a = *(char *const *)va;
	const char *b = *(char *const *)vb;
	int i;

	for (i = 0; i < sort_opts.nkeys; i++) {
		int r = compare_with(a, b, &sort_opts.keys[i]);
		if (r)
			return r;
	}
	if (!sort_opts.nkeys) {
		SortKey whole;
		int r;

		memset(&whole, 0, sizeof whole);
		whole.numeric = sort_opts.global_numeric;
		whole.general = sort_opts.global_general;
		whole.human = sort_opts.global_human;
		whole.version = sort_opts.global_version;
		whole.fold = sort_opts.global_fold;
		whole.ignore_blanks = sort_opts.global_blanks;
		whole.reverse = sort_opts.global_reverse;
		r = compare_with(a, b, &whole);
		if (r)
			return r;
		return 0;
	}
	/* the last resort comparison, unless -s asked for stability */
	if (!sort_opts.stable) {
		int r = strcmp(a, b);
		if (r)
			return sort_opts.global_reverse ? -r : r;
	}
	return 0;
}

static void parse_key(const char *spec, SortKey *k)
{
	const char *p = spec;

	memset(k, 0, sizeof *k);
	k->field_start = (int)strtol(p, (char **)&p, 10);
	if (*p == '.')
		k->char_start = (int)strtol(p + 1, (char **)&p, 10);
	while (*p && *p != ',') {
		switch (*p) {
		case 'n': k->numeric = 1; break;
		case 'g': k->general = 1; break;
		case 'h': k->human = 1; break;
		case 'V': k->version = 1; break;
		case 'r': k->reverse = 1; break;
		case 'f': k->fold = 1; break;
		case 'b': k->ignore_blanks = 1; break;
		case 'M': k->month = 1; break;
		case 'R': k->random = 1; break;
		default: break;
		}
		p++;
	}
	if (*p == ',') {
		p++;
		k->field_end = (int)strtol(p, (char **)&p, 10);
		if (*p == '.')
			k->char_end = (int)strtol(p + 1, (char **)&p, 10);
		while (*p) {
			switch (*p) {
			case 'n': k->numeric = 1; break;
			case 'g': k->general = 1; break;
			case 'h': k->human = 1; break;
			case 'V': k->version = 1; break;
			case 'r': k->reverse = 1; break;
			case 'f': k->fold = 1; break;
			case 'b': k->ignore_blanks = 1; break;
			case 'M': k->month = 1; break;
			default: break;
			}
			p++;
		}
	}
	if (!k->field_start)
		k->field_start = 1;
}

int gnu_sort(int argc, char **argv)
{
	Vec files, lines;
	int unique = 0, check = 0, zero = 0, randomise = 0;
	const char *output = NULL;
	int i;
	size_t k;
	FILE *out = stdout;

	memset(&sort_opts, 0, sizeof sort_opts);
	vec_init(&files);
	vec_init(&lines);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (gnu_long_opt(a, "numeric-sort", NULL)) {
				sort_opts.global_numeric = 1;
				continue;
			}
			if (gnu_long_opt(a, "general-numeric-sort", NULL)) {
				sort_opts.global_general = 1;
				continue;
			}
			if (gnu_long_opt(a, "human-numeric-sort", NULL)) {
				sort_opts.global_human = 1;
				continue;
			}
			if (gnu_long_opt(a, "version-sort", NULL)) {
				sort_opts.global_version = 1;
				continue;
			}
			if (gnu_long_opt(a, "reverse", NULL)) {
				sort_opts.global_reverse = 1;
				continue;
			}
			if (gnu_long_opt(a, "unique", NULL)) {
				unique = 1;
				continue;
			}
			if (gnu_long_opt(a, "stable", NULL)) {
				sort_opts.stable = 1;
				continue;
			}
			if (gnu_long_opt(a, "ignore-case", NULL) ||
			    gnu_long_opt(a, "ignore-leading-blanks", NULL)) {
				sort_opts.global_fold = 1;
				continue;
			}
			if (gnu_long_opt(a, "zero-terminated", NULL)) {
				zero = 1;
				continue;
			}
			if (gnu_long_opt(a, "random-sort", NULL)) {
				randomise = 1;
				continue;
			}
			if (gnu_long_opt(a, "check", NULL)) {
				check = 1;
				continue;
			}
			if (gnu_long_opt(a, "key", &val)) {
				parse_key(val ? val : argv[++i],
					  &sort_opts.keys[sort_opts.nkeys++]);
				continue;
			}
			if (gnu_long_opt(a, "field-separator", &val)) {
				sort_opts.sep = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "output", &val)) {
				output = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "parallel", &val)) {
				if (!val)
					i++;
				continue; /* accepted and ignored */
			}
			if (gnu_long_opt(a, "buffer-size", &val) ||
			    gnu_long_opt(a, "temporary-directory", &val)) {
				if (!val)
					i++;
				continue;
			}
			continue;
		}
		if (a[0] == '-' && a[1]) {
			const char *p;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'n': sort_opts.global_numeric = 1; break;
				case 'g': sort_opts.global_general = 1; break;
				case 'h': sort_opts.global_human = 1; break;
				case 'V': sort_opts.global_version = 1; break;
				case 'r': sort_opts.global_reverse = 1; break;
				case 'f': sort_opts.global_fold = 1; break;
				case 'b': sort_opts.global_blanks = 1; break;
				case 'u': unique = 1; break;
				case 's': sort_opts.stable = 1; break;
				case 'c': check = 1; break;
				case 'z': zero = 1; break;
				case 'R': randomise = 1; break;
				case 'M': break;
				case 'k':
					if (p[1])
						parse_key(p + 1,
							  &sort_opts.keys[sort_opts.nkeys++]);
					else if (i + 1 < argc)
						parse_key(argv[++i],
							  &sort_opts.keys[sort_opts.nkeys++]);
					p = a + strlen(a) - 1;
					break;
				case 't':
					if (p[1])
						sort_opts.sep = p + 1;
					else if (i + 1 < argc)
						sort_opts.sep = argv[++i];
					p = a + strlen(a) - 1;
					break;
				case 'o':
					if (p[1])
						output = p + 1;
					else if (i + 1 < argc)
						output = argv[++i];
					p = a + strlen(a) - 1;
					break;
				default:
					gnu_error("sort", "invalid option -- '%c'", *p);
					vec_free(&files);
					vec_free(&lines);
					return 2;
				}
			}
			continue;
		}
		vec_pushs(&files, a);
	}
	if (!files.len)
		vec_pushs(&files, "-");

	/* propagate the global modifiers onto every key that has none */
	for (i = 0; i < sort_opts.nkeys; i++) {
		SortKey *k2 = &sort_opts.keys[i];

		if (!k2->numeric && !k2->general && !k2->human && !k2->version &&
		    !k2->fold && !k2->month) {
			k2->numeric = sort_opts.global_numeric;
			k2->general = sort_opts.global_general;
			k2->human = sort_opts.global_human;
			k2->version = sort_opts.global_version;
			k2->fold = sort_opts.global_fold;
		}
		if (!k2->reverse)
			k2->reverse = sort_opts.global_reverse;
		if (!k2->ignore_blanks)
			k2->ignore_blanks = sort_opts.global_blanks;
	}

	read_lines("sort", &files, &lines, zero ? '\0' : '\n');

	if (check) {
		for (k = 1; k < lines.len; k++) {
			if (sort_compare(&lines.v[k - 1], &lines.v[k]) > 0) {
				gnu_error("sort", "%s:%zu: disorder: %s",
					  files.v[0], k + 1, lines.v[k]);
				vec_free(&files);
				vec_free(&lines);
				return 1;
			}
		}
		vec_free(&files);
		vec_free(&lines);
		return 0;
	}

	if (randomise) {
		for (k = lines.len; k > 1; k--) {
			size_t j = (size_t)arc4random_uniform((uint32_t)k);
			char *tmp = lines.v[k - 1];
			lines.v[k - 1] = lines.v[j];
			lines.v[j] = tmp;
		}
	} else if (lines.len > 1) {
		/* mergesort is stable, which is what -s promises */
		mergesort(lines.v, lines.len, sizeof *lines.v, sort_compare);
	}

	if (output) {
		out = fopen(output, "w");
		if (!out) {
			gnu_file_error("sort", output);
			vec_free(&files);
			vec_free(&lines);
			return 2;
		}
	}
	for (k = 0; k < lines.len; k++) {
		if (unique && k && sort_compare(&lines.v[k - 1], &lines.v[k]) == 0)
			continue;
		fputs(lines.v[k], out);
		fputc(zero ? '\0' : '\n', out);
	}
	if (out != stdout)
		fclose(out);
	vec_free(&files);
	vec_free(&lines);
	return 0;
}

/* -------------------------------------------------------------------- uniq */

int gnu_uniq(int argc, char **argv)
{
	int count = 0, only_dup = 0, only_uniq = 0, all_dup = 0, fold = 0;
	long skip_fields = 0, skip_chars = 0, check_chars = -1;
	int zero = 0;
	int i;
	Vec files, lines;
	size_t k;
	FILE *out = stdout;

	vec_init(&files);
	vec_init(&lines);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (gnu_long_opt(a, "count", NULL)) {
				count = 1;
				continue;
			}
			if (gnu_long_opt(a, "repeated", NULL)) {
				only_dup = 1;
				continue;
			}
			if (gnu_long_opt(a, "all-repeated", &val)) {
				all_dup = 1;
				only_dup = 1;
				continue;
			}
			if (gnu_long_opt(a, "unique", NULL)) {
				only_uniq = 1;
				continue;
			}
			if (gnu_long_opt(a, "ignore-case", NULL)) {
				fold = 1;
				continue;
			}
			if (gnu_long_opt(a, "skip-fields", &val)) {
				skip_fields = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "skip-chars", &val)) {
				skip_chars = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "check-chars", &val)) {
				check_chars = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "zero-terminated", NULL)) {
				zero = 1;
				continue;
			}
			continue;
		}
		if (a[0] == '-' && a[1]) {
			const char *p;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'c': count = 1; break;
				case 'd': only_dup = 1; break;
				case 'D': all_dup = only_dup = 1; break;
				case 'u': only_uniq = 1; break;
				case 'i': fold = 1; break;
				case 'z': zero = 1; break;
				case 'f':
					skip_fields = p[1] ? strtol(p + 1, NULL, 10)
							   : strtol(argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				case 's':
					skip_chars = p[1] ? strtol(p + 1, NULL, 10)
							  : strtol(argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				case 'w':
					check_chars = p[1] ? strtol(p + 1, NULL, 10)
							   : strtol(argv[++i], NULL, 10);
					p = a + strlen(a) - 1;
					break;
				default:
					if (isdigit((unsigned char)*p)) {
						skip_fields = strtol(p, NULL, 10);
						p = a + strlen(a) - 1;
						break;
					}
					gnu_error("uniq", "invalid option -- '%c'", *p);
					vec_free(&files);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&files, a);
	}

	if (files.len > 1) {
		out = fopen(files.v[1], "w");
		if (!out) {
			gnu_file_error("uniq", files.v[1]);
			vec_free(&files);
			return 1;
		}
		free(vec_remove(&files, 1));
	}
	if (!files.len)
		vec_pushs(&files, "-");
	read_lines("uniq", &files, &lines, zero ? '\0' : '\n');

	{
		/* the part of a line that participates in the comparison */
		Vec keys;

		vec_init(&keys);
		for (k = 0; k < lines.len; k++) {
			const char *p = lines.v[k];
			long n;

			for (n = 0; n < skip_fields && *p; n++) {
				while (*p && isblank((unsigned char)*p))
					p++;
				while (*p && !isblank((unsigned char)*p))
					p++;
			}
			for (n = 0; n < skip_chars && *p; n++)
				p++;
			if (check_chars >= 0)
				vec_push(&keys, xstrndup(p, (size_t)check_chars));
			else
				vec_pushs(&keys, p);
		}

		k = 0;
		while (k < lines.len) {
			size_t run = 1;

			while (k + run < lines.len &&
			       (fold ? str_casecmp(keys.v[k], keys.v[k + run]) == 0
				     : strcmp(keys.v[k], keys.v[k + run]) == 0))
				run++;

			if ((only_dup && run < 2) || (only_uniq && run > 1)) {
				k += run;
				continue;
			}
			if (all_dup) {
				size_t j;
				for (j = 0; j < run; j++) {
					if (count)
						fprintf(out, "%7zu ", run);
					fputs(lines.v[k + j], out);
					fputc(zero ? '\0' : '\n', out);
				}
			} else {
				if (count)
					fprintf(out, "%7zu ", run);
				fputs(lines.v[k], out);
				fputc(zero ? '\0' : '\n', out);
			}
			k += run;
		}
		vec_free(&keys);
	}
	if (out != stdout)
		fclose(out);
	vec_free(&files);
	vec_free(&lines);
	return 0;
}

/* --------------------------------------------------------------------- cut */

/* Parse "1,3-5,7-" into a 0/1 selection bitmap; returns the highest index. */
static void parse_ranges(const char *spec, unsigned char *sel, size_t max, int *open_end)
{
	const char *p = spec;

	*open_end = 0;
	while (*p) {
		long lo = 0, hi = 0;

		if (*p == '-') {
			lo = 1;
		} else {
			lo = strtol(p, (char **)&p, 10);
		}
		if (*p == '-') {
			p++;
			if (isdigit((unsigned char)*p))
				hi = strtol(p, (char **)&p, 10);
			else
				hi = (long)max;
			if (hi == (long)max)
				*open_end = 1;
		} else {
			hi = lo;
		}
		if (lo < 1)
			lo = 1;
		while (lo <= hi && (size_t)lo < max)
			sel[lo++] = 1;
		if (*p == ',')
			p++;
		else if (*p)
			break;
	}
}

#define CUT_MAX 4096

int gnu_cut(int argc, char **argv)
{
	unsigned char sel[CUT_MAX];
	int open_end = 0;
	int mode = 0; /* 'f', 'c' or 'b' */
	const char *delim = "\t";
	const char *out_delim = NULL;
	int only_delim = 0, complement = 0, zero = 0;
	int i, rc = 0;
	Vec files;
	size_t k;
	Buf line;

	memset(sel, 0, sizeof sel);
	vec_init(&files);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (gnu_long_opt(a, "fields", &val)) {
				mode = 'f';
				parse_ranges(val ? val : argv[++i], sel, CUT_MAX, &open_end);
				continue;
			}
			if (gnu_long_opt(a, "characters", &val)) {
				mode = 'c';
				parse_ranges(val ? val : argv[++i], sel, CUT_MAX, &open_end);
				continue;
			}
			if (gnu_long_opt(a, "bytes", &val)) {
				mode = 'b';
				parse_ranges(val ? val : argv[++i], sel, CUT_MAX, &open_end);
				continue;
			}
			if (gnu_long_opt(a, "delimiter", &val)) {
				delim = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "output-delimiter", &val)) {
				out_delim = val ? val : argv[++i];
				continue;
			}
			if (gnu_long_opt(a, "complement", NULL)) {
				complement = 1;
				continue;
			}
			if (gnu_long_opt(a, "only-delimited", NULL)) {
				only_delim = 1;
				continue;
			}
			if (gnu_long_opt(a, "zero-terminated", NULL)) {
				zero = 1;
				continue;
			}
			continue;
		}
		if (a[0] == '-' && a[1]) {
			char opt = a[1];
			const char *rest = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : "");

			switch (opt) {
			case 'f':
			case 'c':
			case 'b':
				mode = opt;
				parse_ranges(rest, sel, CUT_MAX, &open_end);
				break;
			case 'd':
				delim = rest;
				break;
			case 's':
				only_delim = 1;
				if (!a[2] && rest != argv[i])
					i--;
				break;
			case 'n':
			case 'z':
				if (opt == 'z')
					zero = 1;
				if (!a[2])
					i--;
				break;
			default:
				gnu_error("cut", "invalid option -- '%c'", opt);
				vec_free(&files);
				return 1;
			}
			continue;
		}
		vec_pushs(&files, a);
	}
	if (!mode) {
		gnu_error("cut", "you must specify a list of bytes, characters or fields");
		vec_free(&files);
		return 1;
	}
	if (!out_delim)
		out_delim = mode == 'f' ? delim : "";
	if (!files.len)
		vec_pushs(&files, "-");

	buf_init(&line);
	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *f = gnu_open("cut", files.v[k], &is_stdin);

		if (!f) {
			rc = 1;
			continue;
		}
		while (gnu_getdelim(&line, f, zero ? '\0' : '\n') >= 0) {
			size_t n = line.len;
			int first = 1;

			if (n && (line.b[n - 1] == '\n' || line.b[n - 1] == '\0'))
				n--;
			line.b[n] = '\0';

			if (mode == 'f') {
				Vec fields;
				const char *p = line.b;
				size_t j;

				if (!strchr(line.b, delim[0])) {
					if (!only_delim) {
						fputs(line.b, stdout);
						putchar(zero ? '\0' : '\n');
					}
					continue;
				}
				vec_init(&fields);
				for (;;) {
					const char *q = strchr(p, delim[0]);
					if (!q) {
						vec_pushs(&fields, p);
						break;
					}
					vec_push(&fields, xstrndup(p, (size_t)(q - p)));
					p = q + 1;
				}
				for (j = 1; j <= fields.len; j++) {
					int want = j < CUT_MAX ? sel[j] : open_end;

					if (complement)
						want = !want;
					if (!want)
						continue;
					if (!first)
						fputs(out_delim, stdout);
					fputs(fields.v[j - 1], stdout);
					first = 0;
				}
				vec_free(&fields);
			} else {
				size_t j;

				for (j = 1; j <= n; j++) {
					int want = j < CUT_MAX ? sel[j] : open_end;

					if (complement)
						want = !want;
					if (!want)
						continue;
					if (!first && *out_delim)
						fputs(out_delim, stdout);
					putchar(line.b[j - 1]);
					first = 0;
				}
			}
			putchar(zero ? '\0' : '\n');
		}
		gnu_close(f, is_stdin);
	}
	buf_free(&line);
	vec_free(&files);
	return rc;
}

/* ---------------------------------------------------------------------- tr */

/* Expand ranges, [:classes:], [x*n] and escapes into a plain byte list. */
static void tr_expand(const char *spec, Buf *out)
{
	const char *p = spec;

	while (*p) {
		int c;

		if (*p == '[' && p[1] == ':') {
			const char *close = strstr(p, ":]");
			if (close) {
				size_t len = (size_t)(close - p - 2);
				int i;

				for (i = 0; i < 256; i++) {
					int hit = 0;

					if (len == 5 && !strncmp(p + 2, "alpha", 5))
						hit = isalpha(i);
					else if (len == 5 && !strncmp(p + 2, "digit", 5))
						hit = isdigit(i);
					else if (len == 5 && !strncmp(p + 2, "alnum", 5))
						hit = isalnum(i);
					else if (len == 5 && !strncmp(p + 2, "upper", 5))
						hit = isupper(i);
					else if (len == 5 && !strncmp(p + 2, "lower", 5))
						hit = islower(i);
					else if (len == 5 && !strncmp(p + 2, "space", 5))
						hit = isspace(i);
					else if (len == 5 && !strncmp(p + 2, "blank", 5))
						hit = (i == ' ' || i == '\t');
					else if (len == 5 && !strncmp(p + 2, "punct", 5))
						hit = ispunct(i);
					else if (len == 5 && !strncmp(p + 2, "print", 5))
						hit = isprint(i);
					else if (len == 5 && !strncmp(p + 2, "graph", 5))
						hit = isgraph(i);
					else if (len == 5 && !strncmp(p + 2, "cntrl", 5))
						hit = iscntrl(i);
					else if (len == 6 && !strncmp(p + 2, "xdigit", 6))
						hit = isxdigit(i);
					if (hit)
						buf_putc(out, i);
				}
				p = close + 2;
				continue;
			}
		}
		if (*p == '\\' && p[1]) {
			p++;
			switch (*p) {
			case 'n': c = '\n'; break;
			case 't': c = '\t'; break;
			case 'r': c = '\r'; break;
			case 'f': c = '\f'; break;
			case 'v': c = '\v'; break;
			case 'a': c = '\a'; break;
			case 'b': c = '\b'; break;
			case '\\': c = '\\'; break;
			default:
				if (*p >= '0' && *p <= '7') {
					int v = 0, n = 0;
					while (n < 3 && *p >= '0' && *p <= '7') {
						v = v * 8 + (*p - '0');
						p++;
						n++;
					}
					p--;
					c = v;
				} else {
					c = (unsigned char)*p;
				}
				break;
			}
			p++;
		} else {
			c = (unsigned char)*p++;
		}

		if (*p == '-' && p[1] && p[1] != ']') {
			int hi;
			p++;
			if (*p == '\\' && p[1]) {
				p++;
				switch (*p) {
				case 'n': hi = '\n'; break;
				case 't': hi = '\t'; break;
				case 'r': hi = '\r'; break;
				default: hi = (unsigned char)*p; break;
				}
				p++;
			} else {
				hi = (unsigned char)*p++;
			}
			while (c <= hi)
				buf_putc(out, c++);
			continue;
		}
		buf_putc(out, c);
	}
}

int gnu_tr(int argc, char **argv)
{
	int delete_mode = 0, squeeze = 0, complement = 0, truncate_set = 0;
	Vec sets;
	Buf set1, set2;
	int i, c;
	unsigned char map[256];
	unsigned char in_set1[256];
	unsigned char in_set2[256];

	vec_init(&sets);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] == '-' && a[1] && strcmp(a, "--") != 0 && !vec_argv(&sets)[0]) {
			const char *p;

			if (gnu_long_opt(a, "delete", NULL)) {
				delete_mode = 1;
				continue;
			}
			if (gnu_long_opt(a, "squeeze-repeats", NULL)) {
				squeeze = 1;
				continue;
			}
			if (gnu_long_opt(a, "complement", NULL)) {
				complement = 1;
				continue;
			}
			if (gnu_long_opt(a, "truncate-set1", NULL)) {
				truncate_set = 1;
				continue;
			}
			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'd': delete_mode = 1; break;
				case 's': squeeze = 1; break;
				case 'c':
				case 'C': complement = 1; break;
				case 't': truncate_set = 1; break;
				default:
					gnu_error("tr", "invalid option -- '%c'", *p);
					vec_free(&sets);
					return 1;
				}
			}
			continue;
		}
		vec_pushs(&sets, a);
	}
	if (!sets.len) {
		gnu_error("tr", "missing operand");
		vec_free(&sets);
		return 1;
	}

	buf_init(&set1);
	buf_init(&set2);
	tr_expand(sets.v[0], &set1);
	if (sets.len > 1)
		tr_expand(sets.v[1], &set2);

	memset(in_set1, 0, sizeof in_set1);
	memset(in_set2, 0, sizeof in_set2);
	for (i = 0; i < (int)set1.len; i++)
		in_set1[(unsigned char)set1.b[i]] = 1;
	for (i = 0; i < (int)set2.len; i++)
		in_set2[(unsigned char)set2.b[i]] = 1;
	if (complement) {
		for (i = 0; i < 256; i++)
			in_set1[i] = !in_set1[i];
	}

	for (i = 0; i < 256; i++)
		map[i] = (unsigned char)i;
	if (set2.len && !delete_mode) {
		if (complement) {
			unsigned char last = (unsigned char)set2.b[set2.len - 1];
			for (i = 0; i < 256; i++)
				if (in_set1[i])
					map[i] = last;
		} else {
			size_t j;
			for (j = 0; j < set1.len; j++) {
				unsigned char from = (unsigned char)set1.b[j];
				unsigned char to;

				if (j < set2.len)
					to = (unsigned char)set2.b[j];
				else if (truncate_set)
					continue;
				else
					to = (unsigned char)set2.b[set2.len - 1];
				map[from] = to;
			}
		}
	}

	{
		int last_out = -1;
		const unsigned char *squeeze_set = set2.len && !delete_mode ? in_set2 : in_set1;

		while ((c = getchar()) != EOF) {
			unsigned char u = (unsigned char)c;

			if (delete_mode && in_set1[u]) {
				continue;
			}
			if (!delete_mode && set2.len)
				u = map[u];
			if (squeeze && squeeze_set[u] && (int)u == last_out)
				continue;
			putchar(u);
			last_out = u;
		}
	}
	buf_free(&set1);
	buf_free(&set2);
	vec_free(&sets);
	return 0;
}

/* -------------------------------------------------------------------- shuf */

int gnu_shuf(int argc, char **argv)
{
	Vec files, lines;
	long count = -1;
	int echo_mode = 0, repeat = 0, zero = 0;
	long lo = 0, hi = -1;
	int i;
	size_t k;

	vec_init(&files);
	vec_init(&lines);
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] && !echo_mode) {
			if (gnu_long_opt(a, "head-count", &val)) {
				count = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "echo", NULL) || strcmp(a, "-e") == 0) {
				echo_mode = 1;
				continue;
			}
			if (gnu_long_opt(a, "input-range", &val) || strcmp(a, "-i") == 0) {
				const char *spec = val ? val : argv[++i];
				char *dash = strchr(spec, '-');

				lo = strtol(spec, NULL, 10);
				hi = dash ? strtol(dash + 1, NULL, 10) : lo;
				continue;
			}
			if (gnu_long_opt(a, "repeat", NULL) || strcmp(a, "-r") == 0) {
				repeat = 1;
				continue;
			}
			if (gnu_long_opt(a, "zero-terminated", NULL) || strcmp(a, "-z") == 0) {
				zero = 1;
				continue;
			}
			if (strcmp(a, "-n") == 0 && i + 1 < argc) {
				count = strtol(argv[++i], NULL, 10);
				continue;
			}
		}
		if (echo_mode)
			vec_pushs(&lines, a);
		else
			vec_pushs(&files, a);
	}

	if (hi >= lo && hi >= 0) {
		long n;
		char buf[32];

		for (n = lo; n <= hi; n++) {
			snprintf(buf, sizeof buf, "%ld", n);
			vec_pushs(&lines, buf);
		}
	} else if (!echo_mode) {
		if (!files.len)
			vec_pushs(&files, "-");
		read_lines("shuf", &files, &lines, zero ? '\0' : '\n');
	}

	if (!lines.len) {
		vec_free(&files);
		vec_free(&lines);
		return 0;
	}
	if (repeat) {
		long n = count < 0 ? (long)lines.len : count;

		while (n-- > 0) {
			fputs(lines.v[arc4random_uniform((uint32_t)lines.len)], stdout);
			putchar(zero ? '\0' : '\n');
		}
		vec_free(&files);
		vec_free(&lines);
		return 0;
	}
	for (k = lines.len; k > 1; k--) {
		size_t j = (size_t)arc4random_uniform((uint32_t)k);
		char *tmp = lines.v[k - 1];
		lines.v[k - 1] = lines.v[j];
		lines.v[j] = tmp;
	}
	for (k = 0; k < lines.len; k++) {
		if (count >= 0 && (long)k >= count)
			break;
		fputs(lines.v[k], stdout);
		putchar(zero ? '\0' : '\n');
	}
	vec_free(&files);
	vec_free(&lines);
	return 0;
}

/* ------------------------------------------------------------------- xargs */

static int run_xargs(Vec *cmd, int verbose)
{
	pid_t pid;
	int status = 0;

	if (verbose) {
		size_t k;
		for (k = 0; k < cmd->len; k++)
			fprintf(stderr, "%s%s", k ? " " : "", cmd->v[k]);
		fputc('\n', stderr);
	}
	pid = fork();
	if (pid < 0) {
		gnu_error("xargs", "%s", strerror(errno));
		return 1;
	}
	if (pid == 0) {
		char *path = path_lookup(cmd->v[0]);

		if (!path) {
			gnu_error("xargs", "%s: No such file or directory", cmd->v[0]);
			_exit(127);
		}
		execve(path, vec_argv(cmd), vars_environ());
		gnu_error("xargs", "%s: %s", cmd->v[0], strerror(errno));
		_exit(126);
	}
	waitpid(pid, &status, 0);
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int gnu_xargs(int argc, char **argv)
{
	int null_sep = 0, no_run_empty = 0, verbose = 0;
	long max_args = 0;
	const char *replace = NULL;
	int delim = 0;
	Vec base, items;
	int i;
	size_t k;
	int rc = 0;
	Buf token;
	int c;

	vec_init(&base);
	vec_init(&items);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] && !base.len) {
			if (gnu_long_opt(a, "null", NULL) || strcmp(a, "-0") == 0) {
				null_sep = 1;
				continue;
			}
			if (gnu_long_opt(a, "no-run-if-empty", NULL) ||
			    strcmp(a, "-r") == 0) {
				no_run_empty = 1;
				continue;
			}
			if (gnu_long_opt(a, "verbose", NULL) || strcmp(a, "-t") == 0) {
				verbose = 1;
				continue;
			}
			if (gnu_long_opt(a, "max-args", &val)) {
				max_args = strtol(val ? val : argv[++i], NULL, 10);
				continue;
			}
			if (gnu_long_opt(a, "replace", &val)) {
				replace = val ? val : "{}";
				max_args = 1;
				continue;
			}
			if (gnu_long_opt(a, "delimiter", &val)) {
				const char *d = val ? val : argv[++i];
				delim = d[0] == '\\' && d[1] == 'n' ? '\n' : d[0];
				continue;
			}
			if (gnu_long_opt(a, "max-procs", &val)) {
				if (!val)
					i++;
				continue; /* accepted, runs serially */
			}
			if (a[1] == 'n') {
				max_args = a[2] ? strtol(a + 2, NULL, 10)
						: strtol(argv[++i], NULL, 10);
				continue;
			}
			if (a[1] == 'I') {
				replace = a[2] ? a + 2 : argv[++i];
				max_args = 1;
				continue;
			}
			if (a[1] == 'P') {
				if (!a[2])
					i++;
				continue;
			}
			if (a[1] == 'd') {
				const char *d = a[2] ? a + 2 : argv[++i];
				delim = d[0] == '\\' && d[1] == 'n' ? '\n' : d[0];
				continue;
			}
		}
		vec_pushs(&base, a);
	}
	if (!base.len)
		vec_pushs(&base, "echo");

	/* read the items */
	buf_init(&token);
	{
		int sep = null_sep ? '\0' : (delim ? delim : -1);
		int in_quote = 0;

		while ((c = getchar()) != EOF) {
			if (sep >= 0) {
				if (c == sep) {
					vec_push(&items, xstrndup(token.b ? token.b : "",
								  token.len));
					buf_reset(&token);
					continue;
				}
				buf_putc(&token, c);
				continue;
			}
			if (in_quote) {
				if (c == in_quote) {
					in_quote = 0;
					continue;
				}
				buf_putc(&token, c);
				continue;
			}
			if (c == '\'' || c == '"') {
				in_quote = c;
				continue;
			}
			if (c == '\\') {
				int n = getchar();
				if (n != EOF)
					buf_putc(&token, n);
				continue;
			}
			if (isspace(c)) {
				if (token.len) {
					vec_push(&items, xstrndup(token.b, token.len));
					buf_reset(&token);
				}
				continue;
			}
			buf_putc(&token, c);
		}
		if (token.len)
			vec_push(&items, xstrndup(token.b, token.len));
	}
	buf_free(&token);

	if (!items.len) {
		if (no_run_empty || replace) {
			vec_free(&base);
			vec_free(&items);
			return 0;
		}
		rc = run_xargs(&base, verbose);
		vec_free(&base);
		vec_free(&items);
		return rc;
	}

	k = 0;
	while (k < items.len) {
		Vec cmd;
		size_t taken = 0;
		size_t j;

		vec_init(&cmd);
		if (replace) {
			for (j = 0; j < base.len; j++) {
				const char *found = strstr(base.v[j], replace);

				if (!found) {
					vec_pushs(&cmd, base.v[j]);
					continue;
				}
				{
					Buf b;

					buf_init(&b);
					buf_put(&b, base.v[j], (size_t)(found - base.v[j]));
					buf_puts(&b, items.v[k]);
					buf_puts(&b, found + strlen(replace));
					vec_push(&cmd, buf_take(&b));
				}
			}
			taken = 1;
		} else {
			for (j = 0; j < base.len; j++)
				vec_pushs(&cmd, base.v[j]);
			while (k + taken < items.len) {
				vec_pushs(&cmd, items.v[k + taken]);
				taken++;
				if (max_args && (long)taken >= max_args)
					break;
				if (!max_args && cmd.len > 4000)
					break;
			}
		}
		{
			int r = run_xargs(&cmd, verbose);
			if (r)
				rc = r;
			if (r == 255) {
				vec_free(&cmd);
				break;
			}
		}
		vec_free(&cmd);
		k += taken ? taken : 1;
	}
	vec_free(&base);
	vec_free(&items);
	return rc;
}

/* -------------------------------------------------------------------- expr */

/* A small recursive descent evaluator over the argument vector. */
typedef struct {
	char **argv;
	int argc;
	int pos;
	int err;
} Ex;

static char *ex_or(Ex *e);

static const char *ex_peek(Ex *e)
{
	return e->pos < e->argc ? e->argv[e->pos] : NULL;
}

static long ex_num(const char *s)
{
	return strtol(s ? s : "0", NULL, 10);
}

static int ex_is_num(const char *s)
{
	char *end;

	if (!s || !*s)
		return 0;
	strtol(s, &end, 10);
	return *end == '\0';
}

static char *ex_primary(Ex *e)
{
	const char *w = ex_peek(e);

	if (!w) {
		e->err = 2;
		return xstrdup("");
	}
	if (strcmp(w, "(") == 0) {
		char *v;
		e->pos++;
		v = ex_or(e);
		if (ex_peek(e) && strcmp(ex_peek(e), ")") == 0)
			e->pos++;
		else
			e->err = 2;
		return v;
	}
	if (strcmp(w, "length") == 0 && e->pos + 1 < e->argc) {
		e->pos += 2;
		return xasprintf("%zu", strlen(e->argv[e->pos - 1]));
	}
	if (strcmp(w, "substr") == 0 && e->pos + 3 < e->argc) {
		const char *s = e->argv[e->pos + 1];
		long from = ex_num(e->argv[e->pos + 2]);
		long len = ex_num(e->argv[e->pos + 3]);
		size_t slen = strlen(s);

		e->pos += 4;
		if (from < 1 || (size_t)from > slen || len <= 0)
			return xstrdup("");
		if ((size_t)(from - 1 + len) > slen)
			len = (long)slen - from + 1;
		return xstrndup(s + from - 1, (size_t)len);
	}
	if (strcmp(w, "index") == 0 && e->pos + 2 < e->argc) {
		const char *s = e->argv[e->pos + 1];
		const char *set = e->argv[e->pos + 2];
		size_t j;

		e->pos += 3;
		for (j = 0; s[j]; j++)
			if (strchr(set, s[j]))
				return xasprintf("%zu", j + 1);
		return xstrdup("0");
	}
	e->pos++;
	return xstrdup(w);
}

static char *ex_match(Ex *e)
{
	char *left = ex_primary(e);

	while (ex_peek(e) && (strcmp(ex_peek(e), ":") == 0 ||
			      strcmp(ex_peek(e), "match") == 0)) {
		char *pat;
		char *result;

		e->pos++;
		pat = ex_primary(e);
		{
			char *group = NULL;
			Buf anchored;

			buf_init(&anchored);
			if (pat[0] != '^')
				buf_putc(&anchored, '^');
			buf_puts(&anchored, pat);
			{
				char *full = buf_take(&anchored);
				int n = rx_matches_anchored(full, left, &group);

				if (group)
					result = group;
				else
					result = xasprintf("%d", n);
				free(full);
			}
		}
		free(pat);
		free(left);
		left = result;
	}
	return left;
}

static char *ex_mul(Ex *e)
{
	char *left = ex_match(e);

	for (;;) {
		const char *op = ex_peek(e);
		char *right;
		long a, b;

		if (!op || (strcmp(op, "*") && strcmp(op, "/") && strcmp(op, "%")))
			return left;
		e->pos++;
		right = ex_match(e);
		a = ex_num(left);
		b = ex_num(right);
		if ((*op == '/' || *op == '%') && b == 0) {
			gnu_error("expr", "division by zero");
			e->err = 2;
			free(right);
			return left;
		}
		free(left);
		left = xasprintf("%ld", *op == '*' ? a * b : *op == '/' ? a / b : a % b);
		free(right);
	}
}

static char *ex_add(Ex *e)
{
	char *left = ex_mul(e);

	for (;;) {
		const char *op = ex_peek(e);
		char *right;

		if (!op || (strcmp(op, "+") && strcmp(op, "-")))
			return left;
		e->pos++;
		right = ex_mul(e);
		{
			long v = *op == '+' ? ex_num(left) + ex_num(right)
					    : ex_num(left) - ex_num(right);
			free(left);
			free(right);
			left = xasprintf("%ld", v);
		}
	}
}

static char *ex_cmp(Ex *e)
{
	char *left = ex_add(e);

	for (;;) {
		const char *op = ex_peek(e);
		char *right;
		int r;

		if (!op || (strcmp(op, "=") && strcmp(op, "==") && strcmp(op, "!=") &&
			    strcmp(op, "<") && strcmp(op, "<=") && strcmp(op, ">") &&
			    strcmp(op, ">=")))
			return left;
		e->pos++;
		right = ex_add(e);
		if (ex_is_num(left) && ex_is_num(right)) {
			long a = ex_num(left), b = ex_num(right);
			r = a < b ? -1 : a > b ? 1 : 0;
		} else {
			r = strcmp(left, right);
			r = r < 0 ? -1 : r > 0 ? 1 : 0;
		}
		{
			int truth = 0;

			if (!strcmp(op, "=") || !strcmp(op, "=="))
				truth = r == 0;
			else if (!strcmp(op, "!="))
				truth = r != 0;
			else if (!strcmp(op, "<"))
				truth = r < 0;
			else if (!strcmp(op, "<="))
				truth = r <= 0;
			else if (!strcmp(op, ">"))
				truth = r > 0;
			else
				truth = r >= 0;
			free(left);
			free(right);
			left = xasprintf("%d", truth);
		}
	}
}

static int ex_true(const char *s)
{
	return s && *s && strcmp(s, "0") != 0;
}

static char *ex_and(Ex *e)
{
	char *left = ex_cmp(e);

	while (ex_peek(e) && strcmp(ex_peek(e), "&") == 0) {
		char *right;

		e->pos++;
		right = ex_cmp(e);
		if (!ex_true(left) || !ex_true(right)) {
			free(left);
			left = xstrdup("0");
		}
		free(right);
	}
	return left;
}

static char *ex_or(Ex *e)
{
	char *left = ex_and(e);

	while (ex_peek(e) && strcmp(ex_peek(e), "|") == 0) {
		char *right;

		e->pos++;
		right = ex_and(e);
		if (!ex_true(left)) {
			free(left);
			left = right;
		} else {
			free(right);
		}
	}
	return left;
}

int gnu_expr(int argc, char **argv)
{
	Ex e;
	char *result;
	int rc;

	if (argc > 1 && gnu_long_opt(argv[1], "help", NULL)) {
		printf("usage: expr EXPRESSION\n");
		return 0;
	}
	e.argv = argv + 1;
	e.argc = argc - 1;
	e.pos = 0;
	e.err = 0;
	if (!e.argc) {
		gnu_error("expr", "missing operand");
		return 2;
	}
	result = ex_or(&e);
	if (e.err || e.pos != e.argc) {
		gnu_error("expr", "syntax error");
		free(result);
		return 2;
	}
	puts(result);
	rc = ex_true(result) ? 0 : 1;
	free(result);
	return rc;
}
