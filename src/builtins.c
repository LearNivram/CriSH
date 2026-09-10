/* builtins.c - the shell's own commands.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/times.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "color.h"
#include "shell.h"

int builtin_test(int argc, char **argv);

/* ------------------------------------------------------------------ helpers */

static int arg_error(const char *cmd, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "crish: %s: ", cmd);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	return 2;
}

/* ------------------------------------------------------------------- basics */

static int b_true(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

static int b_false(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 1;
}

static int b_echo(int argc, char **argv)
{
	int i = 1, newline = 1, escapes = sh.shopt.xpg_echo;
	Buf out;

	while (i < argc && argv[i][0] == '-' && argv[i][1]) {
		const char *p = argv[i] + 1;
		int valid = 1;

		while (*p) {
			if (*p != 'n' && *p != 'e' && *p != 'E') {
				valid = 0;
				break;
			}
			p++;
		}
		if (!valid)
			break;
		for (p = argv[i] + 1; *p; p++) {
			if (*p == 'n')
				newline = 0;
			else if (*p == 'e')
				escapes = 1;
			else if (*p == 'E')
				escapes = 0;
		}
		i++;
	}

	buf_init(&out);
	for (; i < argc; i++) {
		if (out.len)
			buf_putc(&out, ' ');
		if (!escapes) {
			buf_puts(&out, argv[i]);
			continue;
		}
		{
			const char *p = argv[i];

			while (*p) {
				if (*p != '\\' || !p[1]) {
					buf_putc(&out, *p++);
					continue;
				}
				p++;
				switch (*p) {
				case 'a': buf_putc(&out, '\a'); p++; break;
				case 'b': buf_putc(&out, '\b'); p++; break;
				case 'e': buf_putc(&out, 0x1b); p++; break;
				case 'f': buf_putc(&out, '\f'); p++; break;
				case 'n': buf_putc(&out, '\n'); p++; break;
				case 'r': buf_putc(&out, '\r'); p++; break;
				case 't': buf_putc(&out, '\t'); p++; break;
				case 'v': buf_putc(&out, '\v'); p++; break;
				case '\\': buf_putc(&out, '\\'); p++; break;
				case 'c':
					fwrite(out.b, 1, out.len, stdout);
					buf_free(&out);
					fflush(stdout);
					return 0;
				case '0': {
					int v = 0, n = 0;
					p++;
					while (n < 3 && *p >= '0' && *p <= '7') {
						v = v * 8 + (*p - '0');
						p++;
						n++;
					}
					buf_putc(&out, v);
					break;
				}
				case 'x': {
					int v = 0, n = 0;
					p++;
					while (n < 2 && isxdigit((unsigned char)*p)) {
						int c = tolower((unsigned char)*p);
						v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
						p++;
						n++;
					}
					buf_putc(&out, v);
					break;
				}
				default:
					buf_putc(&out, '\\');
					buf_putc(&out, *p++);
					break;
				}
			}
		}
	}
	if (newline)
		buf_putc(&out, '\n');
	fwrite(out.b, 1, out.len, stdout);
	buf_free(&out);
	return 0;
}

/* GNU printf: recycles the format until the arguments run out, %b and %q. */
static int printf_core(int argc, char **argv, Buf *out)
{
	const char *fmt = argv[1];
	int argi = 2;
	int consumed_any = 0;

	if (argc < 2)
		return arg_error("printf", "usage: printf format [arguments]");

	do {
		const char *p = fmt;

		consumed_any = 0;
		while (*p) {
			if (*p != '%') {
				if (*p == '\\' && p[1]) {
					p++;
					switch (*p) {
					case 'a': buf_putc(out, '\a'); break;
					case 'b': buf_putc(out, '\b'); break;
					case 'e': buf_putc(out, 0x1b); break;
					case 'f': buf_putc(out, '\f'); break;
					case 'n': buf_putc(out, '\n'); break;
					case 'r': buf_putc(out, '\r'); break;
					case 't': buf_putc(out, '\t'); break;
					case 'v': buf_putc(out, '\v'); break;
					case '\\': buf_putc(out, '\\'); break;
					case '0': {
						int v = 0, n = 0;
						p++;
						while (n < 3 && *p >= '0' && *p <= '7') {
							v = v * 8 + (*p - '0');
							p++;
							n++;
						}
						p--;
						buf_putc(out, v);
						break;
					}
					default:
						buf_putc(out, '\\');
						buf_putc(out, *p);
						break;
					}
					p++;
					continue;
				}
				buf_putc(out, *p++);
				continue;
			}
			p++;
			if (*p == '%') {
				buf_putc(out, '%');
				p++;
				continue;
			}
			{
				Buf spec;
				const char *arg;
				char conv;

				buf_init(&spec);
				buf_putc(&spec, '%');
				while (*p && strchr("-+ #0'", *p))
					buf_putc(&spec, *p++);
				while (isdigit((unsigned char)*p))
					buf_putc(&spec, *p++);
				if (*p == '*') {
					const char *w = argi < argc ? argv[argi++] : "0";
					buf_printf(&spec, "%ld", strtol(w, NULL, 10));
					p++;
					consumed_any = 1;
				}
				if (*p == '.') {
					buf_putc(&spec, *p++);
					if (*p == '*') {
						const char *w = argi < argc ? argv[argi++] : "0";
						buf_printf(&spec, "%ld", strtol(w, NULL, 10));
						p++;
						consumed_any = 1;
					} else {
						while (isdigit((unsigned char)*p))
							buf_putc(&spec, *p++);
					}
				}
				while (*p && strchr("hlLqjzt", *p))
					p++;
				conv = *p ? *p++ : 's';
				arg = argi < argc ? argv[argi++] : "";
				if (argi <= argc)
					consumed_any = 1;

				switch (conv) {
				case 'd':
				case 'i': {
					int ok = 1;
					long v = *arg ? arith_eval(arg, &ok) : 0;
					buf_puts(&spec, "ld");
					buf_printf(out, spec.b, v);
					break;
				}
				case 'u':
				case 'o':
				case 'x':
				case 'X': {
					int ok = 1;
					unsigned long v =
						(unsigned long)(*arg ? arith_eval(arg, &ok) : 0);
					buf_putc(&spec, 'l');
					buf_putc(&spec, conv);
					buf_printf(out, spec.b, v);
					break;
				}
				case 'c':
					buf_putc(&spec, 'c');
					buf_printf(out, spec.b, arg[0]);
					break;
				case 'e':
				case 'E':
				case 'f':
				case 'F':
				case 'g':
				case 'G':
				case 'a':
				case 'A':
					buf_putc(&spec, conv);
					buf_printf(out, spec.b, strtod(arg, NULL));
					break;
				case 'q': {
					char *q = shell_quote(arg);
					buf_putc(&spec, 's');
					buf_printf(out, spec.b, q);
					free(q);
					break;
				}
				case 'b': {
					Buf tmp;

					buf_init(&tmp);
					{
						const char *e = arg;
						while (*e) {
							if (*e == '\\' && e[1]) {
								e++;
								switch (*e) {
								case 'n': buf_putc(&tmp, '\n'); break;
								case 't': buf_putc(&tmp, '\t'); break;
								case 'r': buf_putc(&tmp, '\r'); break;
								case '\\': buf_putc(&tmp, '\\'); break;
								case 'a': buf_putc(&tmp, '\a'); break;
								case 'b': buf_putc(&tmp, '\b'); break;
								case 'f': buf_putc(&tmp, '\f'); break;
								case 'v': buf_putc(&tmp, '\v'); break;
								case 'e': buf_putc(&tmp, 0x1b); break;
								case '0': {
									int v = 0, n = 0;
									e++;
									while (n < 3 && *e >= '0' &&
									       *e <= '7') {
										v = v * 8 + (*e - '0');
										e++;
										n++;
									}
									e--;
									buf_putc(&tmp, v);
									break;
								}
								default:
									buf_putc(&tmp, '\\');
									buf_putc(&tmp, *e);
									break;
								}
								e++;
								continue;
							}
							buf_putc(&tmp, *e++);
						}
					}
					buf_putc(&spec, 's');
					buf_printf(out, spec.b, tmp.b ? tmp.b : "");
					buf_free(&tmp);
					break;
				}
				case 's':
				default:
					buf_putc(&spec, 's');
					buf_printf(out, spec.b, arg);
					break;
				}
				buf_free(&spec);
			}
		}
	} while (argi < argc && consumed_any);

	return 0;
}

static int b_printf(int argc, char **argv)
{
	Buf out;
	int rc;
	const char *varname = NULL;
	char **args = argv;
	char **shifted = NULL;

	if (argc >= 3 && strcmp(argv[1], "-v") == 0) {
		int i;

		varname = argv[2];
		/* printf_core wants argv[0] to be the command name, so build a
		 * shifted view rather than writing into the caller's argv */
		argc -= 2;
		shifted = xmalloc((size_t)(argc + 1) * sizeof *shifted);
		shifted[0] = argv[0];
		for (i = 1; i < argc; i++)
			shifted[i] = argv[i + 2];
		shifted[argc] = NULL;
		args = shifted;
	}
	buf_init(&out);
	rc = printf_core(argc, args, &out);
	if (rc == 0) {
		if (varname) {
			var_set(varname, out.b ? out.b : "", 0);
		} else {
			fwrite(out.b, 1, out.len, stdout);
			fflush(stdout);
		}
	}
	buf_free(&out);
	free(shifted);
	return rc;
}

static int b_pwd(int argc, char **argv)
{
	char buf[PATH_MAX];
	int physical = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-P") == 0)
			physical = 1;
		else if (strcmp(argv[i], "-L") == 0)
			physical = 0;
	}
	if (!physical) {
		const char *pwd = var_get("PWD");
		if (pwd && pwd[0] == '/') {
			puts(pwd);
			return 0;
		}
	}
	if (!getcwd(buf, sizeof buf)) {
		shell_error("pwd: %s", strerror(errno));
		return 1;
	}
	puts(buf);
	return 0;
}

