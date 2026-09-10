/* main.c - startup, option parsing and the interactive loop.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shell.h"

Shell sh;

/* ------------------------------------------------------------------ errors */

void shell_error(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fputs("crish: ", stderr);
	if (sh.script_name && !sh.interactive)
		fprintf(stderr, "%s: line %d: ", sh.script_name, sh.lineno);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void shell_error_at(const char *where, const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fprintf(stderr, "crish: %s: ", where);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void shell_fatal(const char *fmt, ...)
{
	va_list ap;

	fflush(stdout);
	fputs("crish: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

/* -------------------------------------------------------------------- init */

static void defaults(void)
{
	sh.opt.hashall = 1;
	sh.opt.emacs = 1;
	sh.opt.history = 1;
	sh.shopt.expand_aliases = 1;
	sh.shopt.gnu_builtins = 1;
	sh.shopt.sourcepath = 1;
	sh.shopt.progcomp = 1;
	sh.shopt.interactive_comments = 1;
	sh.shopt.checkwinsize = 1;
	sh.shopt.cmdhist = 1;
	sh.shopt.extglob = 1;
	sh.last_status = 0;
}

void shell_init(int argc, char **argv, char **envp)
{
	(void)argc;
	sh.pid = getpid();
	sh.argv0 = argv[0];
	vec_init(&sh.dirstack);
	defaults();
	vars_init(envp);
	jobs_init();
}

static void usage(FILE *out)
{
	fprintf(out,
		"CriSH %s - runs Linux shell scripts on a stock Mac.\n"
		"\n"
		"usage: crish [options] [script [args]]\n"
		"       crish -c 'command' [name [args]]\n"
		"\n"
		"  -c cmd        run cmd and exit\n"
		"  -s            read commands from standard input\n"
		"  -i            force interactive behaviour\n"
		"  -l, --login   act as a login shell\n"
		"  -e -u -x -f   the usual set options\n"
		"  -o name       set a long option (errexit, pipefail, ...)\n"
		"  --norc        do not read ~/.crishrc\n"
		"  --posix       stay closer to POSIX, drop the GNU builtins\n"
		"  --version     print the version\n"
		"  --help        print this message\n"
		"\n"
		"`crish update' installs the newest release, `help' lists the builtins.\n",
		CRISH_VERSION);
}

/* ------------------------------------------------------------ interactive */

static void on_sigint(int sig)
{
	(void)sig;
	trap_pending = 1;
}

static void interactive_loop(void)
{
	Buf pending;
	const char *ps1, *ps2;

	line_init();
	history_init();
	signal(SIGINT, on_sigint);
	signal(SIGQUIT, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTSTP, SIG_IGN);

	buf_init(&pending);
	for (;;) {
		char *line;
		char *prompt;
		char *prompt2;

		jobs_notify();
		ps1 = var_get("PS1");
		ps2 = var_get("PS2");
		prompt = prompt_render(pending.len ? (ps2 ? ps2 : "> ") : (ps1 ? ps1 : "$ "));
		prompt2 = prompt_render(ps2 ? ps2 : "> ");

		line = line_read(prompt, prompt2);
		free(prompt);
		free(prompt2);

		if (!line) {
			if (sh.opt.ignoreeof) {
				fprintf(stderr, "Use \"exit\" to leave the shell.\n");
				continue;
			}
			fputc('\n', stdout);
			break;
		}
		if (pending.len)
			buf_putc(&pending, '\n');
		buf_puts(&pending, line);
		free(line);

		{
			Parser *p = parser_new(pending.b ? pending.b : "", "crish");
			char *err = NULL;
			Node *n = parser_next(p, &err);
			int incomplete = parser_incomplete(p);

			if (err && incomplete) {
				/* keep reading: the construct is not finished */
				free(err);
				parser_free(p);
				node_free(n);
				continue;
			}
			parser_free(p);
			node_free(n);
			free(err);
		}

		if (pending.len) {
			history_add(pending.b);
			exec_string(pending.b, "crish");
			buf_reset(&pending);
		}
		trap_run_pending();
		if (sh.exit_requested)
			break;
	}
	buf_free(&pending);
	history_save();
	line_cleanup();
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv, char **envp)
{
	int i = 1;
	const char *command = NULL;
	const char *script = NULL;
	int force_interactive = 0, read_stdin = 0, norc = 0;
	Vec args;

	shell_init(argc, argv, envp);

	if (argv[0] && argv[0][0] == '-')
		sh.login = 1;

	for (; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(a, "--version") == 0) {
			printf("CriSH %s\n", CRISH_VERSION);
			return 0;
		}
		if (strcmp(a, "--help") == 0) {
			usage(stdout);
			return 0;
		}
		if (strcmp(a, "--login") == 0) {
			sh.login = 1;
			continue;
		}
		if (strcmp(a, "--norc") == 0) {
			norc = 1;
			continue;
		}
		if (strcmp(a, "--posix") == 0) {
			sh.opt.posix = 1;
			sh.shopt.gnu_builtins = 0;
			continue;
		}
		if (strcmp(a, "-c") == 0) {
			if (i + 1 >= argc)
				shell_fatal("-c: option requires an argument");
			command = argv[++i];
			continue;
		}
		if (strcmp(a, "-o") == 0 && i + 1 < argc) {
			char *fake[3];
			fake[0] = (char *)"set";
			fake[1] = (char *)"-o";
			fake[2] = argv[++i];
			builtin_find("set")->fn(3, fake);
			continue;
		}
		if (a[0] == '-' && a[1] && a[1] != '-') {
			const char *p;
			int handled = 1;

			for (p = a + 1; *p; p++) {
				switch (*p) {
				case 'c':
					if (i + 1 >= argc)
						shell_fatal("-c: option requires an argument");
					command = argv[++i];
					break;
				case 's': read_stdin = 1; break;
				case 'i': force_interactive = 1; break;
				case 'l': sh.login = 1; break;
				case 'e': sh.opt.errexit = 1; break;
				case 'u': sh.opt.nounset = 1; break;
				case 'x': sh.opt.xtrace = 1; break;
				case 'v': sh.opt.verbose = 1; break;
				case 'f': sh.opt.noglob = 1; break;
				case 'n': sh.opt.noexec = 1; break;
				case 'm': sh.opt.monitor = 1; break;
				case 'C': sh.opt.noclobber = 1; break;
				case 'a': sh.opt.allexport = 1; break;
				case 'h': sh.opt.hashall = 1; break;
				default:
					handled = 0;
					break;
				}
				if (!handled)
					break;
			}
			if (handled)
				continue;
			fprintf(stderr, "crish: %s: invalid option\n", a);
			usage(stderr);
			return 2;
		}
		break;
	}

	if (!command && !read_stdin && i < argc)
		script = argv[i++];

	sh.interactive = force_interactive ||
			 (!command && !script && isatty(0) && isatty(2));
	sh.reading_stdin = read_stdin || (!command && !script);
	jobs_init();

	vec_init(&args);
	for (; i < argc; i++)
		vec_pushs(&args, argv[i]);

	if (command) {
		sh.script_name = args.len ? args.v[0] : "crish";
		if (args.len)
			free(vec_remove(&args, 0));
		args_set(&args);
	} else if (script) {
		sh.script_name = script;
		args_set(&args);
	} else {
		sh.script_name = "crish";
		args_set(&args);
	}
	vec_free(&args);

	if (!norc && (sh.interactive || sh.login))
		config_load();

	if (command) {
		exec_string(command, "-c");
	} else if (script) {
		Vec none;
		vec_init(&none);
		exec_file(script, NULL);
		vec_free(&none);
	} else if (sh.interactive) {
		interactive_loop();
	} else {
		Buf src;
		char chunk[8192];
		size_t n;

		buf_init(&src);
		while ((n = fread(chunk, 1, sizeof chunk, stdin)) > 0)
			buf_put(&src, chunk, n);
		exec_string(src.b ? src.b : "", "stdin");
		buf_free(&src);
	}

	trap_run_exit();
	fflush(NULL);
	return sh.exit_requested ? sh.exit_code : sh.last_status;
}
