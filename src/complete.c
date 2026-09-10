/* complete.c - tab completion for commands, paths and variables.
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

#include "color.h"
#include "shell.h"

/* Each match carries its kind after a \001 separator until the very end, so
 * that name and kind sort and de-duplicate together.  The separator sorts
 * below every printable byte, which keeps identical names adjacent even when
 * one name is a prefix of another. */
#define KIND_DIR     'd'
#define KIND_EXEC    'x'
#define KIND_LINK    'l'
#define KIND_FILE    '-'
#define KIND_BUILTIN 'b'
#define KIND_GNU     'g'
#define KIND_FUNC    'f'
#define KIND_ALIAS   'a'
#define KIND_VAR     'v'

void complete_line(const char *line, size_t point, Vec *out, Vec *kinds,
		   size_t *replace_from);

static void push_kind(Vec *v, char *name, char kind)
{
	size_t n = strlen(name);
	char *tagged = xmalloc(n + 3);

	memcpy(tagged, name, n);
	tagged[n] = '\001';
	tagged[n + 1] = kind;
	tagged[n + 2] = '\0';
	free(name);
	vec_push(v, tagged);
}

static void push_kind_s(Vec *v, const char *name, char kind)
{
	push_kind(v, xstrdup(name), kind);
}

/* Where does the word under the cursor start? */
static size_t word_start(const char *line, size_t point)
{
	size_t i = point;

	while (i > 0) {
		char c = line[i - 1];

		if (c == ' ' || c == '\t' || c == '|' || c == '&' || c == ';' || c == '(' ||
		    c == ')' || c == '<' || c == '>')
			break;
		i--;
	}
	return i;
}

/* Is the word being completed the first of its command? */
static int at_command_position(const char *line, size_t start)
{
	size_t i = start;

	while (i > 0) {
		char c = line[i - 1];

		if (c == ' ' || c == '\t') {
			i--;
			continue;
		}
		return c == '|' || c == '&' || c == ';' || c == '(';
	}
	return 1;
}

static void add_dir_entries(Vec *out, const char *dirpath, const char *prefix,
			    const char *display_prefix, int dirs_only)
{
	DIR *d = opendir(*dirpath ? dirpath : ".");
	struct dirent *e;
	size_t plen = strlen(prefix);

	if (!d)
		return;
	while ((e = readdir(d))) {
		char *full;
		struct stat st;

		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (strncmp(e->d_name, prefix, plen) != 0)
			continue;
		if (prefix[0] != '.' && e->d_name[0] == '.')
			continue;
		full = xasprintf("%s%s", dirpath, e->d_name);
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
			push_kind(out, xasprintf("%s%s/", display_prefix, e->d_name),
				  KIND_DIR);
		} else if (!dirs_only) {
			char kind = KIND_FILE;
			struct stat lst;

			if (lstat(full, &lst) == 0 && S_ISLNK(lst.st_mode))
				kind = KIND_LINK;
			else if (access(full, X_OK) == 0)
				kind = KIND_EXEC;
			push_kind(out, xasprintf("%s%s", display_prefix, e->d_name), kind);
		}
		free(full);
	}
	closedir(d);
}

static void complete_path(const char *word, Vec *out)
{
	const char *slash = strrchr(word, '/');
	char *dirpath;
	const char *base;
	char *display;

	if (slash) {
		dirpath = xstrndup(word, (size_t)(slash - word + 1));
		base = slash + 1;
		display = xstrdup(dirpath);
	} else {
		dirpath = xstrdup("");
		base = word;
		display = xstrdup("");
	}
	if (dirpath[0] == '~') {
		char *expanded = expand_to_string(dirpath);
		char *tilded = xasprintf("%s", expanded);
		free(dirpath);
		dirpath = tilded;
		free(expanded);
	}
	add_dir_entries(out, dirpath, base, display, 0);
	free(dirpath);
	free(display);
}

