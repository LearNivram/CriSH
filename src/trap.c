/* trap.c - signal handlers and the trap builtin's backing store.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shell.h"

#define NSIG_MAX 64
#define SIG_EXIT 0

static char *actions[NSIG_MAX];
static volatile sig_atomic_t caught[NSIG_MAX];
volatile sig_atomic_t trap_pending;

static const struct {
	int sig;
	const char *name;
} signames[] = {
	{ SIGHUP, "HUP" },   { SIGINT, "INT" },	 { SIGQUIT, "QUIT" }, { SIGILL, "ILL" },
	{ SIGTRAP, "TRAP" }, { SIGABRT, "ABRT" }, { SIGFPE, "FPE" },   { SIGKILL, "KILL" },
	{ SIGBUS, "BUS" },   { SIGSEGV, "SEGV" }, { SIGSYS, "SYS" },   { SIGPIPE, "PIPE" },
	{ SIGALRM, "ALRM" }, { SIGTERM, "TERM" }, { SIGURG, "URG" },   { SIGSTOP, "STOP" },
	{ SIGTSTP, "TSTP" }, { SIGCONT, "CONT" }, { SIGCHLD, "CHLD" }, { SIGTTIN, "TTIN" },
	{ SIGTTOU, "TTOU" }, { SIGXCPU, "XCPU" }, { SIGXFSZ, "XFSZ" }, { SIGVTALRM, "VTALRM" },
	{ SIGPROF, "PROF" }, { SIGWINCH, "WINCH" }, { SIGUSR1, "USR1" }, { SIGUSR2, "USR2" },
	{ 0, NULL }
};

int trap_signal_number(const char *name)
{
	int i;
	char *end;
	long n;

	if (!name || !*name)
		return -1;
	if (strcmp(name, "EXIT") == 0 || strcmp(name, "0") == 0)
		return SIG_EXIT;
	if (strcmp(name, "ERR") == 0)
		return NSIG_MAX - 1;
	if (strcmp(name, "DEBUG") == 0)
		return NSIG_MAX - 2;
	if (strcmp(name, "RETURN") == 0)
		return NSIG_MAX - 3;
	n = strtol(name, &end, 10);
	if (*end == '\0' && end != name && n > 0 && n < NSIG_MAX)
		return (int)n;
	if (str_prefix(name, "SIG"))
		name += 3;
	for (i = 0; signames[i].name; i++)
		if (str_casecmp(signames[i].name, name) == 0)
			return signames[i].sig;
	return -1;
}

const char *trap_signal_name(int sig)
{
	int i;

	if (sig == SIG_EXIT)
		return "EXIT";
	if (sig == NSIG_MAX - 1)
		return "ERR";
	if (sig == NSIG_MAX - 2)
		return "DEBUG";
	if (sig == NSIG_MAX - 3)
		return "RETURN";
	for (i = 0; signames[i].name; i++)
		if (signames[i].sig == sig)
			return signames[i].name;
	return "UNKNOWN";
}

static void handler(int sig)
{
	if (sig >= 0 && sig < NSIG_MAX) {
		caught[sig] = 1;
		trap_pending = 1;
	}
}

void trap_set(int sig, const char *action)
{
	if (sig < 0 || sig >= NSIG_MAX)
		return;
	free(actions[sig]);
	actions[sig] = action ? xstrdup(action) : NULL;

	if (sig <= 0 || sig >= NSIG_MAX - 3)
		return;
	if (!action) {
		signal(sig, SIG_DFL);
	} else if (!*action) {
		signal(sig, SIG_IGN);
	} else {
		struct sigaction sa;

		memset(&sa, 0, sizeof sa);
		sa.sa_handler = handler;
		sigemptyset(&sa.sa_mask);
		sa.sa_flags = SA_RESTART;
		sigaction(sig, &sa, NULL);
	}
}

const char *trap_get(int sig)
{
	if (sig < 0 || sig >= NSIG_MAX)
		return NULL;
	return actions[sig];
}

void trap_run_pending(void)
{
	int i;

	if (!trap_pending)
		return;
	trap_pending = 0;
	for (i = 1; i < NSIG_MAX - 3; i++) {
		if (!caught[i])
			continue;
		caught[i] = 0;
		if (actions[i] && *actions[i]) {
			int saved = sh.last_status;
			exec_string(actions[i], "trap");
			sh.last_status = saved;
		}
	}
}

void trap_run_exit(void)
{
	if (actions[SIG_EXIT] && *actions[SIG_EXIT]) {
		char *action = actions[SIG_EXIT];
		int saved = sh.last_status;

		actions[SIG_EXIT] = NULL;
		sh.exit_requested = 0;
		exec_string(action, "trap EXIT");
		sh.last_status = saved;
		free(action);
	}
}

void traps_reset_in_child(void)
{
	int i;

	for (i = 0; i < NSIG_MAX; i++) {
		if (actions[i]) {
			free(actions[i]);
			actions[i] = NULL;
		}
		caught[i] = 0;
		if (i > 0 && i < NSIG_MAX - 3 && i != SIGKILL && i != SIGSTOP)
			signal(i, SIG_DFL);
	}
	trap_pending = 0;
}
