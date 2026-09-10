/* history.c - the command history and its file.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

static Vec entries;
static size_t loaded;
static char *hist_path;
static size_t max_entries = 1000;

static char *history_file(void)
{
	const char *env = var_get("HISTFILE");
	const char *home;

	if (env && *env)
		return xstrdup(env);
	home = var_get("HOME");
	if (!home)
		return NULL;
	return xasprintf("%s/.crish_history", home);
}

void history_init(void)
{
	FILE *f;
	char line[4096];
	const char *size;

	vec_init(&entries);
	size = var_get("HISTSIZE");
	if (size && *size) {
		long n = strtol(size, NULL, 10);
		if (n > 0)
			max_entries = (size_t)n;
	}
	free(hist_path);
	hist_path = history_file();
	if (!hist_path)
		return;
	f = fopen(hist_path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\n")] = '\0';
		if (*line)
			vec_pushs(&entries, line);
	}
	fclose(f);
	loaded = entries.len;
}

void history_add(const char *line)
{
	const char *ctrl;

	if (!line || !*line)
		return;
	while (*line == ' ' || *line == '\t')
		line++;
	if (!*line)
		return;

	ctrl = var_get("HISTCONTROL");
	if (ctrl) {
		if (strstr(ctrl, "ignoredups") && entries.len &&
		    strcmp(entries.v[entries.len - 1], line) == 0)
			return;
		if (strstr(ctrl, "ignorespace") && line[0] == ' ')
			return;
	} else if (entries.len && strcmp(entries.v[entries.len - 1], line) == 0) {
		return;
	}
	vec_pushs(&entries, line);
	while (entries.len > max_entries)
		free(vec_remove(&entries, 0));
}

size_t history_count(void)
{
	return entries.len;
}

const char *history_get(size_t i)
{
	return i < entries.len ? entries.v[i] : "";
}

void history_clear(void)
{
	vec_clear(&entries);
	loaded = 0;
}

void history_save(void)
{
	FILE *f;
	size_t i;

	if (!hist_path || !sh.opt.history)
		return;
	if (sh.shopt.histappend && loaded <= entries.len) {
		f = fopen(hist_path, "a");
		if (!f)
			return;
		for (i = loaded; i < entries.len; i++)
			fprintf(f, "%s\n", entries.v[i]);
		fclose(f);
		return;
	}
	f = fopen(hist_path, "w");
	if (!f)
		return;
	for (i = 0; i < entries.len; i++)
		fprintf(f, "%s\n", entries.v[i]);
	fclose(f);
}
