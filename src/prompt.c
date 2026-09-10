/* prompt.c - PS1 rendering, including git status without forking git.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <dirent.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

/* Walk up looking for .git, then read HEAD.  No process is spawned. */
static char *git_branch(void)
{
	char dir[4096];
	char *p;

	if (!getcwd(dir, sizeof dir))
		return NULL;
	for (;;) {
		char head[4096];
		FILE *f;
		char line[512];

		snprintf(head, sizeof head, "%s/.git/HEAD", dir);
		f = fopen(head, "r");
		if (!f) {
			struct stat st;
			snprintf(head, sizeof head, "%s/.git", dir);
			if (stat(head, &st) == 0 && S_ISREG(st.st_mode)) {
				/* a worktree: .git is a file pointing elsewhere */
				FILE *g = fopen(head, "r");
				if (g) {
					if (fgets(line, sizeof line, g) &&
					    str_prefix(line, "gitdir: ")) {
						char *nl;
						line[strcspn(line, "\n")] = '\0';
						nl = line + 8;
						snprintf(head, sizeof head, "%s/HEAD", nl);
						fclose(g);
						f = fopen(head, "r");
					} else {
						fclose(g);
					}
				}
			}
		}
		if (f) {
			char *result = NULL;

			if (fgets(line, sizeof line, f)) {
				line[strcspn(line, "\n")] = '\0';
				if (str_prefix(line, "ref: refs/heads/"))
					result = xstrdup(line + 16);
				else if (strlen(line) >= 7)
					result = xstrndup(line, 7);
			}
			fclose(f);
			return result;
		}
		p = strrchr(dir, '/');
		if (!p || p == dir)
			return NULL;
		*p = '\0';
	}
}

/* PWD with $HOME replaced by ~. */
static char *pretty_pwd(int basename_only)
{
	const char *pwd = var_get("PWD");
	const char *home = var_get("HOME");
	char cwd[4096];

	if (!pwd) {
		if (!getcwd(cwd, sizeof cwd))
			return xstrdup("?");
		pwd = cwd;
	}
	if (basename_only) {
		const char *slash = strrchr(pwd, '/');

		if (home && strcmp(pwd, home) == 0)
			return xstrdup("~");
		if (slash && slash[1])
			return xstrdup(slash + 1);
		return xstrdup(slash ? "/" : pwd);
	}
	if (home && str_prefix(pwd, home) &&
	    (pwd[strlen(home)] == '\0' || pwd[strlen(home)] == '/'))
		return xasprintf("~%s", pwd + strlen(home));
	return xstrdup(pwd);
}

char *prompt_render(const char *format)
{
	Buf out;
	const char *p;
	time_t now = time(NULL);
	struct tm tmv;

	localtime_r(&now, &tmv);
	buf_init(&out);

	for (p = format ? format : ""; *p; p++) {
		if (*p != '\\') {
			buf_putc(&out, *p);
			continue;
		}
		p++;
		switch (*p) {
		case 'u': {
			const char *u = var_get("USER");
			struct passwd *pw;

			if (!u && (pw = getpwuid(getuid())))
				u = pw->pw_name;
			buf_puts(&out, u ? u : "user");
			break;
		}
		case 'h':
		case 'H': {
			const char *h = var_get("HOSTNAME");
			char host[256];

			if (!h && gethostname(host, sizeof host) == 0)
				h = host;
			if (*p == 'h') {
				const char *dot = h ? strchr(h, '.') : NULL;
				if (dot)
					buf_put(&out, h, (size_t)(dot - h));
				else
					buf_puts(&out, h ? h : "localhost");
			} else {
				buf_puts(&out, h ? h : "localhost");
			}
			break;
		}
		case 'w':
		case 'W': {
			char *pretty = pretty_pwd(*p == 'W');
			buf_puts(&out, pretty);
			free(pretty);
			break;
		}
		case '$':
			buf_putc(&out, geteuid() == 0 ? '#' : '$');
			break;
		case 'n':
			buf_putc(&out, '\n');
			break;
		case 'r':
			buf_putc(&out, '\r');
			break;
		case 'a':
			buf_putc(&out, '\a');
			break;
		case 'e':
			buf_putc(&out, 0x1b);
			break;
		case 's':
			buf_puts(&out, "crish");
			break;
		case 'v':
			buf_puts(&out, CRISH_VERSION);
			break;
		case 'V':
			buf_puts(&out, CRISH_VERSION);
			break;
		case 'j': {
			char b[16];
			snprintf(b, sizeof b, "%d", jobs_count_running());
			buf_puts(&out, b);
			break;
		}
		case '!':
		case '#': {
			char b[16];
			snprintf(b, sizeof b, "%zu", history_count() + 1);
			buf_puts(&out, b);
			break;
		}
		case 'd': {
			char b[64];
			strftime(b, sizeof b, "%a %b %d", &tmv);
			buf_puts(&out, b);
			break;
		}
		case 't': {
			char b[32];
			strftime(b, sizeof b, "%H:%M:%S", &tmv);
			buf_puts(&out, b);
			break;
		}
		case 'T': {
			char b[32];
			strftime(b, sizeof b, "%I:%M:%S", &tmv);
			buf_puts(&out, b);
			break;
		}
		case 'A': {
			char b[32];
			strftime(b, sizeof b, "%H:%M", &tmv);
			buf_puts(&out, b);
			break;
		}
		case '@': {
			char b[32];
			strftime(b, sizeof b, "%I:%M %p", &tmv);
			buf_puts(&out, b);
			break;
		}
		case 'D': {
			/* \D{format} */
			if (p[1] == '{') {
				const char *close = strchr(p + 2, '}');
				if (close) {
					char *fmt = xstrndup(p + 2, (size_t)(close - p - 2));
					char b[256];
					strftime(b, sizeof b, fmt, &tmv);
					buf_puts(&out, b);
					free(fmt);
					p = close;
					break;
				}
			}
			break;
		}
		case 'g': { /* CriSH extension: the current git branch */
			char *branch = git_branch();
			if (branch) {
				buf_puts(&out, branch);
				free(branch);
			}
			break;
		}
		case 'G': { /* CriSH extension: " (branch)" or nothing */
			char *branch = git_branch();
			if (branch) {
				buf_printf(&out, " (%s)", branch);
				free(branch);
			}
			break;
		}
		case '?': { /* CriSH extension: the last exit status */
			char b[16];
			snprintf(b, sizeof b, "%d", sh.last_status);
			buf_puts(&out, b);
			break;
		}
		case '[':
		case ']':
			/* markers for non-printing sequences; the editor measures
			 * escape sequences itself, so they are simply dropped */
			break;
		case '\\':
			buf_putc(&out, '\\');
			break;
		case '0': {
			int v = 0, n = 0;
			while (n < 3 && p[1] >= '0' && p[1] <= '7') {
				v = v * 8 + (p[1] - '0');
				p++;
				n++;
			}
			buf_putc(&out, v);
			break;
		}
		case '\0':
			buf_putc(&out, '\\');
			p--;
			break;
		default:
			buf_putc(&out, '\\');
			buf_putc(&out, *p);
			break;
		}
	}

	/* command substitution and variables inside the prompt */
	{
		char *raw = buf_take(&out);
		char *expanded = expand_to_string(raw);

		free(raw);
		return expanded;
	}
}