static int b_cd(int argc, char **argv)
{
	const char *target = NULL;
	char buf[PATH_MAX];
	int i;
	int physical = 0;
	int print = 0;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-P") == 0) {
			physical = 1;
		} else if (strcmp(argv[i], "-L") == 0) {
			physical = 0;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else {
			break;
		}
	}
	if (i < argc)
		target = argv[i];

	if (!target) {
		target = var_get("HOME");
		if (!target)
			return arg_error("cd", "HOME not set");
	} else if (strcmp(target, "-") == 0) {
		target = var_get("OLDPWD");
		if (!target)
			return arg_error("cd", "OLDPWD not set");
		print = 1;
	}

	if (chdir(target) != 0) {
		/* CDPATH */
		const char *cdpath = var_get("CDPATH");
		int done = 0;

		if (cdpath && target[0] != '/' && target[0] != '.') {
			char *copy = xstrdup(cdpath), *save = NULL, *dir;

			for (dir = strtok_r(copy, ":", &save); dir;
			     dir = strtok_r(NULL, ":", &save)) {
				char *full = xasprintf("%s/%s", *dir ? dir : ".", target);
				if (chdir(full) == 0) {
					done = 1;
					print = 1;
					free(full);
					break;
				}
				free(full);
			}
			free(copy);
		}
		if (!done && sh.shopt.cdspell) {
			/* shopt -s cdspell: try one transposition, one dropped
			 * character and one wrong character */
			size_t n = strlen(target);
			char *try = xmalloc(n + 2);
			size_t k;

			for (k = 0; k + 1 < n && !done; k++) {
				memcpy(try, target, n + 1);
				try[k] = target[k + 1];
				try[k + 1] = target[k];
				if (chdir(try) == 0)
					done = 1;
			}
			for (k = 0; k < n && !done; k++) {
				memcpy(try, target, k);
				memcpy(try + k, target + k + 1, n - k);
				if (*try && chdir(try) == 0)
					done = 1;
			}
			if (done) {
				shell_error("cd: assuming you meant %s", try);
				print = 1;
			}
			free(try);
		}
		if (!done) {
			shell_error("cd: %s: %s", target, strerror(errno));
			return 1;
		}
	}

	{
		const char *oldpwd = var_get("PWD");
		if (oldpwd)
			var_set_global("OLDPWD", oldpwd, V_EXPORT);
	}
	if (getcwd(buf, sizeof buf))
		var_set_global("PWD", buf, V_EXPORT);
	(void)physical;
	if (print)
		puts(var_get("PWD") ? var_get("PWD") : buf);
	return 0;
}

static int b_exit(int argc, char **argv)
{
	sh.exit_requested = 1;
	sh.exit_code = argc > 1 ? (int)strtol(argv[1], NULL, 10) & 0xff : sh.last_status;
	return sh.exit_code;
}

static int b_return(int argc, char **argv)
{
	if (!sh.in_function && !sh.script_name) {
		return arg_error("return", "can only `return' from a function or sourced script");
	}
	sh.returning = 1;
	sh.return_value = argc > 1 ? (int)strtol(argv[1], NULL, 10) & 0xff : sh.last_status;
	return sh.return_value;
}

static int b_shift(int argc, char **argv)
{
	int n = argc > 1 ? (int)strtol(argv[1], NULL, 10) : 1;

	if (n < 0 || (size_t)n > args_count())
		return 1;
	args_shift(n);
	return 0;
}

static int b_eval(int argc, char **argv)
{
	Buf b;
	int i, rc;

	if (argc < 2)
		return 0;
	buf_init(&b);
	for (i = 1; i < argc; i++) {
		if (i > 1)
			buf_putc(&b, ' ');
		buf_puts(&b, argv[i]);
	}
	rc = exec_string(b.b ? b.b : "", "eval");
	buf_free(&b);
	return rc;
}

static int b_source(int argc, char **argv)
{
	char *path;
	int rc;
	Vec args;
	int i;
	int saved_returning;

	if (argc < 2)
		return arg_error(argv[0], "filename argument required");

	if (strchr(argv[1], '/')) {
		path = xstrdup(argv[1]);
	} else {
		path = path_lookup(argv[1]);
		if (!path && access(argv[1], R_OK) == 0)
			path = xstrdup(argv[1]);
		if (!path) {
			shell_error("%s: %s: file not found", argv[0], argv[1]);
			return 1;
		}
	}

	vec_init(&args);
	for (i = 2; i < argc; i++)
		vec_pushs(&args, argv[i]);

	saved_returning = sh.returning;
	sh.returning = 0;
	if (args.len) {
		rc = exec_file(path, &args);
	} else {
		rc = exec_file(path, NULL);
	}
	if (sh.returning) {
		rc = sh.return_value;
		sh.last_status = rc;
	}
	sh.returning = saved_returning;
	vec_free(&args);
	free(path);
	return rc;
}

static int b_exec(int argc, char **argv)
{
	Vec av;
	int i;
	char **env;
	char *path;

	if (argc < 2)
		return 0;
	vec_init(&av);
	for (i = 1; i < argc; i++)
		vec_pushs(&av, argv[i]);
	path = path_lookup(av.v[0]);
	if (!path) {
		shell_error("exec: %s: not found", av.v[0]);
		vec_free(&av);
		return 127;
	}
	env = vars_environ();
	fflush(NULL);
	execve(path, vec_argv(&av), env);
	shell_error("exec: %s: %s", av.v[0], strerror(errno));
	vec_free(&av);
	free(path);
	return 126;
}

