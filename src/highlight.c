/* highlight.c - the tolerant scanner behind the coloured line editor.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "highlight.h"
#include "shell.h"

/* ------------------------------------------------------------------ cache */

/* A failing path_lookup() walks every PATH entry and stats each one.  Doing
 * that on every keystroke, for a word that does not exist yet, is exactly the
 * common case while someone is typing, so the answers are remembered. */

#define CACHE_SLOTS 512

typedef struct {
	char *key;
	int value;
} Slot;

static Slot cmd_cache[CACHE_SLOTS];
static Slot path_cache[CACHE_SLOTS];
static size_t cmd_used, path_used;
static char *cached_path_env;

static unsigned long hash_of(const char *s)
{
	unsigned long h = 5381;

	while (*s)
		h = h * 33 + (unsigned char)*s++;
	return h;
}

static void cache_clear(Slot *table, size_t *used)
{
	size_t i;

	for (i = 0; i < CACHE_SLOTS; i++) {
		free(table[i].key);
		table[i].key = NULL;
	}
	*used = 0;
}

void highlight_invalidate(void)
{
	cache_clear(cmd_cache, &cmd_used);
	cache_clear(path_cache, &path_used);
}

/* Drop everything when PATH changed under us. */
static void check_path_env(void)
{
	const char *now = var_get("PATH");

	if (!now)
		now = "";
	if (cached_path_env && strcmp(cached_path_env, now) == 0)
		return;
	free(cached_path_env);
	cached_path_env = xstrdup(now);
	highlight_invalidate();
}

static int *cache_find(Slot *table, size_t *used, const char *key, int *hit)
{
	unsigned long h = hash_of(key) % CACHE_SLOTS;
	size_t probe;

	for (probe = 0; probe < CACHE_SLOTS; probe++) {
		Slot *s = &table[(h + probe) % CACHE_SLOTS];

		if (!s->key) {
			/* free slot: keep some headroom so probing stays short */
			if (*used * 4 >= CACHE_SLOTS * 3) {
				cache_clear(table, used);
				return cache_find(table, used, key, hit);
			}
			s->key = xstrdup(key);
			(*used)++;
			*hit = 0;
			return &s->value;
		}
		if (strcmp(s->key, key) == 0) {
			*hit = 1;
			return &s->value;
		}
	}
	cache_clear(table, used);
	return cache_find(table, used, key, hit);
}

/* -------------------------------------------------------- classifications */

ColorRole highlight_command_role(const char *word)
{
	int hit = 0;
	int *slot;

	if (!word || !*word)
		return C_CMD_MISSING;

	check_path_env();
	slot = cache_find(cmd_cache, &cmd_used, word, &hit);
	if (hit)
		return (ColorRole)*slot;

	if (alias_get(word))
		*slot = C_ALIAS;
	else if (func_find(word))
		*slot = C_FUNCTION;
	else if (builtin_find(word))
		*slot = C_BUILTIN;
	else if (sh.shopt.gnu_builtins && gnu_find(word))
		*slot = C_GNU;
	else {
		char *found = path_lookup(word);

		if (found) {
			*slot = C_CMD_OK;
			free(found);
		} else {
			*slot = C_CMD_MISSING;
		}
	}
	return (ColorRole)*slot;
}

/* Only words that actually look like a path are stat'ed. */
static int looks_like_path(const char *word)
{
	if (!word || !*word)
		return 0;
	if (word[0] == '/' || word[0] == '~')
		return 1;
	if (word[0] == '.' && (word[1] == '/' || (word[1] == '.' && word[2] == '/')))
		return 1;
	return strchr(word, '/') != NULL;
}

static int path_exists(const char *word)
{
	int hit = 0;
	int *slot;
	struct stat st;

	if (!looks_like_path(word))
		return 0;
	slot = cache_find(path_cache, &path_used, word, &hit);
	if (hit)
		return *slot;

	if (word[0] == '~') {
		const char *home = var_get("HOME");
		char *full = xasprintf("%s%s", home ? home : "", word + 1);

		*slot = lstat(full, &st) == 0;
		free(full);
	} else {
		*slot = lstat(word, &st) == 0;
	}
	return *slot;
}

static int is_keyword(const char *w)
{
	static const char *const words[] = {
		"if", "then", "elif", "else", "fi", "while", "until", "do", "done",
		"for", "in", "case", "esac", "function", "select", "time", "!",
		"[[", "]]", "((", "))", NULL
	};
	int i;

	for (i = 0; words[i]; i++)
		if (strcmp(words[i], w) == 0)
			return 1;
	return 0;
}

