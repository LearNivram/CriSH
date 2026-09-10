/* jobs.c - background jobs and the wait/jobs/fg/bg builtins' backing store.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "shell.h"

enum { J_RUNNING, J_STOPPED, J_DONE };

typedef struct Job {
	int id;
	pid_t pgid;
	char *cmd;
	int state;
	int status;
	int notified;
	int background;
	struct Job *next;
} Job;

static Job *jobs;
static int next_id = 1;
static pid_t shell_pgid;
static int job_control_ok;

void jobs_init(void)
{
	shell_pgid = getpgrp();
	job_control_ok = sh.interactive && isatty(0);
}

static Job *job_find_pgid(pid_t pgid)
{
	Job *j;

	for (j = jobs; j; j = j->next)
		if (j->pgid == pgid)
			return j;
	return NULL;
}

static Job *job_find_id(int id)
{
	Job *j;

	for (j = jobs; j; j = j->next)
		if (j->id == id)
			return j;
	return NULL;
}

static Job *job_current(void)
{
	Job *j, *best = NULL;

	for (j = jobs; j; j = j->next)
		if (j->state != J_DONE && (!best || j->id > best->id))
			best = j;
	return best;
}

int job_add(pid_t pgid, const char *cmd, int background)
{
	Job *j = xcalloc(1, sizeof *j);
	Job **tail = &jobs;

	j->id = next_id++;
	j->pgid = pgid;
	j->cmd = xstrdup(cmd ? cmd : "");
	j->state = J_RUNNING;
	j->background = background;
	while (*tail)
		tail = &(*tail)->next;
	*tail = j;
	if (background)
		fprintf(stderr, "[%d] %ld\n", j->id, (long)pgid);
	return j->id;
}

static void job_remove_done(void)
{
	Job **p = &jobs;

	while (*p) {
		if ((*p)->state == J_DONE && (*p)->notified) {
			Job *j = *p;
			*p = j->next;
			free(j->cmd);
			free(j);
			continue;
		}
		p = &(*p)->next;
	}
	if (!jobs)
		next_id = 1;
}

void jobs_reap(void)
{
	for (;;) {
		int status = 0;
		pid_t pid = waitpid(-1, &status, WNOHANG | WUNTRACED);
		Job *j;

		if (pid <= 0)
			break;
		j = job_find_pgid(pid);
		if (!j)
			continue;
		if (WIFSTOPPED(status)) {
			j->state = J_STOPPED;
			j->notified = 0;
		} else {
			j->state = J_DONE;
			j->status = WIFEXITED(status) ? WEXITSTATUS(status)
						      : 128 + WTERMSIG(status);
			j->notified = 0;
		}
	}
}

void jobs_notify(void)
{
	Job *j;

	jobs_reap();
	for (j = jobs; j; j = j->next) {
		if (j->notified)
			continue;
		if (j->state == J_DONE) {
			fprintf(stderr, "[%d]%s Done                    %s\n", j->id,
				j == job_current() ? "+" : " ", j->cmd);
			j->notified = 1;
		} else if (j->state == J_STOPPED) {
			fprintf(stderr, "[%d]%s Stopped                 %s\n", j->id,
				j == job_current() ? "+" : " ", j->cmd);
			j->notified = 1;
		}
	}
	job_remove_done();
}

int jobs_count_running(void)
{
	Job *j;
	int n = 0;

	for (j = jobs; j; j = j->next)
		if (j->state != J_DONE)
			n++;
	return n;
}

void jobs_list(int mode)
{
	Job *j;
	Job *cur = job_current();

	jobs_reap();
	for (j = jobs; j; j = j->next) {
		const char *state = j->state == J_RUNNING  ? "Running"
				    : j->state == J_STOPPED ? "Stopped"
							    : "Done";

		if (mode == 'p') {
			printf("%ld\n", (long)j->pgid);
			continue;
		}
		if (mode == 'l')
			printf("[%d]%s %ld %-23s %s\n", j->id, j == cur ? "+" : " ",
			       (long)j->pgid, state, j->cmd);
		else
			printf("[%d]%s %-23s %s\n", j->id, j == cur ? "+" : " ", state,
			       j->cmd);
		j->notified = 1;
	}
	job_remove_done();
}

/* Blocking wait on one child that is not a tracked background job. */
int job_wait(pid_t pid, int *status)
{
	int st = 0;
	pid_t r;

	do {
		r = waitpid(pid, &st, WUNTRACED);
	} while (r < 0 && errno == EINTR);

	if (status)
		*status = st;
	if (WIFSTOPPED(st)) {
		Job *j = job_find_pgid(pid);
		if (!j) {
			job_add(pid, "stopped job", 0);
			j = job_find_pgid(pid);
		}
		if (j) {
			j->state = J_STOPPED;
			j->notified = 0;
		}
		fprintf(stderr, "\n[%d]+ Stopped\n", j ? j->id : 0);
	}
	return r;
}

static Job *resolve_spec(const char *spec)
{
	if (!spec || !*spec)
		return job_current();
	if (spec[0] == '%')
		spec++;
	if (!*spec || strcmp(spec, "%") == 0 || strcmp(spec, "+") == 0)
		return job_current();
	if (isdigit((unsigned char)spec[0]))
		return job_find_id(atoi(spec));
	{
		Job *j;
		for (j = jobs; j; j = j->next)
			if (str_prefix(j->cmd, spec))
				return j;
	}
	return NULL;
}

int job_fg(const char *spec)
{
	Job *j = resolve_spec(spec);
	int status = 0;

	if (!j) {
		shell_error("fg: %s: no such job", spec ? spec : "current");
		return 1;
	}
	fprintf(stderr, "%s\n", j->cmd);
	if (job_control_ok)
		tcsetpgrp(0, j->pgid);
	kill(-j->pgid, SIGCONT);
	j->state = J_RUNNING;
	job_wait(j->pgid, &status);
	if (job_control_ok)
		tcsetpgrp(0, shell_pgid);
	if (WIFEXITED(status) || WIFSIGNALED(status)) {
		j->state = J_DONE;
		j->notified = 1;
		job_remove_done();
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

int job_bg(const char *spec)
{
	Job *j = resolve_spec(spec);

	if (!j) {
		shell_error("bg: %s: no such job", spec ? spec : "current");
		return 1;
	}
	kill(-j->pgid, SIGCONT);
	j->state = J_RUNNING;
	fprintf(stderr, "[%d]+ %s &\n", j->id, j->cmd);
	return 0;
}