static int b_colon(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	return 0;
}

/* ---------------------------------------------------------------- variables */

static unsigned parse_decl_flags(const char *opts, int *unset_flags, int *print,
				 int *func_only, int *global)
{
	unsigned flags = 0;
	const char *p;
	int off = 0;

	for (p = opts; *p; p++) {
		if (p == opts && (*p == '-' || *p == '+')) {
			off = *p == '+';
			continue;
		}
		switch (*p) {
		case 'a': flags |= V_ARRAY; break;
		case 'A': flags |= V_ASSOC; break;
		case 'i': flags |= V_INTEGER; break;
		case 'l': flags |= V_LOWER; break;
		case 'u': flags |= V_UPPER; break;
		case 'r': flags |= V_READONLY; break;
		case 'x': flags |= V_EXPORT; break;
		case 't': flags |= V_TRACE; break;
		case 'n': flags |= V_NAMEREF; break;
		case 'g': *global = 1; break;
		case 'p': *print = 1; break;
		case 'f':
		case 'F': *func_only = 1; break;
		default: break;
		}
	}
	if (off)
		*unset_flags = 1;
	return flags;
}

static int declare_common(int argc, char **argv, int force_local, int force_export,
			  int force_readonly)
{
	unsigned flags = 0;
	int unset_flags = 0, print = 0, func_only = 0, global = 0;
	int i = 1;
	int rc = 0;

	while (i < argc && (argv[i][0] == '-' || argv[i][0] == '+') && argv[i][1]) {
		if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		}
		flags |= parse_decl_flags(argv[i], &unset_flags, &print, &func_only, &global);
		i++;
	}
	if (force_export)
		flags |= V_EXPORT;
	if (force_readonly)
		flags |= V_READONLY;

	if (func_only) {
		Func *f;
		for (f = sh.funcs; f; f = f->next)
			printf("%s ()\n", f->name);
		return 0;
	}

	if (i >= argc) {
		Vec names;
		size_t k;

		vec_init(&names);
		vars_all(&names);
		for (k = 0; k < names.len; k++) {
			Var *v = var_find(names.v[k]);
			char *line;

			if (!v)
				continue;
			if (flags && !(v->flags & flags))
				continue;
			if (force_export && !(v->flags & V_EXPORT))
				continue;
			line = var_render_decl(v, print || force_export || force_readonly
							  ? (force_export ? "declare -x" : "declare")
							  : "declare");
			if (force_export) {
				char *q = v->val ? shell_quote(v->val) : NULL;
				printf("declare -x %s%s%s\n", v->name, q ? "=" : "",
				       q ? q : "");
				free(q);
			} else {
				puts(line);
			}
			free(line);
		}
		vec_free(&names);
		return 0;
	}

	for (; i < argc; i++) {
		char *eq = strchr(argv[i], '=');
		char *name = eq ? xstrndup(argv[i], (size_t)(eq - argv[i])) : xstrdup(argv[i]);

		{
			char *bracket = strchr(name, '[');
			char *plus;

			if (bracket)
				*bracket = '\0';
			plus = strchr(name, '+');
			if (plus)
				*plus = '\0';
		}
		if (!is_valid_name(name)) {
			shell_error("%s: `%s': not a valid identifier", argv[0], argv[i]);
			rc = 1;
			free(name);
			continue;
		}
		if (print) {
			Var *v = var_find(name);
			if (v) {
				char *line = var_render_decl(v, "declare");
				puts(line);
				free(line);
			} else {
				shell_error("%s: %s: not found", argv[0], name);
				rc = 1;
			}
			free(name);
			continue;
		}
		if (force_local || (sh.in_function && !global && force_local))
			var_make_local(name);
		if (unset_flags) {
			Var *v = var_find(name);
			if (v)
				v->flags &= ~flags;
		} else if (flags) {
			var_declare(name, flags);
		}
		if (eq) {
			/* let the assignment machinery handle arrays and += */
			char *text = xstrdup(argv[i]);
			extern void builtins_assign(const char *text);
			builtins_assign(text);
			free(text);
			if (flags & V_EXPORT)
				var_export(name, 1);
		} else if (!flags && !force_local && !force_export && !force_readonly) {
			Var *v = var_find(name);
			if (v) {
				char *line = var_render_decl(v, "declare");
				puts(line);
				free(line);
			}
		}
		free(name);
	}
	return rc;
}

static int b_declare(int argc, char **argv)
{
	return declare_common(argc, argv, 0, 0, 0);
}

static int b_local(int argc, char **argv)
{
	if (!sh.in_function)
		return arg_error("local", "can only be used in a function");
	return declare_common(argc, argv, 1, 0, 0);
}

static int b_export(int argc, char **argv)
{
	return declare_common(argc, argv, 0, 1, 0);
}

static int b_readonly(int argc, char **argv)
{
	return declare_common(argc, argv, 0, 0, 1);
}

static int b_unset(int argc, char **argv)
{
	int i = 1;
	int funcs = 0, vars = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strchr(argv[i], 'f'))
			funcs = 1;
		if (strchr(argv[i], 'v'))
			vars = 1;
	}
	for (; i < argc; i++) {
		char *name = xstrdup(argv[i]);
		char *bracket = strchr(name, '[');

		if (bracket && !funcs) {
			char *close = strrchr(bracket, ']');
			*bracket = '\0';
			if (close)
				*close = '\0';
			if (strcmp(bracket + 1, "@") == 0 || strcmp(bracket + 1, "*") == 0)
				var_array_clear(name);
			else {
				char *idx = expand_unsplit(bracket + 1);
				var_array_unset(name, idx);
				free(idx);
			}
			free(name);
			continue;
		}
		if (funcs) {
			func_undefine(name);
		} else {
			if (!vars && func_find(name) && !var_find(name))
				func_undefine(name);
			else
				var_unset(name);
		}
		free(name);
	}
	return 0;
}

/* ------------------------------------------------------------------- read */