/* After these, the next word is a command again. */
static int keyword_keeps_command_position(const char *w)
{
	static const char *const words[] = { "if",   "then", "elif", "else", "while",
					     "until", "do",  "time", "!",    NULL };
	int i;

	for (i = 0; words[i]; i++)
		if (strcmp(words[i], w) == 0)
			return 1;
	return 0;
}

/* ---------------------------------------------------------------- scanner */

typedef struct {
	Span *spans;
	size_t count;
	size_t cap;
} SpanList;

static void span_add(SpanList *l, size_t start, size_t len, ColorRole role)
{
	if (!len)
		return;
	if (l->count == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 32;
		l->spans = xrealloc(l->spans, l->cap * sizeof *l->spans);
	}
	l->spans[l->count].start = start;
	l->spans[l->count].len = len;
	l->spans[l->count].role = role;
	l->count++;
}

static int is_meta(int c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '|' || c == '&' ||
	       c == ';' || c == '(' || c == ')' || c == '<' || c == '>';
}

/* $var, ${...}, $(...), $((...)), $'...' — returns the length consumed. */
static void scan(const char *line, size_t len, size_t base, SpanList *list);

static size_t scan_dollar(const char *s, size_t len, size_t i, size_t base, SpanList *out)
{
	size_t start = i;

	i++; /* past $ */
	if (i >= len) {
		span_add(out, base + start, 1, C_VAR);
		return 1;
	}
	if (s[i] == '(') {
		int depth = 0;
		size_t j = i;
		int arith = i + 1 < len && s[i + 1] == '(';

		for (; j < len; j++) {
			if (s[j] == '(')
				depth++;
			else if (s[j] == ')' && --depth == 0) {
				j++;
				break;
			}
		}
		if (arith) {
			span_add(out, base + start, j - start, C_SUBST);
			return j - start;
		}
		/* a command substitution: colour the brackets, then look inside so
		 * that the command in there is checked like any other */
		span_add(out, base + start, 2, C_SUBST);
		{
			size_t inner = start + 2;
			size_t inner_len = j > inner ? j - inner - 1 : 0;

			if (inner_len)
				scan(s + inner, inner_len, base + inner, out);
			if (j > start + 2)
				span_add(out, base + j - 1, 1, C_SUBST);
		}
		return j - start;
	}
	if (s[i] == '{') {
		int depth = 0;
		size_t j = i;

		for (; j < len; j++) {
			if (s[j] == '{')
				depth++;
			else if (s[j] == '}' && --depth == 0) {
				j++;
				break;
			}
		}
		span_add(out, base + start, j - start, C_VAR);
		return j - start;
	}
	if (s[i] == '\'') {
		/* $'...' with escapes highlighted inside */
		size_t j = i + 1;

		span_add(out, base + start, 2, C_STRING);
		while (j < len && s[j] != '\'') {
			if (s[j] == '\\' && j + 1 < len) {
				span_add(out, base + j, 2, C_ESCAPE);
				j += 2;
				continue;
			}
			span_add(out, base + j, 1, C_STRING);
			j++;
		}
		if (j < len)
			span_add(out, base + j, 1, C_STRING);
		return (j < len ? j + 1 : j) - start;
	}
	if (isalpha((unsigned char)s[i]) || s[i] == '_') {
		size_t j = i;

		while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_'))
			j++;
		span_add(out, base + start, j - start, C_VAR);
		return j - start;
	}
	if (strchr("?$!#*@-0123456789", s[i])) {
		span_add(out, base + start, 2, C_VAR);
		return 2;
	}
	span_add(out, base + start, 1, C_VAR);
	return 1;
}

/* A double-quoted run: the quotes and text are string coloured, but the
 * expansions inside keep their own colour, which is the useful part. */
