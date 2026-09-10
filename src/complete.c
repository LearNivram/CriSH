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

#include "shell.h"

void complete_line(const char *line, size_t point, Vec *out, size_t *replace_from);

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
			vec_push(out, xasprintf("%s%s/", display_prefix, e->d_name));
		} else if (!dirs_only) {
			vec_push(out, xasprintf("%s%s", display_prefix, e->d_name));
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
			vec_pushs(out, b[i].name);
	{
		size_t gn;
		const GnuTool *g = gnu_table(&gn);

		if (sh.shopt.gnu_builtins)
			for (i = 0; i < gn; i++)
				if (strncmp(g[i].name, word, wlen) == 0)
					vec_pushs(out, g[i].name);
	}
	for (f = sh.funcs; f; f = f->next)
		if (strncmp(f->name, word, wlen) == 0)
			vec_pushs(out, f->name);
	for (a = sh.aliases; a; a = a->next)
		if (strncmp(a->name, word, wlen) == 0)
			vec_pushs(out, a->name);

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
				vec_pushs(out, e->d_name);
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
			vec_push(out, braced ? xasprintf("${%s}", names.v[i])
					     : xasprintf("$%s", names.v[i]));
	vec_free(&names);
}

static void dedupe(Vec *v)
{
	size_t i = 0;

	vec_sort(v);
	while (i + 1 < v->len) {
		if (strcmp(v->v[i], v->v[i + 1]) == 0)
			free(vec_remove(v, i + 1));
		else
			i++;
	}
}

void complete_line(const char *line, size_t point, Vec *out, size_t *replace_from)
{
	size_t start = word_start(line, point);
	char *word = xstrndup(line + start, point - start);

	*replace_from = start;

	if (word[0] == '$')
		complete_variable(word, out);
	else if (at_command_position(line, start) && !strchr(word, '/'))
		complete_command(word, out);
	else
		complete_path(word, out);

	dedupe(out);
	free(word);
}