static int b_read(int argc, char **argv)
{
	int i = 1;
	int raw = 0, silent = 0, nchars = 0;
	const char *delim_opt = NULL;
	const char *prompt = NULL;
	const char *arrayname = NULL;
	Vec names;
	Buf line;
	int c;
	int rc = 0;
	int timeout_set = 0;
	double timeout = 0;

	vec_init(&names);
	for (; i < argc; i++) {
		const char *a = argv[i];

		if (a[0] != '-' || !a[1])
			break;
		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(a, "-r") == 0) {
			raw = 1;
		} else if (strcmp(a, "-s") == 0) {
			silent = 1;
		} else if (strcmp(a, "-p") == 0 && i + 1 < argc) {
			prompt = argv[++i];
		} else if (strcmp(a, "-d") == 0 && i + 1 < argc) {
			delim_opt = argv[++i];
		} else if (strcmp(a, "-n") == 0 && i + 1 < argc) {
			nchars = atoi(argv[++i]);
		} else if (strcmp(a, "-N") == 0 && i + 1 < argc) {
			nchars = atoi(argv[++i]);
		} else if (strcmp(a, "-a") == 0 && i + 1 < argc) {
			arrayname = argv[++i];
		} else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
			timeout = strtod(argv[++i], NULL);
			timeout_set = 1;
		} else {
			/* combined short options such as -rs or -ra name */
			const char *p;
			int understood = 1;

			for (p = a + 1; *p && understood; p++) {
				switch (*p) {
				case 'r': raw = 1; break;
				case 's': silent = 1; break;
				case 'e': break;
				case 'a':
					if (i + 1 < argc)
						arrayname = argv[++i];
					break;
				case 'p':
					if (i + 1 < argc)
						prompt = argv[++i];
					break;
				case 'd':
					if (i + 1 < argc)
						delim_opt = argv[++i];
					break;
				case 'n':
				case 'N':
					if (i + 1 < argc)
						nchars = atoi(argv[++i]);
					break;
				case 't':
					if (i + 1 < argc) {
						timeout = strtod(argv[++i], NULL);
						timeout_set = 1;
					}
					break;
				default:
					understood = 0;
					break;
				}
			}
			if (!understood)
				break;
		}
	}
	for (; i < argc; i++)
		vec_pushs(&names, argv[i]);

	(void)silent;
	(void)timeout;
	(void)timeout_set;

	if (prompt) {
		fputs(prompt, stderr);
		fflush(stderr);
	}

	buf_init(&line);
	{
		int delim = delim_opt ? (delim_opt[0] ? delim_opt[0] : '\0') : '\n';
		int count = 0;

		while ((c = fgetc(stdin)) != EOF) {
			if (!raw && c == '\\') {
				int nc = fgetc(stdin);
				if (nc == '\n')
					continue;
				if (nc == EOF)
					break;
				buf_putc(&line, nc);
				continue;
			}
			if (c == delim)
				break;
			buf_putc(&line, c);
			if (nchars && ++count >= nchars)
				break;
		}
		if (c == EOF && line.len == 0)
			rc = 1;
	}

	if (arrayname) {
		Vec fields;
		size_t k;
		char idx[32];
		const char *ifs = var_get("IFS");
		char *save = NULL, *tok;
		char *copy = xstrdup(line.b ? line.b : "");

		vec_init(&fields);
		for (tok = strtok_r(copy, ifs && *ifs ? ifs : " \t\n", &save); tok;
		     tok = strtok_r(NULL, ifs && *ifs ? ifs : " \t\n", &save))
			vec_pushs(&fields, tok);
		var_array_clear(arrayname);
		var_declare(arrayname, V_ARRAY);
		for (k = 0; k < fields.len; k++) {
			snprintf(idx, sizeof idx, "%zu", k);
			var_array_set(arrayname, idx, fields.v[k], 0);
		}
		vec_free(&fields);
		free(copy);
		buf_free(&line);
		vec_free(&names);
		return rc;
	}

	if (!names.len) {
		var_set("REPLY", line.b ? line.b : "", 0);
		buf_free(&line);
		vec_free(&names);
		return rc;
	}

	{
		const char *ifs = var_get("IFS");
		const char *s = line.b ? line.b : "";
		size_t k;

		if (!ifs)
			ifs = " \t\n";
		for (k = 0; k < names.len; k++) {
			Buf field;

			while (*s && strchr(ifs, *s) && strchr(" \t\n", *s))
				s++;
			buf_init(&field);
			if (k + 1 == names.len) {
				const char *end = s + strlen(s);
				while (end > s && strchr(ifs, end[-1]) && strchr(" \t\n", end[-1]))
					end--;
				buf_put(&field, s, (size_t)(end - s));
				s = end;
			} else {
				while (*s && !strchr(ifs, *s))
					buf_putc(&field, *s++);
				if (*s)
					s++;
			}
			var_set(names.v[k], field.b ? field.b : "", 0);
			buf_free(&field);
		}
	}
	buf_free(&line);
	vec_free(&names);
	return rc;
}

static int b_mapfile(int argc, char **argv)
{
	const char *name = "MAPFILE";
	int strip = 0;
	int i = 1;
	long maxcount = 0, skip = 0, origin = 0;
	Buf line;
	int c;
	long index;
	char idx[32];
	int delim = '\n';

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strcmp(argv[i], "-t") == 0)
			strip = 1;
		else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
			maxcount = strtol(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc)
			skip = strtol(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-O") == 0 && i + 1 < argc)
			origin = strtol(argv[++i], NULL, 10);
		else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
			delim = argv[++i][0];
		else if (strcmp(argv[i], "-u") == 0 && i + 1 < argc)
			i++; /* fd: honoured through the caller's redirection */
	}
	if (i < argc)
		name = argv[i];

	if (!origin) {
		var_array_clear(name);
		var_declare(name, V_ARRAY);
	}
	index = origin;
	buf_init(&line);
	{
		long seen = 0, stored = 0;

		for (;;) {
			int got_any = 0;

			buf_reset(&line);
			while ((c = fgetc(stdin)) != EOF) {
				got_any = 1;
				buf_putc(&line, c);
				if (c == delim)
					break;
			}
			if (!got_any)
				break;
			if (seen++ < skip)
				continue;
			if (maxcount && stored >= maxcount)
				break;
			if (strip && line.len && line.b[line.len - 1] == delim)
				line.b[--line.len] = '\0';
			snprintf(idx, sizeof idx, "%ld", index++);
			var_array_set(name, idx, line.b ? line.b : "", 0);
			stored++;
		}
	}
	buf_free(&line);
	return 0;
}

/* ------------------------------------------------------------------ options */

struct opt_entry {
	const char *name;
	int *flag;
	char letter;
};

static struct opt_entry *option_table(size_t *n)
{
	static struct opt_entry table[] = {
		{ "errexit", &sh.opt.errexit, 'e' },
		{ "nounset", &sh.opt.nounset, 'u' },
		{ "xtrace", &sh.opt.xtrace, 'x' },
		{ "verbose", &sh.opt.verbose, 'v' },
		{ "noglob", &sh.opt.noglob, 'f' },
		{ "noexec", &sh.opt.noexec, 'n' },
		{ "pipefail", &sh.opt.pipefail, 0 },
		{ "monitor", &sh.opt.monitor, 'm' },
		{ "notify", &sh.opt.notify, 'b' },
		{ "noclobber", &sh.opt.noclobber, 'C' },
		{ "allexport", &sh.opt.allexport, 'a' },
		{ "emacs", &sh.opt.emacs, 0 },
		{ "vi", &sh.opt.vi, 0 },
		{ "posix", &sh.opt.posix, 0 },
		{ "privileged", &sh.opt.privileged, 'p' },
		{ "onecmd", &sh.opt.onecmd, 't' },
		{ "hashall", &sh.opt.hashall, 'h' },
		{ "histexpand", &sh.opt.histexpand, 'H' },
		{ "history", &sh.opt.history, 0 },
		{ "ignoreeof", &sh.opt.ignoreeof, 0 },
		{ "keyword", &sh.opt.keyword, 'k' },
	};

	*n = sizeof table / sizeof table[0];
	return table;
}

static struct opt_entry *shopt_table(size_t *n)
{
	static struct opt_entry table[] = {
		{ "globstar", &sh.shopt.globstar, 0 },
		{ "nullglob", &sh.shopt.nullglob, 0 },
		{ "failglob", &sh.shopt.failglob, 0 },
		{ "dotglob", &sh.shopt.dotglob, 0 },
		{ "extglob", &sh.shopt.extglob, 0 },
		{ "nocasematch", &sh.shopt.nocasematch, 0 },
		{ "nocaseglob", &sh.shopt.nocaseglob, 0 },
		{ "inherit_errexit", &sh.shopt.inherit_errexit, 0 },
		{ "lastpipe", &sh.shopt.lastpipe, 0 },
		{ "expand_aliases", &sh.shopt.expand_aliases, 0 },
		{ "checkwinsize", &sh.shopt.checkwinsize, 0 },
		{ "cmdhist", &sh.shopt.cmdhist, 0 },
		{ "histappend", &sh.shopt.histappend, 0 },
		{ "autocd", &sh.shopt.autocd, 0 },
		{ "cdspell", &sh.shopt.cdspell, 0 },
		{ "gnu_builtins", &sh.shopt.gnu_builtins, 0 },
		{ "gnu_warn", &sh.shopt.gnu_warn, 0 },
		{ "syntax_highlight", &sh.shopt.syntax_highlight, 0 },
		{ "color", &sh.shopt.color, 0 },
		{ "xpg_echo", &sh.shopt.xpg_echo, 0 },
		{ "huponexit", &sh.shopt.huponexit, 0 },
		{ "progcomp", &sh.shopt.progcomp, 0 },
		{ "sourcepath", &sh.shopt.sourcepath, 0 },
		{ "interactive_comments", &sh.shopt.interactive_comments, 0 },
	};