static size_t scan_dquote(const char *s, size_t len, size_t i, size_t base,
			  SpanList *out)
{
	size_t start = i;
	size_t run = i;

	span_add(out, base + i, 1, C_STRING);
	i++;
	run = i;
	while (i < len && s[i] != '"') {
		if (s[i] == '\\' && i + 1 < len) {
			span_add(out, base + run, i - run, C_STRING);
			span_add(out, base + i, 2, C_ESCAPE);
			i += 2;
			run = i;
			continue;
		}
		if (s[i] == '$' || s[i] == '`') {
			span_add(out, base + run, i - run, C_STRING);
			if (s[i] == '`') {
				size_t j = i + 1;

				while (j < len && s[j] != '`')
					j++;
				span_add(out, base + i, (j < len ? j + 1 : j) - i, C_SUBST);
				i = j < len ? j + 1 : j;
			} else {
				i += scan_dollar(s, len, i, base, out);
			}
			run = i;
			continue;
		}
		i++;
	}
	span_add(out, base + run, i - run, C_STRING);
	if (i < len) {
		span_add(out, base + i, 1, C_STRING);
		i++;
	}
	return i - start;
}

/* NAME=, NAME+=, NAME[i]= at the head of a command is an assignment prefix,
 * not the command itself. */
static size_t assignment_prefix(const char *w, size_t len)
{
	size_t i = 0;

	if (!len || !(isalpha((unsigned char)w[0]) || w[0] == '_'))
		return 0;
	while (i < len && (isalnum((unsigned char)w[i]) || w[i] == '_'))
		i++;
	if (i < len && w[i] == '[') {
		while (i < len && w[i] != ']')
			i++;
		if (i < len)
			i++;
	}
	if (i < len && w[i] == '+')
		i++;
	return i < len && w[i] == '=' ? i : 0;
}

/* What the scanner is waiting for, so that `for f in ...` reads properly. */
enum { EX_NONE, EX_VARNAME, EX_IN };

