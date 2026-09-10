/* sed.c - a GNU-compatible stream editor.
 *
 * The headline difference from the BSD sed on a Mac is -i: here the suffix is
 * attached to the option (`-i` or `-i.bak`), never taken from the next
 * argument, so `sed -i 's/a/b/' file` edits the file instead of destroying it.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../regex.h"
#include "gnu.h"

/* ------------------------------------------------------------- the program */

typedef enum {
	A_NONE,
	A_LINE,
	A_LAST,
	A_REGEX,
	A_STEP,   /* first~step */
	A_REL,    /* addr2 of the form +N */
	A_MULT,   /* addr2 of the form ~N */
	A_ZERO    /* 0,/re/ */
} AddrType;

typedef struct {
	AddrType type;
	long line;
	long step;
	Rx *rx;
} Addr;

typedef struct Cmd {
	Addr a1, a2;
	int negate;
	int has_range;
	int range_active;
	long range_end_line;
	char name;

	/* s /// */
	Rx *rx;
	char *replacement;
	int global;
	int print_after;
	int occurrence;
	char *wfile;
	FILE *wfp;

	/* y/// */
	unsigned char trans[256];

	char *text;  /* a, i, c, r, R, b, t, T, : label */
	struct Cmd *block; /* { ... } */
	struct Cmd *next;
} Cmd;

typedef struct {
	Cmd *prog;
	int quiet;
	int mode;      /* RX_BRE or RX_ERE */
	int separate;
	int null_data;
	long line_limit;
} Sed;

static Sed sed;

/* ---------------------------------------------------------------- parsing */