	*n = sizeof table / sizeof table[0];
	return table;
}

static int b_set(int argc, char **argv)
{
	size_t n;
	struct opt_entry *table = option_table(&n);
	int i = 1;
	size_t k;

	if (argc == 1) {
		Vec names;
		size_t j;

		vec_init(&names);
		vars_all(&names);
		for (j = 0; j < names.len; j++) {
			Var *v = var_find(names.v[j]);
			if (!v || (v->flags & V_UNSET))
				continue;
			if (v->flags & (V_ARRAY | V_ASSOC)) {
				char *line = var_render_decl(v, "declare");
				puts(line);
				free(line);
			} else {
				char *q = shell_quote(v->val ? v->val : "");
				printf("%s=%s\n", v->name, q);
				free(q);
			}
		}
		vec_free(&names);
		return 0;
	}

	for (; i < argc; i++) {
		const char *a = argv[i];

		if (strcmp(a, "--") == 0) {
			i++;
			break;
		}
		if (strcmp(a, "-o") == 0 || strcmp(a, "+o") == 0) {
			int on = a[0] == '-';

			if (i + 1 >= argc) {
				for (k = 0; k < n; k++)
					printf("%-16s%s\n", table[k].name,
					       *table[k].flag ? "on" : "off");
				continue;
			}
			i++;
			for (k = 0; k < n; k++) {
				if (strcmp(table[k].name, argv[i]) == 0) {
					*table[k].flag = on;
					break;
				}
			}
			if (k == n)
				return arg_error("set", "%s: invalid option name", argv[i]);
			continue;
		}
		if ((a[0] == '-' || a[0] == '+') && a[1]) {
			int on = a[0] == '-';
			const char *p;

			for (p = a + 1; *p; p++) {
				for (k = 0; k < n; k++) {
					if (table[k].letter == *p) {
						*table[k].flag = on;
						break;
					}
				}
				if (k == n)
					return arg_error("set", "-%c: invalid option", *p);
			}
			continue;
		}
		break;
	}

	if (i < argc || (argc > 1 && strcmp(argv[argc - 1], "--") == 0)) {
		Vec args;

		vec_init(&args);
		for (; i < argc; i++)
			vec_pushs(&args, argv[i]);
		args_set(&args);
	}
	return 0;
}

static int b_shopt(int argc, char **argv)
{
	size_t n;
	struct opt_entry *table = shopt_table(&n);
	int set = -1, quiet = 0, print = 0;
	int i = 1;
	size_t k;
	int rc = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strcmp(argv[i], "-s") == 0)
			set = 1;
		else if (strcmp(argv[i], "-u") == 0)
			set = 0;
		else if (strcmp(argv[i], "-q") == 0)
			quiet = 1;
		else if (strcmp(argv[i], "-p") == 0)
			print = 1;
		else if (strcmp(argv[i], "-o") == 0)
			return b_set(argc - i + 1, argv + i - 1);
	}
	(void)print;

	if (i >= argc) {
		for (k = 0; k < n; k++) {
			if (set >= 0 && *table[k].flag != set)
				continue;
			printf("%-24s%s\n", table[k].name, *table[k].flag ? "on" : "off");
		}
		return 0;
	}
	for (; i < argc; i++) {
		for (k = 0; k < n; k++)
			if (strcmp(table[k].name, argv[i]) == 0)
				break;
		if (k == n) {
			shell_error("shopt: %s: invalid shell option name", argv[i]);
			rc = 1;
			continue;
		}
		if (set >= 0)
			*table[k].flag = set;
		else if (!quiet)
			printf("%-24s%s\n", table[k].name, *table[k].flag ? "on" : "off");
		if (set < 0 && *table[k].flag == 0)
			rc = 1;
	}
	return rc;
}

/* ------------------------------------------------------------------ aliases */

static int b_alias(int argc, char **argv)
{
	int i;
	int rc = 0;

	if (argc == 1) {
		Vec out;
		size_t k;

		vec_init(&out);
		alias_list(&out);
		for (k = 0; k < out.len; k++) {
			char *eq = strchr(out.v[k], '=');
			if (eq) {
				char *q = shell_quote(eq + 1);
				*eq = '\0';
				printf("alias %s=%s\n", out.v[k], q);
				free(q);
			}
		}
		vec_free(&out);
		return 0;
	}
	for (i = 1; i < argc; i++) {
		char *eq = strchr(argv[i], '=');

		if (eq) {
			char *name = xstrndup(argv[i], (size_t)(eq - argv[i]));
			alias_set(name, eq + 1);
			free(name);
		} else {
			const char *v = alias_get(argv[i]);
			if (v) {
				char *q = shell_quote(v);
				printf("alias %s=%s\n", argv[i], q);
				free(q);
			} else {
				shell_error("alias: %s: not found", argv[i]);
				rc = 1;
			}
		}
	}
	return rc;
}

static int b_unalias(int argc, char **argv)
{
	int i;

	if (argc > 1 && strcmp(argv[1], "-a") == 0) {
		while (sh.aliases)
			alias_unset(sh.aliases->name);
		return 0;
	}
	for (i = 1; i < argc; i++)
		alias_unset(argv[i]);
	return 0;
}

/* ------------------------------------------------------------------- type */

static int b_type(int argc, char **argv)
{
	int i = 1;
	int rc = 0;
	int mode = 0; /* 0 full, 't' short, 'p' path, 'a' all */

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strchr(argv[i], 't'))
			mode = 't';
		else if (strchr(argv[i], 'p') || strchr(argv[i], 'P'))
			mode = 'p';
		else if (strchr(argv[i], 'a'))
			mode = 'a';
	}
	for (; i < argc; i++) {
		const char *name = argv[i];
		const char *kind = NULL;
		char *path = NULL;

		if (alias_get(name))
			kind = "alias";
		else if (func_find(name))
			kind = "function";
		else if (builtin_find(name))
			kind = "builtin";
		else if (sh.shopt.gnu_builtins && gnu_find(name))
			kind = "builtin";
		else if ((path = path_lookup(name)))
			kind = "file";

		if (!kind) {
			if (mode != 't' && mode != 'p')
				fprintf(stderr, "crish: type: %s: not found\n", name);
			rc = 1;
			continue;
		}
		if (mode == 't') {
			puts(kind);
		} else if (mode == 'p') {
			if (path)
				puts(path);
		} else if (strcmp(kind, "alias") == 0) {
			printf("%s is aliased to `%s'\n", name, alias_get(name));
		} else if (strcmp(kind, "file") == 0) {
			printf("%s is %s\n", name, path);
		} else if (strcmp(kind, "builtin") == 0 && gnu_find(name) &&
			   !builtin_find(name)) {
			printf("%s is a shell builtin (CriSH GNU-compatible)\n", name);
		} else {
			printf("%s is a shell %s\n", name, kind);
		}
		free(path);
	}
	return rc;
}