static void scan(const char *line, size_t len, size_t base, SpanList *list)
{
	size_t i = 0;
	int cmd_position = 1;
	int expect = EX_NONE;

	while (i < len) {
		unsigned char c = (unsigned char)line[i];

		if (c == ' ' || c == '\t') {
			i++;
			continue;
		}
		if (c == '\n') {
			cmd_position = 1;
			expect = EX_NONE;
			i++;
			continue;
		}
		if (c == '#' && (i == 0 || isspace((unsigned char)line[i - 1]))) {
			size_t j = i;

			while (j < len && line[j] != '\n')
				j++;
			span_add(list, base + i, j - i, C_COMMENT);
			i = j;
			continue;
		}
		if (c == '\\' && i + 1 < len) {
			span_add(list, base + i, 2, C_ESCAPE);
			i += 2;
			continue;
		}
		if (c == '\'') {
			size_t j = i + 1;

			while (j < len && line[j] != '\'')
				j++;
			span_add(list, base + i, (j < len ? j + 1 : j) - i, C_STRING);
			i = j < len ? j + 1 : j;
			cmd_position = 0;
			continue;
		}
		if (c == '"') {
			i += scan_dquote(line, len, i, base, list);
			cmd_position = 0;
			continue;
		}
		if (c == '`') {
			size_t j = i + 1;

			while (j < len && line[j] != '`')
				j++;
			span_add(list, base + i, 1, C_SUBST);
			if (j > i + 1)
				scan(line + i + 1, j - i - 1, base + i + 1, list);
			if (j < len)
				span_add(list, base + j, 1, C_SUBST);
			i = j < len ? j + 1 : j;
			cmd_position = 0;
			continue;
		}
		if (c == '$') {
			i += scan_dollar(line, len, i, base, list);
			cmd_position = 0;
			continue;
		}
		if (c == '|' || c == ';') {
			size_t n = (i + 1 < len && line[i + 1] == (char)c) ? 2 : 1;

			if (c == '|' && i + 1 < len && line[i + 1] == '&')
				n = 2;
			span_add(list, base + i, n, C_OPERATOR);
			i += n;
			cmd_position = 1;
			expect = EX_NONE;
			continue;
		}
		if (c == '&') {
			if (i + 1 < len && line[i + 1] == '>') {
				size_t n = (i + 2 < len && line[i + 2] == '>') ? 3 : 2;

				span_add(list, base + i, n, C_REDIR);
				i += n;
				cmd_position = 0;
				continue;
			}
			{
				size_t n = (i + 1 < len && line[i + 1] == '&') ? 2 : 1;

				span_add(list, base + i, n, C_OPERATOR);
				i += n;
				cmd_position = 1;
				expect = EX_NONE;
			}
			continue;
		}
		if (c == '<' || c == '>') {
			size_t j = i + 1;

			if (j < len && (line[j] == '>' || line[j] == '<' || line[j] == '&' ||
					line[j] == '|'))
				j++;
			if (j < len && line[j] == '<' && line[i] == '<' && line[i + 1] == '<')
				j++;
			/* a descriptor after >& or <& belongs to the redirection */
			while (j < len && isdigit((unsigned char)line[j]))
				j++;
			if (j < len && line[j] == '-' && j > i + 1)
				j++;
			span_add(list, base + i, j - i, C_REDIR);
			i = j;
			cmd_position = 0;
			continue;
		}
		if (c == '(' || c == ')' || c == '{' || c == '}') {
			size_t n = (i + 1 < len && line[i + 1] == (char)c &&
				    (c == '(' || c == ')'))
					   ? 2
					   : 1;

			span_add(list, base + i, n, C_OPERATOR);
			i += n;
			cmd_position = c == '(' || c == '{';
			expect = EX_NONE;
			continue;
		}

		/* a plain word */
		{
			size_t start = i;
			int simple = 1;

			/* an IO number glued to a redirection is part of it */
			if (isdigit(c)) {
				size_t j = i;

				while (j < len && isdigit((unsigned char)line[j]))
					j++;
				if (j < len && (line[j] == '<' || line[j] == '>')) {
					span_add(list, base + i, j - i, C_REDIR);
					i = j;
					continue;
				}
			}

			while (i < len && !is_meta((unsigned char)line[i])) {
				char ch = line[i];

				if (ch == '\'' || ch == '"' || ch == '$' || ch == '`' ||
				    ch == '\\') {
					simple = 0;
					break;
				}
				i++;
			}
			if (i == start) {
				i++; /* make progress on anything unexpected */
				continue;
			}
			{
				size_t wlen = i - start;
				char *word = xstrndup(line + start, wlen);

				if (!simple) {
					/* a word mixing quotes and text: the pieces around
					 * it are coloured on their own */
				} else if (expect == EX_VARNAME) {
					span_add(list, base + start, wlen, C_VAR);
					expect = EX_IN;
					free(word);
					continue;
				} else if (expect == EX_IN && strcmp(word, "in") == 0) {
					span_add(list, base + start, wlen, C_KEYWORD);
					expect = EX_NONE;
					cmd_position = 0;
					free(word);
					continue;
				} else if (cmd_position && is_keyword(word)) {
					span_add(list, base + start, wlen, C_KEYWORD);
					cmd_position = keyword_keeps_command_position(word);
					if (strcmp(word, "for") == 0 ||
					    strcmp(word, "select") == 0 ||
					    strcmp(word, "case") == 0)
						expect = EX_VARNAME;
					free(word);
					continue;
				} else if (cmd_position) {
					size_t eq = assignment_prefix(word, wlen);

					if (eq) {
						/* NAME=value cmd: still a command position */
						span_add(list, base + start, eq, C_VAR);
						span_add(list, base + start + eq, 1, C_OPERATOR);
						free(word);
						continue;
					}
					span_add(list, base + start, wlen,
						 highlight_command_role(word));
				} else if (word[0] == '-') {
					span_add(list, base + start, wlen, C_FLAG);
				} else if (path_exists(word)) {
					span_add(list, base + start, wlen, C_PATH_EXISTS);
				}
				free(word);
			}
			if (simple)
				cmd_position = 0;
		}
	}
}

size_t highlight_line(const char *line, size_t len, Span **out)
{
	SpanList list;

	memset(&list, 0, sizeof list);
	scan(line, len, 0, &list);
	*out = list.spans;
	return list.count;
}

char *highlight_render_range(const char *line, size_t len, size_t from, size_t to)
{
	Span *spans = NULL;
	size_t n = highlight_line(line, len, &spans);
	size_t i, at;
	Buf out;

	if (to > len)
		to = len;
	if (from > to)
		from = to;
	at = from;

	buf_init(&out);
	for (i = 0; i < n; i++) {
		size_t s_start = spans[i].start;
		size_t s_end = s_start + spans[i].len;

		if (s_end <= from || s_start >= to)
			continue;
		if (s_start < at)
			s_start = at;
		if (s_end > to)
			s_end = to;
		if (s_start > at)
			buf_put(&out, line + at, s_start - at);
		if (s_end > s_start)
			color_putn(&out, spans[i].role, line + s_start, s_end - s_start);
		at = s_end;
	}
	if (at < to)
		buf_put(&out, line + at, to - at);
	free(spans);
	return buf_take(&out);
}

char *highlight_render(const char *line, size_t len)
{
	return highlight_render_range(line, len, 0, len);
}
