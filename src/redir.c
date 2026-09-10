/* redir.c - applying and undoing redirections.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

typedef struct Save {
	int fd;
	int saved; /* -1 means the fd was not open before */
	struct Save *next;
} Save;

struct SavedFds {
	Save *list;
};

/* The built-in utilities run in this process and share stdin/stdout/stderr,
 * so a FILE's buffer and EOF flag outlive a redirection of its descriptor.
 * Every dup2 onto 0, 1 or 2 has to resynchronise the matching stream. */
void redir_sync_std(int fd)
{
	switch (fd) {
	case 0:
		fpurge(stdin);
		clearerr(stdin);
		break;
	case 1:
		fflush(stdout);
		clearerr(stdout);
		break;
	case 2:
		fflush(stderr);
		clearerr(stderr);
		break;
	default:
		break;
	}
}

static void save_fd(SavedFds *s, int fd)
{
	Save *e = xmalloc(sizeof *e);

	e->fd = fd;
	redir_sync_std(fd);
	e->saved = fcntl(fd, F_DUPFD_CLOEXEC, 10);
	e->next = s->list;
	s->list = e;
}

/* Write text to an anonymous temp file and return a descriptor at offset 0. */
static int heredoc_fd(const char *text)
{
	char path[] = "/tmp/crish-here-XXXXXX";
	int fd = mkstemp(path);
	size_t len = strlen(text);
	ssize_t n;

	if (fd < 0) {
		shell_error("cannot create here-document: %s", strerror(errno));
		return -1;
	}
	unlink(path);
	while (len) {
		n = write(fd, text, len);
		if (n <= 0)
			break;
		text += n;
		len -= (size_t)n;
	}
	lseek(fd, 0, SEEK_SET);
	return fd;
}

static int open_target(Redir *r, const char *file, int *want_fd)
{
	int flags = 0, fd;

	switch (r->type) {
	case R_IN:
		flags = O_RDONLY;
		*want_fd = r->fd >= 0 ? r->fd : 0;
		break;
	case R_OUT:
	case R_ALL_OUT:
		if (sh.opt.noclobber && r->type == R_OUT)
			flags = O_WRONLY | O_CREAT | O_EXCL;
		else
			flags = O_WRONLY | O_CREAT | O_TRUNC;
		*want_fd = r->fd >= 0 ? r->fd : 1;
		break;
	case R_CLOBBER:
		flags = O_WRONLY | O_CREAT | O_TRUNC;
		*want_fd = r->fd >= 0 ? r->fd : 1;
		break;
	case R_APPEND:
	case R_ALL_APPEND:
		flags = O_WRONLY | O_CREAT | O_APPEND;
		*want_fd = r->fd >= 0 ? r->fd : 1;
		break;
	case R_RDWR:
		flags = O_RDWR | O_CREAT;
		*want_fd = r->fd >= 0 ? r->fd : 0;
		break;
	default:
		return -1;
	}
	fd = open(file, flags, 0666);
	if (fd < 0)
		shell_error("%s: %s", file, strerror(errno));
	return fd;
}

SavedFds *redir_apply(Redir *r, int *ok)
{
	SavedFds *s = xcalloc(1, sizeof *s);

	*ok = 1;
	for (; r; r = r->next) {
		char *target;
		int want = -1;

		if (r->type == R_HEREDOC) {
			char *body = r->here_quoted ? xstrdup(r->word)
						    : expand_to_string(r->word);
			int fd = heredoc_fd(body);

			free(body);
			if (fd < 0) {
				*ok = 0;
				return s;
			}
			want = r->fd >= 0 ? r->fd : 0;
			save_fd(s, want);
			dup2(fd, want);
			redir_sync_std(want);
			close(fd);
			continue;
		}

		if (r->type == R_HERESTR) {
			char *val = expand_unsplit(r->word);
			char *text = xasprintf("%s\n", val);
			int fd = heredoc_fd(text);

			free(val);
			free(text);
			if (fd < 0) {
				*ok = 0;
				return s;
			}
			want = r->fd >= 0 ? r->fd : 0;
			save_fd(s, want);
			dup2(fd, want);
			redir_sync_std(want);
			close(fd);
			continue;
		}

		target = expand_unsplit(r->word);

		if (r->type == R_DUP_IN || r->type == R_DUP_OUT) {
			want = r->fd >= 0 ? r->fd : (r->type == R_DUP_IN ? 0 : 1);
			if (strcmp(target, "-") == 0) {
				save_fd(s, want);
				close(want);
				free(target);
				continue;
			}
			{
				char *end;
				long n = strtol(target, &end, 10);

				if (*end == '\0' && end != target) {
					if (fcntl((int)n, F_GETFD) < 0) {
						shell_error("%ld: bad file descriptor", n);
						*ok = 0;
						free(target);
						return s;
					}
					save_fd(s, want);
					dup2((int)n, want);
					redir_sync_std(want);
				} else {
					/* >&file behaves like >file */
					int fd = open(target,
						      r->type == R_DUP_OUT
							      ? (O_WRONLY | O_CREAT | O_TRUNC)
							      : O_RDONLY,
						      0666);
					if (fd < 0) {
						shell_error("%s: %s", target, strerror(errno));
						*ok = 0;
						free(target);
						return s;
					}
					save_fd(s, want);
					dup2(fd, want);
					redir_sync_std(want);
					close(fd);
				}
			}
			free(target);
			continue;
		}

		{
			int fd = open_target(r, target, &want);

			if (fd < 0) {
				*ok = 0;
				free(target);
				return s;
			}
			if (r->varname) {
				int assigned = fcntl(fd, F_DUPFD_CLOEXEC, 10);
				char buf[16];

				close(fd);
				snprintf(buf, sizeof buf, "%d", assigned);
				var_set(r->varname, buf, 0);
				free(target);
				continue;
			}
			save_fd(s, want);
			dup2(fd, want);
			redir_sync_std(want);
			close(fd);
			if (r->type == R_ALL_OUT || r->type == R_ALL_APPEND) {
				save_fd(s, 2);
				dup2(want, 2);
				redir_sync_std(2);
			}
		}
		free(target);
	}
	return s;
}

void redir_restore(SavedFds *s)
{
	Save *e;

	if (!s)
		return;
	for (e = s->list; e; e = e->next) {
		redir_sync_std(e->fd);
		if (e->saved >= 0) {
			dup2(e->saved, e->fd);
			close(e->saved);
		} else {
			close(e->fd);
		}
		redir_sync_std(e->fd);
	}
	while (s->list) {
		Save *next = s->list->next;
		free(s->list);
		s->list = next;
	}
	free(s);
}