static void complete_command(const char *word, Vec *out)
{
	size_t n, i;
	const Builtin *b = builtin_table(&n);
	const char *path;
	char *copy, *dir, *save = NULL;
	size_t wlen = strlen(word);
	Func *f;
	Alias *a;

	for (i = 0; i < n; i++)
		if (strncmp(b[i].name, word, wlen) == 0)
			push_kind_s(out, b[i].name, KIND_BUILTIN);
	{
		size_t gn;
		const GnuTool *g = gnu_table(&gn);

		if (sh.shopt.gnu_builtins)
			for (i = 0; i < gn; i++)
				if (strncmp(g[i].name, word, wlen) == 0)
					push_kind_s(out, g[i].name, KIND_GNU);
	}
	for (f = sh.funcs; f; f = f->next)
		if (strncmp(f->name, word, wlen) == 0)
			push_kind_s(out, f->name, KIND_FUNC);
	for (a = sh.aliases; a; a = a->next)
		if (strncmp(a->name, word, wlen) == 0)
			push_kind_s(out, a->name, KIND_ALIAS);

	path = var_get("PATH");
	if (!path)
		return;
	copy = xstrdup(path);
	for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		DIR *d = opendir(*dir ? dir : ".");
		struct dirent *e;

		if (!d)
			continue;
		while ((e = readdir(d))) {
			char *full;

			if (strncmp(e->d_name, word, wlen) != 0)
				continue;
			full = xasprintf("%s/%s", dir, e->d_name);
			if (access(full, X_OK) == 0)
				push_kind_s(out, e->d_name, KIND_EXEC);
			free(full);
		}
		closedir(d);
	}
	free(copy);
}

static void complete_variable(const char *word, Vec *out)
{
	Vec names;
	size_t i;
	const char *prefix = word + 1;
	int braced = prefix[0] == '{';

	if (braced)
		prefix++;
	vec_init(&names);
	vars_all(&names);
	for (i = 0; i < names.len; i++)
		if (str_prefix(names.v[i], prefix))
			push_kind(out, braced ? xasprintf("${%s}", names.v[i])
					      : xasprintf("$%s", names.v[i]),
				  KIND_VAR);
	vec_free(&names);
}

/* Names still carry their kind, so compare only up to the separator. */
static int same_name(const char *a, const char *b)
{
	const char *sa = strchr(a, '\001');
	const char *sb = strchr(b, '\001');
	size_t la = sa ? (size_t)(sa - a) : strlen(a);
	size_t lb = sb ? (size_t)(sb - b) : strlen(b);

	return la == lb && memcmp(a, b, la) == 0;
}

static void dedupe(Vec *v)
{
	size_t i = 0;

	vec_sort(v);
	while (i + 1 < v->len) {
		if (same_name(v->v[i], v->v[i + 1]))
			free(vec_remove(v, i + 1));
		else
			i++;
	}
}

void complete_line(const char *line, size_t point, Vec *out, Vec *kinds,
		   size_t *replace_from)
{
	size_t start = word_start(line, point);
	char *word = xstrndup(line + start, point - start);
	Vec tagged;
	size_t i;

	*replace_from = start;
	vec_init(&tagged);

	if (word[0] == '$')
		complete_variable(word, &tagged);
	else if (at_command_position(line, start) && !strchr(word, '/'))
		complete_command(word, &tagged);
	else
		complete_path(word, &tagged);

	dedupe(&tagged);

	/* split the kind back off */
	for (i = 0; i < tagged.len; i++) {
		char *sep = strchr(tagged.v[i], '\001');

		if (!sep)
			continue;
		if (kinds) {
			char k[2];

			k[0] = sep[1];
			k[1] = '\0';
			vec_pushs(kinds, k);
		}
		vec_push(out, xstrndup(tagged.v[i], (size_t)(sep - tagged.v[i])));
	}
	vec_free(&tagged);
	free(word);
}

/* The colour a completion kind is shown in. */
ColorRole complete_kind_color(char kind);
ColorRole complete_kind_color(char kind)
{
	switch (kind) {
	case KIND_DIR: return C_DIR;
	case KIND_EXEC: return C_EXEC;
	case KIND_LINK: return C_LINK;
	case KIND_BUILTIN: return C_BUILTIN;
	case KIND_GNU: return C_GNU;
	case KIND_FUNC: return C_FUNCTION;
	case KIND_ALIAS: return C_ALIAS;
	case KIND_VAR: return C_VAR;
	default: return C_FILE;
	}
}