static int b_command(int argc, char **argv)
{
	/* the -v/-V forms; the plain form is handled in exec.c */
	int i = 1;
	int verbose = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		if (strchr(argv[i], 'v'))
			verbose = 'v';
		else if (strchr(argv[i], 'V'))
			verbose = 'V';
	}
	if (!verbose) {
		if (i >= argc)
			return 0;
		return 0;
	}
	{
		char *fake[4];
		int rc;

		fake[0] = (char *)"type";
		fake[1] = (char *)(verbose == 'v' ? "-p" : "");
		fake[2] = argv[i];
		fake[3] = NULL;
		if (verbose == 'v') {
			const char *name = argv[i];
			if (func_find(name) || builtin_find(name) ||
			    (sh.shopt.gnu_builtins && gnu_find(name))) {
				puts(name);
				return 0;
			}
			{
				char *p = path_lookup(name);
				if (p) {
					puts(p);
					free(p);
					return 0;
				}
			}
			return 1;
		}
		rc = b_type(3, fake);
		return rc;
	}
}

/* -------------------------------------------------------------------- misc */

static int b_let(int argc, char **argv)
{
	int i;
	long last = 0;
	int ok = 1;

	if (argc < 2)
		return arg_error("let", "expression expected");
	for (i = 1; i < argc; i++)
		last = arith_eval(argv[i], &ok);
	if (!ok)
		return 1;
	return last != 0 ? 0 : 1;
}

static int b_getopts(int argc, char **argv)
{
	const char *spec;
	const char *name;
	const char *optind_s = var_get("OPTIND");
	long optind_v = optind_s ? strtol(optind_s, NULL, 10) : 1;
	static int charpos = 1;
	const char *arg;
	char opt;
	const char *found;
	char valbuf[2];
	Vec *pos = args_vec();
	size_t nargs;
	char **list;
	int silent;

	if (argc < 3)
		return arg_error("getopts", "usage: getopts optstring name [args]");
	spec = argv[1];
	name = argv[2];
	silent = spec[0] == ':';

	if (argc > 3) {
		nargs = (size_t)(argc - 3);
		list = argv + 3;
	} else {
		nargs = pos->len;
		list = pos->v;
	}

	if (optind_v <= 0)
		optind_v = 1;
	if ((size_t)optind_v > nargs)
		return 1;
	arg = list[optind_v - 1];
	if (!arg || arg[0] != '-' || !arg[1]) {
		charpos = 1;
		return 1;
	}
	if (strcmp(arg, "--") == 0) {
		char buf[32];
		snprintf(buf, sizeof buf, "%ld", optind_v + 1);
		var_set("OPTIND", buf, 0);
		charpos = 1;
		return 1;
	}
	opt = arg[charpos];
	if (!opt) {
		char buf[32];
		charpos = 1;
		snprintf(buf, sizeof buf, "%ld", optind_v + 1);
		var_set("OPTIND", buf, 0);
		return b_getopts(argc, argv);
	}
	charpos++;
	found = strchr(spec + (silent ? 1 : 0), opt);
	valbuf[0] = opt;
	valbuf[1] = '\0';

	if (!found || opt == ':') {
		var_set(name, "?", 0);
		if (silent)
			var_set("OPTARG", valbuf, 0);
		else
			fprintf(stderr, "crish: illegal option -- %c\n", opt);
		if (!arg[charpos]) {
			char buf[32];
			snprintf(buf, sizeof buf, "%ld", optind_v + 1);
			var_set("OPTIND", buf, 0);
			charpos = 1;
		}
		return 0;
	}
	if (found[1] == ':') {
		const char *value;

		if (arg[charpos]) {
			value = arg + charpos;
		} else if ((size_t)optind_v < nargs) {
			value = list[optind_v];
			optind_v++;
		} else {
			if (silent) {
				var_set(name, ":", 0);
				var_set("OPTARG", valbuf, 0);
			} else {
				fprintf(stderr, "crish: option requires an argument -- %c\n",
					opt);
				var_set(name, "?", 0);
			}
			{
				char buf[32];
				snprintf(buf, sizeof buf, "%ld", optind_v + 1);
				var_set("OPTIND", buf, 0);
			}
			charpos = 1;
			return 0;
		}
		var_set("OPTARG", value, 0);
		charpos = 1;
		optind_v++;
	} else {
		var_unset("OPTARG");
		if (!arg[charpos]) {
			charpos = 1;
			optind_v++;
		}
	}
	{
		char buf[32];
		snprintf(buf, sizeof buf, "%ld", optind_v);
		var_set("OPTIND", buf, 0);
	}
	var_set(name, valbuf, 0);
	return 0;
}

static int b_break(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1;

	if (n < 1)
		n = 1;
	if (!sh.loop_depth)
		return 0;
	sh.break_count = n;
	return 0;
}

static int b_continue(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 1;

	if (n < 1)
		n = 1;
	if (!sh.loop_depth)
		return 0;
	sh.continue_count = n;
	return 0;
}

static int b_trap(int argc, char **argv)
{
	int i = 1;

	if (argc == 1 || (argc > 1 && strcmp(argv[1], "-p") == 0)) {
		int s;
		for (s = 0; s < 64; s++) {
			const char *a = trap_get(s);
			if (a) {
				char *q = shell_quote(a);
				printf("trap -- %s %s\n", q, trap_signal_name(s));
				free(q);
			}
		}
		return 0;
	}
	if (strcmp(argv[1], "-l") == 0) {
		int s;
		for (s = 1; s < 32; s++)
			printf("%2d) SIG%s\n", s, trap_signal_name(s));
		return 0;
	}
	{
		const char *action = argv[i++];
		int reset = strcmp(action, "-") == 0;

		for (; i < argc; i++) {
			int sig = trap_signal_number(argv[i]);
			if (sig < 0) {
				shell_error("trap: %s: invalid signal specification", argv[i]);
				return 1;
			}
			trap_set(sig, reset ? NULL : action);
		}
	}
	return 0;
}

static int b_jobs(int argc, char **argv)
{
	int mode = 0;
	int i;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-l") == 0)
			mode = 'l';
		else if (strcmp(argv[i], "-p") == 0)
			mode = 'p';
	}
	jobs_list(mode);
	return 0;
}

static int b_fg(int argc, char **argv)
{
	return job_fg(argc > 1 ? argv[1] : NULL);
}

static int b_bg(int argc, char **argv)
{
	return job_bg(argc > 1 ? argv[1] : NULL);
}