static const char *skip_blank(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

static char *collect_delim(const char **pp, int delim, int keep_escape)
{
	const char *p = *pp;
	Buf b;

	buf_init(&b);
	while (*p && *p != delim) {
		if (*p == '\\' && p[1]) {
			if (p[1] == delim) {
				buf_putc(&b, delim);
				p += 2;
				continue;
			}
			if (keep_escape)
				buf_putc(&b, '\\');
			buf_putc(&b, p[1]);
			p += 2;
			continue;
		}
		buf_putc(&b, *p++);
	}
	*pp = p;
	return buf_take(&b);
}

static int parse_addr(const char **pp, Addr *a, const char *prog)
{
	const char *p = skip_blank(*pp);

	a->type = A_NONE;
	a->rx = NULL;

	if (*p == '$') {
		a->type = A_LAST;
		*pp = p + 1;
		return 1;
	}
	if (isdigit((unsigned char)*p)) {
		char *end;

		a->line = strtol(p, &end, 10);
		p = end;
		if (*p == '~') {
			a->step = strtol(p + 1, &end, 10);
			p = end;
			a->type = A_STEP;
		} else {
			a->type = a->line == 0 ? A_ZERO : A_LINE;
		}
		*pp = p;
		return 1;
	}
	if (*p == '/' || *p == '\\') {
		int delim = '/';
		char *pat;
		int flags = 0;
		const char *err = NULL;

		if (*p == '\\') {
			p++;
			delim = (unsigned char)*p;
		}
		p++;
		pat = collect_delim(&p, delim, 1);
		if (*p != delim) {
			gnu_error(prog, "unterminated address regex");
			free(pat);
			return -1;
		}
		p++;
		while (*p == 'I' || *p == 'M') {
			if (*p == 'I')
				flags |= RX_ICASE;
			else
				flags |= RX_MULTILINE;
			p++;
		}
		if (*pat) {
			a->rx = rx_compile(pat, sed.mode, flags, &err);
			if (!a->rx) {
				gnu_error(prog, "%s: %s", pat, err ? err : "bad regex");
				free(pat);
				return -1;
			}
		}
		free(pat);
		a->type = A_REGEX;
		*pp = p;
		return 1;
	}
	*pp = p;
	return 0;
}

static Cmd *parse_script(const char **pp, const char *prog, int in_block);

static int parse_s(Cmd *c, const char **pp, const char *prog)
{
	const char *p = *pp;
	int delim;
	char *pat;
	int flags = 0;
	const char *err = NULL;

	if (!*p) {
		gnu_error(prog, "unterminated `s' command");
		return 0;
	}
	delim = (unsigned char)*p++;
	pat = collect_delim(&p, delim, 1);
	if (*p != delim) {
		gnu_error(prog, "unterminated `s' command");
		free(pat);
		return 0;
	}
	p++;
	c->replacement = collect_delim(&p, delim, 1);
	if (*p != delim) {
		gnu_error(prog, "unterminated `s' command");
		free(pat);
		return 0;
	}
	p++;

	for (;;) {
		if (*p == 'g') {
			c->global = 1;
			p++;
		} else if (*p == 'p') {
			c->print_after = 1;
			p++;
		} else if (*p == 'i' || *p == 'I') {
			flags |= RX_ICASE;
			p++;
		} else if (*p == 'm' || *p == 'M') {
			flags |= RX_MULTILINE;
			p++;
		} else if (*p == 'e') {
			p++;
		} else if (isdigit((unsigned char)*p)) {
			char *end;
			c->occurrence = (int)strtol(p, &end, 10);
			p = end;
		} else if (*p == 'w') {
			p = skip_blank(p + 1);
			{
				Buf b;
				buf_init(&b);
				while (*p && *p != '\n' && *p != ';')
					buf_putc(&b, *p++);
				c->wfile = buf_take(&b);
			}
			break;
		} else {
			break;
		}
	}

	c->rx = rx_compile(pat, sed.mode, flags, &err);
	if (!c->rx) {
		gnu_error(prog, "%s: %s", pat, err ? err : "bad regex");
		free(pat);
		return 0;
	}
	free(pat);
	*pp = p;
	return 1;
}

static int parse_y(Cmd *c, const char **pp, const char *prog)
{
	const char *p = *pp;
	int delim;
	char *from, *to;
	size_t i;

	if (!*p) {
		gnu_error(prog, "unterminated `y' command");
		return 0;
	}
	delim = (unsigned char)*p++;
	from = collect_delim(&p, delim, 0);
	if (*p != delim) {
		gnu_error(prog, "unterminated `y' command");
		free(from);
		return 0;
	}
	p++;
	to = collect_delim(&p, delim, 0);
	if (*p != delim || strlen(from) != strlen(to)) {
		gnu_error(prog, "strings for `y' command are different lengths");
		free(from);
		free(to);
		return 0;
	}
	p++;
	for (i = 0; i < 256; i++)
		c->trans[i] = (unsigned char)i;
	for (i = 0; from[i]; i++)
		c->trans[(unsigned char)from[i]] = (unsigned char)to[i];
	free(from);
	free(to);
	*pp = p;
	return 1;
}

/* a\ text, i\ text, c\ text and the one-line GNU forms. */
static char *parse_text(const char **pp)
{
	const char *p = *pp;
	Buf b;

	buf_init(&b);
	if (*p == '\\')
		p++;
	if (*p == '\n')
		p++;
	p = skip_blank(p);
	while (*p) {
		if (*p == '\\' && p[1]) {
			p++;
			if (*p == 'n')
				buf_putc(&b, '\n');
			else if (*p == 't')
				buf_putc(&b, '\t');
			else if (*p == '\n')
				buf_putc(&b, '\n');
			else
				buf_putc(&b, *p);
			p++;
			continue;
		}
		if (*p == '\n')
			break;
		buf_putc(&b, *p++);
	}
	*pp = p;
	return buf_take(&b);
}

static char *parse_label(const char **pp)
{
	const char *p = skip_blank(*pp);
	Buf b;

	buf_init(&b);
	while (*p && *p != '\n' && *p != ';' && *p != '}')
		buf_putc(&b, *p++);
	*pp = p;
	{
		char *s = buf_take(&b);
		char *trimmed = xstrdup(str_trim(s));
		free(s);
		return trimmed;
	}
}

static Cmd *parse_script(const char **pp, const char *prog, int in_block)
{
	Cmd *head = NULL, **tail = &head;
	const char *p = *pp;

	for (;;) {
		Cmd *c;
		int r;

		while (*p == ';' || *p == '\n' || *p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		if (*p == '}') {
			if (in_block) {
				p++;
				break;
			}
			gnu_error(prog, "unexpected `}'");
			return head;
		}
		if (*p == '#') {
			while (*p && *p != '\n')
				p++;
			continue;
		}

		c = xcalloc(1, sizeof *c);
		r = parse_addr(&p, &c->a1, prog);
		if (r < 0) {
			free(c);
			return head;
		}
		if (r > 0) {
			p = skip_blank(p);
			if (*p == ',') {
				p++;
				c->has_range = 1;
				p = skip_blank(p);
				if (*p == '+') {
					char *end;
					p++;
					c->a2.type = A_REL;
					c->a2.line = strtol(p, &end, 10);
					p = end;
				} else if (*p == '~') {
					char *end;
					p++;
					c->a2.type = A_MULT;
					c->a2.line = strtol(p, &end, 10);
					p = end;
				} else if (parse_addr(&p, &c->a2, prog) < 0) {
					free(c);
					return head;
				}
			}
		}
		p = skip_blank(p);
		while (*p == '!') {
			c->negate = !c->negate;
			p = skip_blank(p + 1);
		}
		if (!*p) {
			gnu_error(prog, "missing command");
			free(c);
			return head;
		}

		c->name = *p++;
		switch (c->name) {
		case 's':
			if (!parse_s(c, &p, prog)) {
				free(c);
				return head;
			}
			break;
		case 'y':
			if (!parse_y(c, &p, prog)) {
				free(c);
				return head;
			}
			break;
		case 'a':
		case 'i':
		case 'c':
			c->text = parse_text(&p);
			break;
		case 'r':
		case 'R':
		case 'w':
		case 'W':
			c->text = parse_label(&p);
			break;
		case 'b':
		case 't':
		case 'T':
		case ':':
			c->text = parse_label(&p);
			break;
		case 'q':
		case 'Q':
			p = skip_blank(p);
			if (isdigit((unsigned char)*p)) {
				char *end;
				c->occurrence = (int)strtol(p, &end, 10);
				p = end;
			}
			break;
		case '{':
			c->block = parse_script(&p, prog, 1);
			break;
		case '=':
		case 'd':
		case 'D':
		case 'g':
		case 'G':
		case 'h':
		case 'H':
		case 'l':
		case 'n':
		case 'N':
		case 'p':
		case 'P':
		case 'x':
		case 'z':
		case 'F':
			break;
		default:
			gnu_error(prog, "unknown command: `%c'", c->name);
			free(c);
			return head;
		}

		*tail = c;
		tail = &c->next;
	}
	*pp = p;
	return head;
}

/* --------------------------------------------------------------- execution */

typedef struct {
	Buf pattern;
	Buf hold;
	Buf append;
	long lineno;
	int last_line;
	int substituted;
	int quit;
	int quit_code;
	int suppress_print;
	const char *filename;
	FILE *in;
	FILE *out;
} Run;

static int addr_matches(Cmd *c, Addr *a, Run *r)
{
	switch (a->type) {
	case A_LINE:
	case A_ZERO:
		return r->lineno == a->line;
	case A_LAST:
		return r->last_line;
	case A_REGEX:
		if (!a->rx)
			return 0;
		return rx_search(a->rx, r->pattern.b ? r->pattern.b : "", r->pattern.len, 0,
				 NULL);
	case A_STEP:
		if (a->step <= 0)
			return r->lineno == a->line;
		return r->lineno >= a->line && (r->lineno - a->line) % a->step == 0;
	default:
		return 0;
	}
	(void)c;
}

static int cmd_selected(Cmd *c, Run *r)
{
	int hit;

	if (c->a1.type == A_NONE) {
		hit = 1;
	} else if (!c->has_range) {
		hit = addr_matches(c, &c->a1, r);
	} else if (c->range_active) {
		hit = 1;
		switch (c->a2.type) {
		case A_LINE:
			if (r->lineno >= c->a2.line)
				c->range_active = 0;
			break;
		case A_LAST:
			if (r->last_line)
				c->range_active = 0;
			break;
		case A_REGEX:
			if (addr_matches(c, &c->a2, r))
				c->range_active = 0;
			break;
		case A_REL:
			if (r->lineno >= c->range_end_line)
				c->range_active = 0;
			break;
		case A_MULT:
			if (c->a2.line > 0 && r->lineno % c->a2.line == 0)
				c->range_active = 0;
			break;
		default:
			c->range_active = 0;
			break;
		}
		if (r->last_line)
			c->range_active = 0;
	} else {
		int start = c->a1.type == A_ZERO ? r->lineno >= 1 : addr_matches(c, &c->a1, r);

		hit = start;
		if (start) {
			c->range_active = 1;
			if (c->a2.type == A_REL)
				c->range_end_line = r->lineno + c->a2.line;
			/* a range whose end is already past closes immediately */
			if (c->a2.type == A_LINE && c->a2.line <= r->lineno)
				c->range_active = 0;
			if (c->a1.type == A_ZERO && addr_matches(c, &c->a2, r))
				c->range_active = 0;
			if (r->last_line)
				c->range_active = 0;
		}
	}
	return c->negate ? !hit : hit;
}

/* GNU replacement text: &, \1..\9, \n, \U \L \u \l \E */
static void expand_replacement(const char *rep, const char *src, const RxMatch *m, Buf *out)
{
	const char *p;
	int upper_all = 0, lower_all = 0, upper_one = 0, lower_one = 0;

	for (p = rep; *p; p++) {
		const char *piece = NULL;
		size_t piece_len = 0;
		char single = 0;

		if (*p == '&') {
			piece = src + m->start;
			piece_len = (size_t)(m->end - m->start);
		} else if (*p == '\\' && p[1]) {
			p++;
			if (*p >= '0' && *p <= '9') {
				int g = *p - '0';

				if (g <= m->ngroups && m->group[g].start >= 0 &&
				    m->group[g].end >= m->group[g].start) {
					piece = src + m->group[g].start;
					piece_len = (size_t)(m->group[g].end -
							     m->group[g].start);
				} else {
					continue;
				}
			} else {
				switch (*p) {
				case 'n': single = '\n'; break;
				case 't': single = '\t'; break;
				case 'r': single = '\r'; break;
				case 'f': single = '\f'; break;
				case 'v': single = '\v'; break;
				case 'a': single = '\a'; break;
				case 'U': upper_all = 1; lower_all = 0; continue;
				case 'L': lower_all = 1; upper_all = 0; continue;
				case 'E': upper_all = lower_all = 0; continue;
				case 'u': upper_one = 1; continue;
				case 'l': lower_one = 1; continue;
				default: single = *p; break;
				}
			}
		} else {
			single = *p;
		}

		if (piece) {
			size_t i;

			for (i = 0; i < piece_len; i++) {
				int ch = (unsigned char)piece[i];

				if (upper_one) {
					ch = toupper(ch);
					upper_one = 0;
				} else if (lower_one) {
					ch = tolower(ch);
					lower_one = 0;
				} else if (upper_all) {
					ch = toupper(ch);
				} else if (lower_all) {
					ch = tolower(ch);
				}
				buf_putc(out, ch);
			}
			continue;
		}
		{
			int ch = (unsigned char)single;

			if (upper_one) {
				ch = toupper(ch);
				upper_one = 0;
			} else if (lower_one) {
				ch = tolower(ch);
				lower_one = 0;
			} else if (upper_all) {
				ch = toupper(ch);
			} else if (lower_all) {
				ch = tolower(ch);
			}
			buf_putc(out, ch);
		}
	}
}

static void do_substitute(Cmd *c, Run *r)
{
	const char *src = r->pattern.b ? r->pattern.b : "";
	size_t len = r->pattern.len;
	size_t pos = 0;
	int n = 0;
	int want = c->occurrence ? c->occurrence : 1;
	Buf out;
	int changed = 0;
	RxMatch m;

	buf_init(&out);
	while (pos <= len && rx_search(c->rx, src, len, pos, &m)) {
		n++;
		if (n < want) {
			size_t step = m.end > m.start ? (size_t)m.end : (size_t)m.start + 1;

			buf_put(&out, src + pos, (step > len ? len : step) - pos);
			pos = step > len ? len : step;
			if (pos >= len)
				break;
			continue;
		}
		buf_put(&out, src + pos, (size_t)m.start - pos);
		expand_replacement(c->replacement, src, &m, &out);
		changed = 1;
		if (m.end > m.start) {
			pos = (size_t)m.end;
		} else {
			if ((size_t)m.end < len)
				buf_putc(&out, src[m.end]);
			pos = (size_t)m.end + 1;
		}
		if (!c->global)
			break;
	}
	if (!changed) {
		buf_free(&out);
		return;
	}
	if (pos < len)
		buf_put(&out, src + pos, len - pos);

	buf_free(&r->pattern);
	r->pattern = out;
	r->substituted = 1;

	if (c->print_after) {
		fwrite(r->pattern.b, 1, r->pattern.len, r->out);
		fputc('\n', r->out);
	}
	if (c->wfile) {
		if (!c->wfp)
			c->wfp = strcmp(c->wfile, "/dev/stdout") == 0 ? stdout
								      : fopen(c->wfile, "w");
		if (c->wfp) {
			fwrite(r->pattern.b, 1, r->pattern.len, c->wfp);
			fputc('\n', c->wfp);
			fflush(c->wfp);
		}
	}
}

static void print_unambiguous(const char *s, size_t len, FILE *out)
{
	size_t i;

	for (i = 0; i < len; i++) {
		unsigned char ch = (unsigned char)s[i];

		switch (ch) {
		case '\\': fputs("\\\\", out); break;
		case '\a': fputs("\\a", out); break;
		case '\b': fputs("\\b", out); break;
		case '\f': fputs("\\f", out); break;
		case '\n': fputs("\\n", out); break;
		case '\r': fputs("\\r", out); break;
		case '\t': fputs("\\t", out); break;
		case '\v': fputs("\\v", out); break;
		default:
			if (isprint(ch))
				fputc(ch, out);
			else
				fprintf(out, "\\%03o", ch);
			break;
		}
	}
	fputs("$\n", out);
}

/* Returns: 0 normal, 1 start the next cycle (d), 2 restart without reading (D). */
static int run_commands(Cmd *list, Run *r);

static int find_label(Cmd *list, const char *label, Cmd **found)
{
	Cmd *c;

	for (c = list; c; c = c->next) {
		if (c->name == ':' && c->text && strcmp(c->text, label) == 0) {
			*found = c;
			return 1;
		}
		if (c->block && find_label(c->block, label, found))
			return 1;
	}
	return 0;
}

static int read_next_line(Run *r)
{
	Buf line;
	long got;
	int c;

	buf_init(&line);
	got = gnu_getdelim(&line, r->in, sed.null_data ? '\0' : '\n');
	if (got < 0) {
		buf_free(&line);
		return 0;
	}
	if (line.len && (line.b[line.len - 1] == '\n' || line.b[line.len - 1] == '\0'))
		line.b[--line.len] = '\0';
	buf_free(&r->pattern);
	r->pattern = line;
	r->lineno++;

	c = getc(r->in);
	if (c == EOF)
		r->last_line = 1;
	else
		ungetc(c, r->in);
	return 1;
}

static void flush_append(Run *r)
{
	if (r->append.len) {
		fwrite(r->append.b, 1, r->append.len, r->out);
		buf_reset(&r->append);
	}
}

static int run_commands(Cmd *list, Run *r)
{
	Cmd *c = list;

	while (c) {
		int selected = cmd_selected(c, r);

		if (!selected) {
			c = c->next;
			continue;
		}
		switch (c->name) {
		case '{': {
			int rc = run_commands(c->block, r);
			if (rc)
				return rc;
			break;
		}
		case 's':
			do_substitute(c, r);
			break;
		case 'y': {
			size_t i;
			for (i = 0; i < r->pattern.len; i++)
				r->pattern.b[i] =
					(char)c->trans[(unsigned char)r->pattern.b[i]];
			break;
		}
		case 'p':
			fwrite(r->pattern.b ? r->pattern.b : "", 1, r->pattern.len, r->out);
			fputc('\n', r->out);
			break;
		case 'P': {
			const char *nl = r->pattern.b ? memchr(r->pattern.b, '\n',
							       r->pattern.len)
						      : NULL;
			size_t n = nl ? (size_t)(nl - r->pattern.b) : r->pattern.len;

			fwrite(r->pattern.b ? r->pattern.b : "", 1, n, r->out);
			fputc('\n', r->out);
			break;
		}
		case 'd':
			return 1;
		case 'D': {
			const char *nl = r->pattern.b ? memchr(r->pattern.b, '\n',
							       r->pattern.len)
						      : NULL;
			if (!nl)
				return 1;
			{
				size_t off = (size_t)(nl - r->pattern.b) + 1;
				memmove(r->pattern.b, r->pattern.b + off,
					r->pattern.len - off + 1);
				r->pattern.len -= off;
			}
			return 2;
		}
		case '=':
			fprintf(r->out, "%ld\n", r->lineno);
			break;
		case 'l':
			print_unambiguous(r->pattern.b ? r->pattern.b : "", r->pattern.len,
					  r->out);
			break;
		case 'n':
			if (!sed.quiet) {
				fwrite(r->pattern.b ? r->pattern.b : "", 1, r->pattern.len,
				       r->out);
				fputc('\n', r->out);
			}
			flush_append(r);
			if (!read_next_line(r)) {
				r->suppress_print = 1;
				return 1;
			}
			break;
		case 'N': {
			Buf saved = r->pattern;

			buf_init(&r->pattern);
			flush_append(r);
			if (!read_next_line(r)) {
				r->pattern = saved;
				/* GNU prints the pattern space and exits */
				return sed.quiet ? 1 : 0;
			}
			{
				Buf joined;

				buf_init(&joined);
				buf_put(&joined, saved.b ? saved.b : "", saved.len);
				buf_putc(&joined, '\n');
				buf_put(&joined, r->pattern.b ? r->pattern.b : "",
					r->pattern.len);
				buf_free(&saved);
				buf_free(&r->pattern);
				r->pattern = joined;
			}
			break;
		}
		case 'h':
			buf_reset(&r->hold);
			buf_put(&r->hold, r->pattern.b ? r->pattern.b : "", r->pattern.len);
			break;
		case 'H':
			buf_putc(&r->hold, '\n');
			buf_put(&r->hold, r->pattern.b ? r->pattern.b : "", r->pattern.len);
			break;
		case 'g':
			buf_reset(&r->pattern);
			buf_put(&r->pattern, r->hold.b ? r->hold.b : "", r->hold.len);
			break;
		case 'G':
			buf_putc(&r->pattern, '\n');
			buf_put(&r->pattern, r->hold.b ? r->hold.b : "", r->hold.len);
			break;
		case 'x': {
			Buf tmp = r->pattern;
			r->pattern = r->hold;
			r->hold = tmp;
			break;
		}
		case 'z':
			buf_reset(&r->pattern);
			break;
		case 'F':
			fprintf(r->out, "%s\n", r->filename ? r->filename : "-");
			break;
		case 'a':
			buf_puts(&r->append, c->text ? c->text : "");
			buf_putc(&r->append, '\n');
			break;
		case 'i':
			fputs(c->text ? c->text : "", r->out);
			fputc('\n', r->out);
			break;
		case 'c':
			if (!c->has_range || !c->range_active) {
				fputs(c->text ? c->text : "", r->out);
				fputc('\n', r->out);
			}
			return 1;
		case 'r': {
			FILE *f = c->text ? fopen(c->text, "r") : NULL;

			if (f) {
				char chunk[4096];
				size_t n;

				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					buf_put(&r->append, chunk, n);
				fclose(f);
			}
			break;
		}
		case 'w':
		case 'W':
			if (!c->wfp && c->text)
				c->wfp = strcmp(c->text, "/dev/stdout") == 0
						 ? stdout
						 : fopen(c->text, "w");
			if (c->wfp) {
				size_t n = r->pattern.len;

				if (c->name == 'W') {
					const char *nl = memchr(r->pattern.b, '\n', n);
					if (nl)
						n = (size_t)(nl - r->pattern.b);
				}
				fwrite(r->pattern.b ? r->pattern.b : "", 1, n, c->wfp);
				fputc('\n', c->wfp);
				fflush(c->wfp);
			}
			break;
		case 'q':
			r->quit = 1;
			r->quit_code = c->occurrence;
			return 0;
		case 'Q':
			r->quit = 1;
			r->quit_code = c->occurrence;
			r->suppress_print = 1;
			return 1;
		case 'b':
			if (!c->text || !*c->text)
				return 3; /* end of script */
			{
				Cmd *target = NULL;
				if (find_label(sed.prog, c->text, &target)) {
					c = target->next;
					continue;
				}
				gnu_error("sed", "can't find label for jump to `%s'", c->text);
				return 3;
			}
		case 't':
		case 'T': {
			int want = c->name == 't' ? r->substituted : !r->substituted;

			if (!want)
				break;
			r->substituted = 0;
			if (!c->text || !*c->text)
				return 3;
			{
				Cmd *target = NULL;
				if (find_label(sed.prog, c->text, &target)) {
					c = target->next;
					continue;
				}
				gnu_error("sed", "can't find label for jump to `%s'", c->text);
				return 3;
			}
		}
		case ':':
			break;
		default:
			break;
		}
		c = c->next;
	}
	return 0;
}

static int sed_stream(FILE *in, FILE *out, const char *name)
{
	Run r;
	int exit_code = 0;

	memset(&r, 0, sizeof r);
	buf_init(&r.pattern);
	buf_init(&r.hold);
	buf_init(&r.append);
	r.in = in;
	r.out = out;
	r.filename = name;

	while (read_next_line(&r)) {
		int rc;

		r.substituted = 0;
		r.suppress_print = 0;
	restart:
		rc = run_commands(sed.prog, &r);
		if (rc == 2) {
			r.substituted = 0;
			goto restart;
		}
		if (rc != 1 && !sed.quiet && !r.suppress_print) {
			fwrite(r.pattern.b ? r.pattern.b : "", 1, r.pattern.len, out);
			fputc(sed.null_data ? '\0' : '\n', out);
		}
		flush_append(&r);
		if (r.quit) {
			exit_code = r.quit_code;
			break;
		}
		if (sed.line_limit && r.lineno >= sed.line_limit)
			break;
	}

	buf_free(&r.pattern);
	buf_free(&r.hold);
	buf_free(&r.append);
	return exit_code;
}

/* Reset the range state so that -s and -i start each file cleanly. */
static void reset_ranges(Cmd *c)
{
	for (; c; c = c->next) {
		c->range_active = 0;
		if (c->block)
			reset_ranges(c->block);
	}
}

int gnu_sed(int argc, char **argv)
{
	Buf script;
	int have_script = 0;
	int in_place = 0;
	const char *suffix = "";
	Vec files;
	int i;
	size_t k;
	int rc = 0;
	const char *prog = "sed";

	memset(&sed, 0, sizeof sed);
	sed.mode = RX_BRE;
	buf_init(&script);
	vec_init(&files);

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		const char *val;

		if (a[0] == '-' && a[1] == '-') {
			if (strcmp(a, "--") == 0) {
				i++;
				break;
			}
			if (gnu_long_opt(a, "expression", &val)) {
				if (script.len)
					buf_putc(&script, '\n');
				buf_puts(&script, val ? val : argv[++i]);
				have_script = 1;
				continue;
			}
			if (gnu_long_opt(a, "file", &val)) {
				const char *path = val ? val : argv[++i];
				FILE *f = fopen(path, "r");
				char chunk[4096];
				size_t n;

				if (!f) {
					gnu_file_error(prog, path);
					return 1;
				}
				if (script.len)
					buf_putc(&script, '\n');
				while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
					buf_put(&script, chunk, n);
				fclose(f);
				have_script = 1;
				continue;
			}
			if (gnu_long_opt(a, "in-place", &val)) {
				in_place = 1;
				sed.separate = 1;
				if (val)
					suffix = val;
				continue;
			}
			if (gnu_long_opt(a, "quiet", NULL) ||
			    gnu_long_opt(a, "silent", NULL)) {
				sed.quiet = 1;
				continue;
			}
			if (gnu_long_opt(a, "regexp-extended", NULL)) {
				sed.mode = RX_ERE;
				continue;
			}
			if (gnu_long_opt(a, "separate", NULL)) {
				sed.separate = 1;
				continue;
			}
			if (gnu_long_opt(a, "null-data", NULL)) {
				sed.null_data = 1;
				continue;
			}
			if (gnu_long_opt(a, "posix", NULL) ||
			    gnu_long_opt(a, "unbuffered", NULL) ||
			    gnu_long_opt(a, "follow-symlinks", NULL) ||
			    gnu_long_opt(a, "debug", NULL) ||
			    gnu_long_opt(a, "sandbox", NULL))
				continue;
			if (gnu_long_opt(a, "line-length", &val)) {
				if (!val)
					i++;
				continue;
			}
			if (gnu_long_opt(a, "help", NULL)) {
				printf("usage: sed [-nEsz] [-i[SUFFIX]] [-e script] "
				       "[-f file] [script] [file ...]\n"
				       "  -i takes its suffix attached, as GNU sed does.\n");
				return 0;
			}
			gnu_error(prog, "unrecognized option '%s'", a);
			return 1;
		}
		if (a[0] == '-' && a[1]) {
			const char *p;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'n': sed.quiet = 1; break;
				case 'E':
				case 'r': sed.mode = RX_ERE; break;
				case 's': sed.separate = 1; break;
				case 'z': sed.null_data = 1; break;
				case 'u': break;
				case 'i':
					/* GNU: the suffix is attached, never a separate arg */
					in_place = 1;
					sed.separate = 1;
					if (p[1])
						suffix = p + 1;
					p = a + strlen(a) - 1;
					break;
				case 'e':
					if (script.len)
						buf_putc(&script, '\n');
					buf_puts(&script, p[1] ? p + 1 : argv[++i]);
					have_script = 1;
					p = a + strlen(a) - 1;
					break;
				case 'f': {
					const char *path = p[1] ? p + 1 : argv[++i];
					FILE *f = fopen(path, "r");
					char chunk[4096];
					size_t n;

					if (!f) {
						gnu_file_error(prog, path);
						return 1;
					}
					if (script.len)
						buf_putc(&script, '\n');
					while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
						buf_put(&script, chunk, n);
					fclose(f);
					have_script = 1;
					p = a + strlen(a) - 1;
					break;
				}
				case 'l':
					if (!p[1])
						i++;
					p = a + strlen(a) - 1;
					break;
				default:
					gnu_error(prog, "invalid option -- '%c'", *p);
					return 1;
				}
			}
			continue;
		}
		if (!have_script) {
			buf_puts(&script, a);
			have_script = 1;
			continue;
		}
		vec_pushs(&files, a);
	}
	for (; i < argc; i++) {
		if (!have_script) {
			buf_puts(&script, argv[i]);
			have_script = 1;
			continue;
		}
		vec_pushs(&files, argv[i]);
	}

	if (!have_script) {
		gnu_error(prog, "no script specified");
		return 1;
	}
	/* `sed -i '' 's/a/b/' f` is the BSD idiom; here the empty string was
	 * taken as the script, so say what happened rather than failing oddly. */
	if (in_place && script.len == 0 && files.len >= 2) {
		gnu_error(prog, "-i is a GNU option here; the suffix is attached, so "
			       "the empty argument became the script");
		gnu_error(prog, "  write:  sed -i '%s' %s", files.v[0], files.v[1]);
		buf_free(&script);
		vec_free(&files);
		return 1;
	}
	{
		const char *p = script.b ? script.b : "";

		sed.prog = parse_script(&p, prog, 0);
	}
	if (!sed.prog) {
		buf_free(&script);
		vec_free(&files);
		return 1;
	}

	if (!files.len) {
		if (in_place) {
			gnu_error(prog, "no input files while in place editing");
			return 1;
		}
		vec_pushs(&files, "-");
	}

	for (k = 0; k < files.len; k++) {
		int is_stdin;
		FILE *in;
		FILE *out = stdout;
		char *tmpname = NULL;

		if (sed.separate || in_place)
			reset_ranges(sed.prog);

		in = gnu_open(prog, files.v[k], &is_stdin);
		if (!in) {
			rc = 2;
			continue;
		}
		if (in_place && !is_stdin) {
			tmpname = xasprintf("%s.crish-sed-XXXXXX", files.v[k]);
			{
				int fd = mkstemp(tmpname);

				if (fd < 0) {
					gnu_file_error(prog, tmpname);
					free(tmpname);
					gnu_close(in, is_stdin);
					rc = 2;
					continue;
				}
				out = fdopen(fd, "w");
			}
		}

		{
			int r = sed_stream(in, out, files.v[k]);

			if (r)
				rc = r;
		}
		gnu_close(in, is_stdin);

		if (in_place && tmpname) {
			struct stat st;

			fclose(out);
			if (stat(files.v[k], &st) == 0)
				chmod(tmpname, st.st_mode);
			if (*suffix) {
				char *backup = str_prefix(suffix, "*")
						       ? NULL
						       : xasprintf("%s%s", files.v[k], suffix);
				if (backup) {
					rename(files.v[k], backup);
					free(backup);
				}
			}
			if (rename(tmpname, files.v[k]) != 0) {
				gnu_file_error(prog, files.v[k]);
				rc = 2;
			}
			free(tmpname);
		}
	}

	buf_free(&script);
	vec_free(&files);
	fflush(stdout);
	return rc;
}
