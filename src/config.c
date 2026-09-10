/* config.c - startup files.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

static void source_if_readable(const char *path)
{
	if (access(path, R_OK) != 0)
		return;
	exec_file(path, NULL);
}

void config_load(void)
{
	const char *home = var_get("HOME");
	const char *env;

	if (sh.login) {
		source_if_readable("/etc/crish/crishrc");
		if (home) {
			char *p = xasprintf("%s/.crish_profile", home);
			source_if_readable(p);
			free(p);
		}
	}

	env = var_get("CRISHRC");
	if (env && *env) {
		source_if_readable(env);
		return;
	}
	if (!home)
		return;
	{
		char *p = xasprintf("%s/.crishrc", home);
		source_if_readable(p);
		free(p);
	}
}