static int b_wait(int argc, char **argv)
{
	int status = 0;

	if (argc == 1) {
		while (wait(&status) > 0)
			;
		jobs_reap();
		return 0;
	}
	{
		int i;

		for (i = 1; i < argc; i++) {
			pid_t pid;
			const char *a = argv[i];

			if (a[0] == '%')
				continue;
			pid = (pid_t)strtol(a, NULL, 10);
			if (waitpid(pid, &status, 0) < 0)
				continue;
		}
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

static int b_kill(int argc, char **argv)
{
	int sig = SIGTERM;
	int i = 1;
	int rc = 0;

	if (argc > 1 && strcmp(argv[1], "-l") == 0) {
		int s;
		for (s = 1; s < 32; s++)
			printf("%2d) SIG%-9s%s", s, trap_signal_name(s), s % 4 ? "" : "\n");
		printf("\n");
		return 0;
	}
	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		const char *name = argv[i] + 1;

		if (strcmp(name, "s") == 0 && i + 1 < argc)
			name = argv[++i];
		else if (strcmp(name, "n") == 0 && i + 1 < argc)
			name = argv[++i];
		sig = trap_signal_number(name);
		if (sig < 0) {
			shell_error("kill: %s: invalid signal specification", argv[i]);
			return 1;
		}
	}
	for (; i < argc; i++) {
		pid_t pid = (pid_t)strtol(argv[i], NULL, 10);

		if (kill(pid, sig) < 0) {
			shell_error("kill: (%ld): %s", (long)pid, strerror(errno));
			rc = 1;
		}
	}
	return rc;
}

static int b_umask(int argc, char **argv)
{
	mode_t old = umask(0);

	umask(old);
	if (argc == 1 || (argc == 2 && strcmp(argv[1], "-S") == 0)) {
		if (argc == 2) {
			char buf[16];
			mode_t m = ~old & 0777;
			snprintf(buf, sizeof buf, "u=%s%s%s,g=%s%s%s,o=%s%s%s",
				 m & 0400 ? "r" : "", m & 0200 ? "w" : "", m & 0100 ? "x" : "",
				 m & 0040 ? "r" : "", m & 0020 ? "w" : "", m & 0010 ? "x" : "",
				 m & 0004 ? "r" : "", m & 0002 ? "w" : "", m & 0001 ? "x" : "");
			puts(buf);
		} else {
			printf("%04o\n", old);
		}
		return 0;
	}
	umask((mode_t)strtol(argv[1], NULL, 8));
	return 0;
}

static int b_times(int argc, char **argv)
{
	struct tms t;
	long ticks = sysconf(_SC_CLK_TCK);

	(void)argc;
	(void)argv;
	times(&t);
	printf("%ldm%.3fs %ldm%.3fs\n", t.tms_utime / ticks / 60,
	       (double)(t.tms_utime % (ticks * 60)) / ticks, t.tms_stime / ticks / 60,
	       (double)(t.tms_stime % (ticks * 60)) / ticks);
	printf("%ldm%.3fs %ldm%.3fs\n", t.tms_cutime / ticks / 60,
	       (double)(t.tms_cutime % (ticks * 60)) / ticks, t.tms_cstime / ticks / 60,
	       (double)(t.tms_cstime % (ticks * 60)) / ticks);
	return 0;
}

static int b_hash(int argc, char **argv)
{
	int i;

	if (argc > 1 && strcmp(argv[1], "-r") == 0) {
		path_hash_clear();
		return 0;
	}
	for (i = 1; i < argc; i++) {
		char *p = path_lookup(argv[i]);
		if (!p) {
			shell_error("hash: %s: not found", argv[i]);
			return 1;
		}
		free(p);
	}
	return 0;
}

static int b_dirs(int argc, char **argv)
{
	size_t i;
	int one_per_line = 0;

	for (i = 1; i < (size_t)argc; i++) {
		if (strcmp(argv[i], "-c") == 0) {
			vec_clear(&sh.dirstack);
			return 0;
		}
		if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-v") == 0)
			one_per_line = 1;
	}
	printf("%s", var_get("PWD") ? var_get("PWD") : "");
	for (i = sh.dirstack.len; i > 0; i--)
		printf("%s%s", one_per_line ? "\n" : " ", sh.dirstack.v[i - 1]);
	printf("\n");
	return 0;
}

static int b_pushd(int argc, char **argv)
{
	const char *pwd = var_get("PWD");
	char *fake[3];

	if (argc < 2) {
		if (!sh.dirstack.len)
			return arg_error("pushd", "no other directory");
		{
			char *top = vec_remove(&sh.dirstack, sh.dirstack.len - 1);
			fake[0] = (char *)"cd";
			fake[1] = top;
			fake[2] = NULL;
			if (pwd)
				vec_pushs(&sh.dirstack, pwd);
			b_cd(2, fake);
			free(top);
		}
		return b_dirs(1, argv);
	}
	if (pwd)
		vec_pushs(&sh.dirstack, pwd);
	fake[0] = (char *)"cd";
	fake[1] = argv[1];
	fake[2] = NULL;
	if (b_cd(2, fake) != 0) {
		if (sh.dirstack.len)
			free(vec_remove(&sh.dirstack, sh.dirstack.len - 1));
		return 1;
	}
	return b_dirs(1, argv);
}

static int b_popd(int argc, char **argv)
{
	char *fake[3];
	char *top;

	if (!sh.dirstack.len)
		return arg_error("popd", "directory stack empty");
	top = vec_remove(&sh.dirstack, sh.dirstack.len - 1);
	fake[0] = (char *)"cd";
	fake[1] = top;
	fake[2] = NULL;
	b_cd(2, fake);
	free(top);
	return b_dirs(argc, argv);
}

static int b_ulimit(int argc, char **argv)
{
	struct rlimit rl;
	int which = RLIMIT_FSIZE;
	int i = 1;
	int hard = 0;

	for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
		switch (argv[i][1]) {
		case 'n': which = RLIMIT_NOFILE; break;
		case 'u': which = RLIMIT_NPROC; break;
		case 's': which = RLIMIT_STACK; break;
		case 'c': which = RLIMIT_CORE; break;
		case 'f': which = RLIMIT_FSIZE; break;
		case 'v': which = RLIMIT_AS; break;
		case 'H': hard = 1; break;
		case 'S': hard = 0; break;
		case 'a': {
			static const struct {
				int which;
				const char *label;
			} all[] = { { RLIMIT_CORE, "core file size (blocks)" },
				    { RLIMIT_FSIZE, "file size (blocks)" },
				    { RLIMIT_NOFILE, "open files" },
				    { RLIMIT_STACK, "stack size (kbytes)" },
				    { RLIMIT_NPROC, "max user processes" } };
			size_t k;
			for (k = 0; k < sizeof all / sizeof all[0]; k++) {
				getrlimit(all[k].which, &rl);
				if (rl.rlim_cur == RLIM_INFINITY)
					printf("%-32s unlimited\n", all[k].label);
				else
					printf("%-32s %llu\n", all[k].label,
					       (unsigned long long)rl.rlim_cur);
			}
			return 0;
		}
		default: break;
		}
	}
	if (getrlimit(which, &rl) < 0)
		return 1;
	if (i >= argc) {
		rlim_t v = hard ? rl.rlim_max : rl.rlim_cur;
		if (v == RLIM_INFINITY)
			puts("unlimited");
		else
			printf("%llu\n", (unsigned long long)v);
		return 0;
	}
	{
		rlim_t v = strcmp(argv[i], "unlimited") == 0
				   ? RLIM_INFINITY
				   : (rlim_t)strtoull(argv[i], NULL, 10);
		if (hard)
			rl.rlim_max = v;
		else
			rl.rlim_cur = v;
		if (setrlimit(which, &rl) < 0) {
			shell_error("ulimit: %s", strerror(errno));
			return 1;
		}
	}
	return 0;
}

static int b_history(int argc, char **argv)
{
	size_t n = history_count();
	size_t start = 0;
	size_t i;

	if (argc > 1 && strcmp(argv[1], "-c") == 0) {
		history_clear();
		return 0;
	}
	if (argc > 1 && isdigit((unsigned char)argv[1][0])) {
		size_t want = (size_t)strtoul(argv[1], NULL, 10);
		if (want < n)
			start = n - want;
	}
	for (i = start; i < n; i++)
		printf("%5zu  %s\n", i + 1, history_get(i));
	return 0;
}

static int b_help(int argc, char **argv)
{
	size_t n, i;
	const Builtin *table = builtin_table(&n);

	if (argc > 1) {
		for (i = 0; i < n; i++) {
			if (strcmp(table[i].name, argv[1]) == 0) {
				printf("%s: %s\n    %s\n", table[i].name, table[i].usage,
				       table[i].summary);
				return 0;
			}
		}
		{
			const GnuTool *g = gnu_find(argv[1]);
			if (g) {
				printf("%s: %s\n", g->name, g->summary);
				printf("    A GNU-compatible built-in.  Run `%s --help'.\n",
				       g->name);
				return 0;
			}
		}
		shell_error("help: no help topics match `%s'", argv[1]);
		return 1;
	}

	printf("CriSH %s - a shell for macOS that runs Linux scripts unchanged.\n\n",
	       CRISH_VERSION);
	printf("Shell builtins:\n");
	for (i = 0; i < n; i++)
		printf("  %-12s %s\n", table[i].name, table[i].summary);
	printf("\nBuilt-in GNU-compatible utilities:\n  ");
	{
		size_t gn;
		const GnuTool *g = gnu_table(&gn);
		for (i = 0; i < gn; i++)
			printf("%s%s", g[i].name, i + 1 < gn ? " " : "\n");
	}
	printf("\nRun `help NAME' for one command, `crish --version' for the version.\n");
	return 0;
}

static int b_theme(int argc, char **argv)
{
	size_t count, i;
	const char *const *names = color_theme_names(&count);
	int j;

	if (argc > 1 && (strcmp(argv[1], "--roles") == 0)) {
		for (j = 0; j < C_ROLE_COUNT; j++) {
			const char *name = color_role_name((ColorRole)j);

			if (name)
				printf("%s%s", name, (j + 1) % 8 ? " " : "\n");
		}
		printf("\n");
		return 0;
	}
	if (argc > 1 && strcmp(argv[1], "--preview") == 0) {
		printf("theme %s%s\n\n", color_theme_name(),
		       color_enabled() ? "" : "   (colour is off right now)");
		for (j = 1; j < C_ROLE_COUNT; j++) {
			const char *name = color_role_name((ColorRole)j);

			if (!name)
				continue;
			printf("  %s%-10s%s  %s\n", color((ColorRole)j), name, color_off(),
			       name);
		}
		return 0;
	}
	if (argc > 1) {
		if (!color_set_theme(argv[1])) {
			shell_error("theme: %s: no such theme", argv[1]);
			return 1;
		}
		var_set("CRISH_THEME", argv[1], V_EXPORT);
		return 0;
	}
	for (i = 0; i < count; i++)
		printf("%s%s\n", strcmp(names[i], color_theme_name()) == 0 ? "* " : "  ",
		       names[i]);
	printf("\nthe current theme is %s; `theme NAME' switches, "
	       "`theme --preview' shows every role\n",
	       color_theme_name());
	return 0;
}

/* Used by declare to route NAME=value through the normal assignment path. */
void builtins_assign(const char *text);
void builtins_assign(const char *text)
{
	char *err = NULL;
	Node *n;
	Buf b;

	buf_init(&b);
	buf_puts(&b, text);
	n = parse_string(b.b, "declare", &err);
	if (n)
		exec_node(n);
	node_free(n);
	free(err);
	buf_free(&b);
}

/* -------------------------------------------------------------- the table */

static const Builtin builtins[] = {
	{ ":", b_colon, ":", "do nothing, successfully" },
	{ ".", b_source, ". file [args]", "read and run a file in this shell" },
	{ "alias", b_alias, "alias [name[=value] ...]", "define or list aliases" },
	{ "bg", b_bg, "bg [job]", "resume a job in the background" },
	{ "break", b_break, "break [n]", "leave a loop" },
	{ "builtin", b_colon, "builtin cmd [args]", "run a builtin, ignoring functions" },
	{ "cd", b_cd, "cd [-L|-P] [dir]", "change the working directory" },
	{ "command", b_command, "command [-vV] cmd", "run a command, ignoring functions" },
	{ "continue", b_continue, "continue [n]", "start the next loop iteration" },
	{ "declare", b_declare, "declare [-aAfgilnrtux] [name[=value]]", "declare variables" },
	{ "dirs", b_dirs, "dirs [-clpv]", "show the directory stack" },
	{ "echo", b_echo, "echo [-neE] [args]", "write arguments to standard output" },
	{ "eval", b_eval, "eval [args]", "run arguments as a shell command" },
	{ "exec", b_exec, "exec [cmd [args]]", "replace the shell with a command" },
	{ "exit", b_exit, "exit [n]", "leave the shell" },
	{ "export", b_export, "export [-fn] [name[=value]]", "put variables in the environment" },
	{ "false", b_false, "false", "fail" },
	{ "fg", b_fg, "fg [job]", "bring a job to the foreground" },
	{ "getopts", b_getopts, "getopts optstring name [args]", "parse option arguments" },
	{ "hash", b_hash, "hash [-r] [name]", "remember or forget command locations" },
	{ "help", b_help, "help [name]", "describe builtins" },
	{ "history", b_history, "history [-c] [n]", "show the command history" },
	{ "jobs", b_jobs, "jobs [-lp]", "list active jobs" },
	{ "kill", b_kill, "kill [-s sig] pid", "send a signal" },
	{ "let", b_let, "let expr [expr ...]", "evaluate arithmetic" },
	{ "local", b_local, "local [-aAilnrux] name[=value]", "declare function-local variables" },
	{ "mapfile", b_mapfile, "mapfile [-tn] [array]", "read lines into an array" },
	{ "popd", b_popd, "popd", "pop the directory stack" },
	{ "printf", b_printf, "printf [-v var] format [args]", "format and print" },
	{ "pushd", b_pushd, "pushd [dir]", "push onto the directory stack" },
	{ "pwd", b_pwd, "pwd [-LP]", "print the working directory" },
	{ "read", b_read, "read [-rsp] [-d c] [-n n] [-a arr] [name ...]", "read one line" },
	{ "readarray", b_mapfile, "readarray [-tn] [array]", "read lines into an array" },
	{ "readonly", b_readonly, "readonly [-a] name[=value]", "mark variables unwritable" },
	{ "return", b_return, "return [n]", "leave a function or sourced file" },
	{ "set", b_set, "set [-abefhkmnptuvxC] [-o opt] [args]", "set options and parameters" },
	{ "shift", b_shift, "shift [n]", "drop leading positional parameters" },
	{ "shopt", b_shopt, "shopt [-pqsu] [name]", "set shell behaviour options" },
	{ "source", b_source, "source file [args]", "read and run a file in this shell" },
	{ "test", builtin_test, "test expr", "evaluate a conditional expression" },
	{ "theme", b_theme, "theme [name|--preview|--roles]", "choose the colour scheme" },
	{ "[", builtin_test, "[ expr ]", "evaluate a conditional expression" },
	{ "times", b_times, "times", "report accumulated process times" },
	{ "trap", b_trap, "trap [action] [signal ...]", "run a command on a signal" },
	{ "true", b_true, "true", "succeed" },
	{ "type", b_type, "type [-atpP] name", "say how a name would be interpreted" },
	{ "typeset", b_declare, "typeset [-aAfgilnrtux] name", "a synonym for declare" },
	{ "ulimit", b_ulimit, "ulimit [-HSacfnsuv] [limit]", "get or set resource limits" },
	{ "umask", b_umask, "umask [-S] [mode]", "get or set the file creation mask" },
	{ "unalias", b_unalias, "unalias [-a] name", "remove aliases" },
	{ "unset", b_unset, "unset [-fv] name", "remove variables or functions" },
	{ "update", cmd_update, "update [--check|--pre]", "update CriSH itself" },
	{ "wait", b_wait, "wait [pid ...]", "wait for background jobs" },
};

const Builtin *builtin_find(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof builtins / sizeof builtins[0]; i++)
		if (strcmp(builtins[i].name, name) == 0)
			return &builtins[i];
	return NULL;
}

const Builtin *builtin_table(size_t *count)
{
	*count = sizeof builtins / sizeof builtins[0];
	return builtins;
}
