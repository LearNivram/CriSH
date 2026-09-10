/* exec.c - running the syntax tree.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "highlight.h"
#include "shell.h"

int builtin_test(int argc, char **argv);
int cond_eval(Vec *tokens);
void expand_close_psubs(void);

/* errexit must not fire inside a condition or on the left of && / ||. */
static int errexit_suspended;

/* ---------------------------------------------------------- PATH lookup */

typedef struct HashEnt {
	char *name;
	char *path;
	long hits;
	struct HashEnt *next;
} HashEnt;

static HashEnt *cmd_hash;

void path_hash_clear(void)
{
	highlight_invalidate();
	while (cmd_hash) {
		HashEnt *next = cmd_hash->next;
		free(cmd_hash->name);
		free(cmd_hash->path);
		free(cmd_hash);
		cmd_hash = next;
	}
}

static int is_directory(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* shopt -s gnu_warn: say so, once per name, when a built-in utility is used
 * in place of a binary that is also on the PATH. */
static void warn_shadow(const char *name)
{
	static Vec warned;
	size_t i;
	char *found;

	if (!gnu_find(name))
		return;
	for (i = 0; i < warned.len; i++)
		if (strcmp(warned.v[i], name) == 0)
			return;
	found = path_lookup(name);
	if (found) {
		shell_error("%s: using the built-in GNU-compatible %s, not %s "
			    "(shopt -u gnu_builtins to change that)",
			    name, name, found);
		free(found);
	}
	vec_pushs(&warned, name);
}

static int is_executable(const char *path)
{
	struct stat st;

	return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

char *path_lookup(const char *name)
{
	const char *path;
	HashEnt *e;
	char *copy, *dir, *save = NULL;

	if (strchr(name, '/'))
		return is_executable(name) ? xstrdup(name) : NULL;

	for (e = cmd_hash; e; e = e->next) {
		if (strcmp(e->name, name) == 0) {
			if (is_executable(e->path)) {
				e->hits++;
				return xstrdup(e->path);
			}
			break;
		}
	}

	path = var_get("PATH");
	if (!path)
		path = "/usr/bin:/bin";
	copy = xstrdup(path);
	for (dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
		char *full = xasprintf("%s/%s", *dir ? dir : ".", name);

		if (is_executable(full)) {
			if (sh.opt.hashall) {
				HashEnt *n = xcalloc(1, sizeof *n);
				n->name = xstrdup(name);
				n->path = xstrdup(full);
				n->hits = 1;
				n->next = cmd_hash;
				cmd_hash = n;
			}
			free(copy);
			return full;
		}
		free(full);
	}
	free(copy);
	return NULL;
}

/* ------------------------------------------------------------- functions */

Func *func_find(const char *name)
{
	Func *f;

	for (f = sh.funcs; f; f = f->next)
		if (strcmp(f->name, name) == 0)
			return f;
	return NULL;
}

void func_define(const char *name, Node *body)
{
	Func *f = func_find(name);

	if (f) {
		node_free(f->body);
		f->body = body;
		return;
	}
	f = xcalloc(1, sizeof *f);
	f->name = xstrdup(name);
	f->body = body;
	f->next = sh.funcs;
	sh.funcs = f;
	highlight_invalidate();
}

void func_undefine(const char *name)
{
	Func **p;

	for (p = &sh.funcs; *p; p = &(*p)->next) {
		if (strcmp((*p)->name, name) == 0) {
			Func *f = *p;
			*p = f->next;
			free(f->name);
			node_free(f->body);
			free(f);
			highlight_invalidate();
			return;
		}
	}
}

/* --------------------------------------------------------------- aliases */

const char *alias_get(const char *name)
{
	Alias *a;

	for (a = sh.aliases; a; a = a->next)
		if (strcmp(a->name, name) == 0)
			return a->value;
	return NULL;
}

void alias_set(const char *name, const char *value)
{
	Alias *a;

	for (a = sh.aliases; a; a = a->next) {
		if (strcmp(a->name, name) == 0) {
			free(a->value);
			a->value = xstrdup(value);
			return;
		}
	}
	a = xcalloc(1, sizeof *a);
	a->name = xstrdup(name);
	a->value = xstrdup(value);
	a->next = sh.aliases;
	sh.aliases = a;
	highlight_invalidate();
}

void alias_unset(const char *name)
{
	Alias **p;

	for (p = &sh.aliases; *p; p = &(*p)->next) {
		if (strcmp((*p)->name, name) == 0) {
			Alias *a = *p;
			*p = a->next;
			free(a->name);
			free(a->value);
			free(a);
			highlight_invalidate();
			return;
		}
	}
}

void alias_list(Vec *out)
{
	Alias *a;

	for (a = sh.aliases; a; a = a->next)
		vec_push(out, xasprintf("%s=%s", a->name, a->value));
	vec_sort(out);
}

/* ------------------------------------------------------------ assignments */

/* NAME=(a b c) or NAME=([k]=v ...) */
static void assign_array(const char *name, const char *body, int append)
{
	Vec words, fields;
	size_t i;
	long next_index = 0;

	if (!append)
		var_array_clear(name);
	else
		next_index = (long)var_array_count(name);

	vec_init(&words);
	vec_init(&fields);
	{
		/* strip the outer parentheses */
		size_t len = strlen(body);
		char *inner;

		if (len >= 2 && body[0] == '(' && body[len - 1] == ')')
			inner = xstrndup(body + 1, len - 2);
		else
			inner = xstrdup(body);
		{
			char *err = NULL;
			Parser *p = parser_new(inner, "array assignment");
			Node *n = parser_next(p, &err);

			if (n && n->type == N_SIMPLE) {
				for (i = 0; i < n->assigns.len; i++)
					vec_pushs(&words, n->assigns.v[i]);
				for (i = 0; i < n->words.len; i++)
					vec_pushs(&words, n->words.v[i]);
			}
			node_free(n);
			free(err);
			parser_free(p);
		}
		free(inner);
	}

	for (i = 0; i < words.len; i++) {
		const char *w = words.v[i];

		if (w[0] == '[') {
			const char *close = NULL;
			int depth = 0;
			const char *q;

			for (q = w; *q; q++) {
				if (*q == '[')
					depth++;
				else if (*q == ']' && --depth == 0) {
					close = q;
					break;
				}
			}
			if (close && close[1] == '=') {
				char *key_raw = xstrndup(w + 1, (size_t)(close - w - 1));
				char *key = expand_unsplit(key_raw);
				Vec vals;
				size_t k;

				vec_init(&vals);
				expand_word(close + 2, &vals);
				var_array_set(name, key,
					      vals.len ? vals.v[0] : "", 0);
				for (k = 1; k < vals.len; k++)
					var_array_set(name, key, vals.v[k], 1);
				{
					Var *v = var_find(name);
					if (v && !(v->flags & V_ASSOC)) {
						int ok = 1;
						next_index = arith_eval(key, &ok) + 1;
					}
				}
				free(key_raw);
				free(key);
				vec_free(&vals);
				continue;
			}
		}
		vec_clear(&fields);
		expand_word(w, &fields);
		{
			size_t k;
			char idx[32];

			for (k = 0; k < fields.len; k++) {
				snprintf(idx, sizeof idx, "%ld", next_index++);
				var_array_set(name, idx, fields.v[k], 0);
			}
		}
	}
	vec_free(&words);
	vec_free(&fields);
}

/* Apply NAME=value, NAME+=value, NAME[idx]=value or NAME=(...). */
static void apply_assignment(const char *text, int exported)
{
	const char *eq = NULL;
	const char *p;
	int append = 0;
	char *name, *subscript = NULL;
	int depth = 0;

	for (p = text; *p; p++) {
		if (*p == '[')
			depth++;
		else if (*p == ']')
			depth--;
		else if (*p == '=' && depth == 0) {
			eq = p;
			break;
		}
	}
	if (!eq)
		return;
	if (eq > text && eq[-1] == '+') {
		append = 1;
		eq--;
	}
	name = xstrndup(text, (size_t)(eq - text));
	{
		char *bracket = strchr(name, '[');
		if (bracket) {
			char *close = strrchr(bracket, ']');
			*bracket = '\0';
			subscript = xstrdup(bracket + 1);
			if (close)
				subscript[strlen(subscript) - 1] = '\0';
		}
	}
	{
		const char *value = eq + (append ? 2 : 1);

		if (value[0] == '(' && !subscript) {
			assign_array(name, value, append);
		} else if (subscript) {
			char *idx = expand_unsplit(subscript);
			char *val = expand_unsplit(value);
			var_array_set(name, idx, val, append);
			free(idx);
			free(val);
		} else {
			char *val = expand_unsplit(value);

			if (append) {
				const char *old = var_get(name);
				Var *v = var_find(name);

				if (v && (v->flags & (V_ARRAY | V_ASSOC))) {
					char idx[32];
					snprintf(idx, sizeof idx, "%zu",
						 var_array_count(name));
					var_array_set(name, idx, val, 0);
					free(val);
					free(name);
					free(subscript);
					return;
				}
				if (v && (v->flags & V_INTEGER)) {
					int ok = 1;
					char buf[32];
					long sum = (old ? arith_eval(old, &ok) : 0) +
						   arith_eval(val, &ok);
					snprintf(buf, sizeof buf, "%ld", sum);
					var_set(name, buf, 0);
				} else {
					char *joined = xasprintf("%s%s", old ? old : "", val);
					var_set(name, joined, 0);
					free(joined);
				}
			} else {
				var_set(name, val, 0);
			}
			free(val);
		}
		if (exported)
			var_export(name, 1);
	}
	free(name);
	free(subscript);
}

/* ------------------------------------------------------------------ trace */

static void xtrace_words(Vec *argv, int quote)
{
	const char *ps4 = var_get("PS4");
	char *prefix = ps4 ? expand_to_string(ps4) : xstrdup("+ ");
	size_t i;

	fputs(prefix, stderr);
	for (i = 0; i < argv->len; i++) {
		char *q = quote ? shell_quote(argv->v[i]) : NULL;

		fprintf(stderr, "%s%s", i ? " " : "", q ? q : argv->v[i]);
		free(q);
	}
	fputc('\n', stderr);
	free(prefix);
}

static void xtrace(Vec *argv)
{
	xtrace_words(argv, 1);
}

/* bash traces an assignment with its value expanded, not its source text. */
static void trace_assignments(const Vec *assigns)
{
	Vec trace;
	size_t k;

	vec_init(&trace);
	for (k = 0; k < assigns->len; k++) {
		const char *text = assigns->v[k];
		const char *eq = strchr(text, '=');

		if (!eq) {
			vec_pushs(&trace, text);
			continue;
		}
		{
			char *name = xstrndup(text, (size_t)(eq - text));
			char *val = eq[1] == '(' ? xstrdup(eq + 1)
						 : expand_unsplit(eq + 1);
			char *quoted = shell_quote(val);

			vec_push(&trace, xasprintf("%s=%s", name, quoted));
			free(name);
			free(val);
			free(quoted);
		}
	}
	xtrace_words(&trace, 0);
	vec_free(&trace);
}

/* ------------------------------------------------------------- forked exec */

static void child_exec(Vec *argv)
{
	char *path = path_lookup(argv->v[0]);
	char **env = vars_environ();

	if (!path) {
		fprintf(stderr, "crish: %s: command not found\n", argv->v[0]);
		_exit(127);
	}
	execve(path, vec_argv(argv), env);
	if (errno == ENOEXEC) {
		/* a script with no shebang runs under this shell */
		Vec re;
		size_t i;

		vec_init(&re);
		vec_pushs(&re, "/bin/sh");
		for (i = 0; i < argv->len; i++)
			vec_pushs(&re, argv->v[i]);
		re.v[1] = xstrdup(path);
		execve("/bin/sh", vec_argv(&re), env);
	}
	fprintf(stderr, "crish: %s: %s\n", argv->v[0], strerror(errno));
	_exit(errno == ENOENT ? 127 : 126);
}

/* ----------------------------------------------------------- simple command */

static int run_function(Func *f, Vec *argv)
{
	Vec args;
	Vec saved;
	size_t i;
	int status;

	if (sh.depth > 512) {
		shell_error("%s: maximum function nesting level exceeded", f->name);
		return 1;
	}

	vec_init(&args);
	for (i = 1; i < argv->len; i++)
		vec_pushs(&args, argv->v[i]);
	saved = *args_vec();
	vec_init(args_vec());
	args_set(&args);

	vars_push_scope();
	sh.depth++;
	sh.in_function++;
	{
		int save_ret = sh.returning;
		sh.returning = 0;
		exec_node(f->body);
		status = sh.last_status;
		if (sh.returning) {
			status = sh.return_value;
			sh.last_status = status;
		}
		sh.returning = save_ret;
	}
	sh.in_function--;
	sh.depth--;
	vars_pop_scope();

	vec_free(args_vec());
	*args_vec() = saved;
	return status;
}

static int exec_simple(Node *n)
{
	Vec argv;
	SavedFds *saved = NULL;
	int ok = 1, status = 0;
	size_t i;
	int use_builtin = 1, use_function = 1, use_gnu = 1;
	const char *cmdname;

	vec_init(&argv);
	expand_words(&n->words, &argv);

	/* assignments with no command word change the shell */
	if (!argv.len) {
		if (sh.opt.xtrace && n->assigns.len)
			trace_assignments(&n->assigns);
		if (n->redir) {
			fflush(NULL);
			saved = redir_apply(n->redir, &ok);
			fflush(NULL);
			redir_restore(saved);
			if (!ok) {
				vec_free(&argv);
				return 1;
			}
		}
		for (i = 0; i < n->assigns.len; i++)
			apply_assignment(n->assigns.v[i], 0);
		vec_free(&argv);
		expand_close_psubs();
		return sh.last_status = 0;
	}

	if (sh.opt.xtrace)
		xtrace(&argv);

	cmdname = argv.v[0];

	/* command / builtin prefixes */
	while (argv.len > 1 &&
	       (strcmp(cmdname, "command") == 0 || strcmp(cmdname, "builtin") == 0)) {
		if (strcmp(cmdname, "command") == 0) {
			if (strcmp(argv.v[1], "-v") == 0 || strcmp(argv.v[1], "-V") == 0)
				break;
			if (strcmp(argv.v[1], "-p") == 0 && argv.len > 2) {
				/* -p asks for the system command, so the built-in
				 * GNU utilities step aside as well */
				use_gnu = 0;
				free(vec_remove(&argv, 1));
			}
		}
		use_function = 0;
		free(vec_remove(&argv, 0));
		cmdname = argv.v[0];
	}

	fflush(NULL);
	saved = redir_apply(n->redir, &ok);
	if (!ok) {
		redir_restore(saved);
		vec_free(&argv);
		expand_close_psubs();
		sh.last_status = 1;
		if (!sh.interactive && sh.opt.errexit && !errexit_suspended) {
			sh.exit_requested = 1;
			sh.exit_code = 1;
		}
		return 1;
	}

	/* temporary assignments live only for this command */
	if (n->assigns.len) {
		int is_special = builtin_find(cmdname) != NULL || func_find(cmdname) != NULL;

		if (is_special) {
			for (i = 0; i < n->assigns.len; i++)
				apply_assignment(n->assigns.v[i], 1);
		} else {
			vars_push_scope();
			for (i = 0; i < n->assigns.len; i++) {
				char *eq = strchr(n->assigns.v[i], '=');
				if (eq) {
					char *nm = xstrndup(n->assigns.v[i],
							    (size_t)(eq - n->assigns.v[i]));
					char *plus = strchr(nm, '+');
					if (plus)
						*plus = '\0';
					var_make_local(nm);
					free(nm);
				}
				apply_assignment(n->assigns.v[i], 1);
			}
		}
	}

	{
		Func *f = use_function ? func_find(cmdname) : NULL;
		const Builtin *b = use_builtin ? builtin_find(cmdname) : NULL;
		const GnuTool *g =
			(!f && !b && use_gnu && sh.shopt.gnu_builtins) ? gnu_find(cmdname)
								      : NULL;

		if (f) {
			status = run_function(f, &argv);
		} else if (b) {
			status = b->fn((int)argv.len, vec_argv(&argv));
		} else if (g) {
			if (sh.shopt.gnu_warn)
				warn_shadow(cmdname);
			fflush(stdout);
			status = g->fn((int)argv.len, vec_argv(&argv));
			fflush(stdout);
		} else if (sh.shopt.autocd && argv.len == 1 && !path_lookup(cmdname) &&
			   is_directory(cmdname)) {
			/* shopt -s autocd: a bare directory name means cd */
			char *fake[3];

			fake[0] = (char *)"cd";
			fake[1] = argv.v[0];
			fake[2] = NULL;
			status = builtin_find("cd")->fn(2, fake);
		} else {
			pid_t pid;

			fflush(NULL);
			pid = fork();
			if (pid < 0) {
				shell_error("fork: %s", strerror(errno));
				status = 1;
			} else if (pid == 0) {
				sh.subshell++;
				traps_reset_in_child();
				signal(SIGINT, SIG_DFL);
				signal(SIGQUIT, SIG_DFL);
				child_exec(&argv);
				_exit(127);
			} else {
				int wstatus;
				job_wait(pid, &wstatus);
				status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus)
							    : 128 + WTERMSIG(wstatus);
			}
		}
	}

	if (n->assigns.len && !(builtin_find(cmdname) || func_find(cmdname)))
		vars_pop_scope();

	fflush(NULL);
	redir_restore(saved);
	vec_free(&argv);
	expand_close_psubs();
	sh.last_status = status;
	return status;
}

/* ------------------------------------------------------------- pipelines */

static int exec_pipeline(Node *n)
{
	size_t i;
	int prev_read = -1;
	pid_t *pids = xcalloc(n->nkids, sizeof *pids);
	int status = 0;
	int worst = 0;

	fflush(NULL);
	for (i = 0; i < n->nkids; i++) {
		int fds[2] = { -1, -1 };
		int last = i + 1 == n->nkids;

		if (!last && pipe(fds) < 0) {
			shell_error("pipe: %s", strerror(errno));
			free(pids);
			return 1;
		}
		pids[i] = fork();
		if (pids[i] < 0) {
			shell_error("fork: %s", strerror(errno));
			free(pids);
			return 1;
		}
		if (pids[i] == 0) {
			sh.subshell++;
			traps_reset_in_child();
			signal(SIGINT, SIG_DFL);
			signal(SIGQUIT, SIG_DFL);
			signal(SIGPIPE, SIG_DFL);
			if (prev_read >= 0) {
				dup2(prev_read, 0);
				close(prev_read);
				redir_sync_std(0);
			}
			if (!last) {
				close(fds[0]);
				dup2(fds[1], 1);
				if (n->kidflags[i])
					dup2(fds[1], 2);
				close(fds[1]);
				redir_sync_std(1);
				if (n->kidflags[i])
					redir_sync_std(2);
			}
			exec_node(n->kids[i]);
			exit(sh.last_status);
		}
		if (prev_read >= 0)
			close(prev_read);
		if (!last) {
			close(fds[1]);
			prev_read = fds[0];
		}
	}

	for (i = 0; i < n->nkids; i++) {
		int wstatus = 0;
		int rc;

		job_wait(pids[i], &wstatus);
		rc = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
		if (i + 1 == n->nkids)
			status = rc;
		if (rc)
			worst = rc;
	}
	free(pids);
	if (sh.opt.pipefail && worst)
		status = worst;
	sh.last_status = status;
	return status;
}

/* ----------------------------------------------------------- control flow */

static int exec_subshell(Node *n)
{
	pid_t pid;
	int wstatus = 0;

	fflush(NULL);
	pid = fork();

	if (pid < 0) {
		shell_error("fork: %s", strerror(errno));
		return 1;
	}
	if (pid == 0) {
		int ok = 1;
		SavedFds *saved;

		sh.subshell++;
		traps_reset_in_child();
		signal(SIGINT, SIG_DFL);
		saved = redir_apply(n->redir, &ok);
		(void)saved;
		if (!ok)
			_exit(1);
		if (!sh.shopt.inherit_errexit)
			; /* bash keeps errexit in subshells for command substitution */
		exec_node(n->body);
		exit(sh.last_status);
	}
	job_wait(pid, &wstatus);
	sh.last_status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);
	return sh.last_status;
}

static int cond_status(Node *n)
{
	int saved = errexit_suspended;
	int r;

	errexit_suspended = 1;
	r = exec_node(n);
	errexit_suspended = saved;
	return r;
}

static int loop_body(Node *body)
{
	int status;

	sh.loop_depth++;
	status = exec_node(body);
	sh.loop_depth--;
	return status;
}

/* Returns 1 when the enclosing loop must stop. */
static int loop_control(void)
{
	if (sh.returning || sh.exit_requested)
		return 1;
	if (sh.break_count) {
		sh.break_count--;
		return 1;
	}
	if (sh.continue_count) {
		sh.continue_count--;
		if (sh.continue_count)
			return 1;
	}
	return 0;
}

static int exec_for(Node *n)
{
	Vec items;
	size_t i;
	int status = 0;

	vec_init(&items);
	if (n->no_items) {
		for (i = 1; i <= args_count(); i++)
			vec_pushs(&items, args_get(i));
	} else {
		expand_words(&n->items, &items);
	}
	for (i = 0; i < items.len; i++) {
		var_set(n->name, items.v[i], 0);
		status = loop_body(n->body);
		if (loop_control())
			break;
	}
	vec_free(&items);
	sh.last_status = status;
	return status;
}

static int exec_for_arith(Node *n)
{
	int ok = 1;
	int status = 0;

	if (n->a1 && *n->a1)
		arith_eval(n->a1, &ok);
	for (;;) {
		if (n->a2 && *n->a2) {
			long c = arith_eval(n->a2, &ok);
			if (!ok || !c)
				break;
		}
		status = loop_body(n->body);
		if (loop_control())
			break;
		if (n->a3 && *n->a3)
			arith_eval(n->a3, &ok);
	}
	sh.last_status = status;
	return status;
}

static int exec_while(Node *n, int until)
{
	int status = 0;

	for (;;) {
		int c = cond_status(n->left);

		if (sh.exit_requested || sh.returning)
			break;
		if (until ? (c == 0) : (c != 0))
			break;
		status = loop_body(n->body);
		if (loop_control())
			break;
	}
	sh.last_status = status;
	return status;
}

static int exec_case(Node *n)
{
	char *subject = expand_unsplit(n->word);
	CaseItem *item;
	int status = 0;
	int matched = 0;

	for (item = n->cases; item; item = item->next) {
		size_t i;
		int hit = 0;

		for (i = 0; i < item->patterns.len; i++) {
			char *pat = expand_pattern(item->patterns.v[i]);
			hit = glob_match(pat, subject, sh.shopt.nocasematch);
			free(pat);
			if (hit)
				break;
		}
		if (!hit)
			continue;
		matched = 1;
		for (;;) {
			if (item->body)
				status = exec_node(item->body);
			if (item->fallthrough && item->next) {
				item = item->next;
				continue;
			}
			break;
		}
		if (item && item->retest) {
			item = item->next;
			if (!item)
				break;
			/* ;;& retests the remaining patterns */
			{
				CaseItem *rest;
				for (rest = item; rest; rest = rest->next) {
					size_t k;
					int h = 0;
					for (k = 0; k < rest->patterns.len; k++) {
						char *pat = expand_pattern(rest->patterns.v[k]);
						h = glob_match(pat, subject,
							       sh.shopt.nocasematch);
						free(pat);
						if (h)
							break;
					}
					if (h && rest->body) {
						status = exec_node(rest->body);
						if (!rest->retest)
							break;
					}
				}
			}
		}
		break;
	}
	free(subject);
	if (!matched)
		status = 0;
	sh.last_status = status;
	return status;
}

static int exec_select(Node *n)
{
	Vec items;
	size_t i;
	int status = 0;
	const char *ps3;

	vec_init(&items);
	if (n->no_items) {
		for (i = 1; i <= args_count(); i++)
			vec_pushs(&items, args_get(i));
	} else {
		expand_words(&n->items, &items);
	}
	if (!items.len) {
		vec_free(&items);
		return 0;
	}
	ps3 = var_get("PS3");
	for (;;) {
		char line[1024];
		char *reply;

		for (i = 0; i < items.len; i++)
			fprintf(stderr, "%zu) %s\n", i + 1, items.v[i]);
		fputs(ps3 ? ps3 : "#? ", stderr);
		fflush(stderr);
		if (!fgets(line, sizeof line, stdin))
			break;
		line[strcspn(line, "\n")] = '\0';
		var_set("REPLY", line, 0);
		reply = line;
		if (!*reply)
			continue;
		{
			char *end;
			long k = strtol(reply, &end, 10);

			if (*end == '\0' && k >= 1 && (size_t)k <= items.len)
				var_set(n->name, items.v[k - 1], 0);
			else
				var_set(n->name, "", 0);
		}
		status = loop_body(n->body);
		if (loop_control())
			break;
	}
	vec_free(&items);
	sh.last_status = status;
	return status;
}

static void report_time(const struct timeval *start, const struct rusage *before)
{
	struct timeval now;
	struct rusage after;
	double real, user, sys;

	gettimeofday(&now, NULL);
	getrusage(RUSAGE_CHILDREN, &after);
	real = (double)(now.tv_sec - start->tv_sec) +
	       (double)(now.tv_usec - start->tv_usec) / 1e6;
	user = (double)(after.ru_utime.tv_sec - before->ru_utime.tv_sec) +
	       (double)(after.ru_utime.tv_usec - before->ru_utime.tv_usec) / 1e6;
	sys = (double)(after.ru_stime.tv_sec - before->ru_stime.tv_sec) +
	      (double)(after.ru_stime.tv_usec - before->ru_stime.tv_usec) / 1e6;
	fprintf(stderr, "\nreal\t%dm%.3fs\nuser\t%dm%.3fs\nsys\t%dm%.3fs\n", (int)(real / 60),
		real - 60 * (int)(real / 60), (int)(user / 60), user - 60 * (int)(user / 60),
		(int)(sys / 60), sys - 60 * (int)(sys / 60));
}

/* ------------------------------------------------------------ the dispatch */

int exec_node(Node *n)
{
	int status = 0;

	if (!n)
		return sh.last_status;
	if (sh.exit_requested || sh.returning || sh.break_count || sh.continue_count)
		return sh.last_status;

	trap_run_pending();
	sh.lineno = n->lineno;

	/* set -n / crish -n: parse everything, run nothing. */
	if (sh.opt.noexec && !sh.interactive)
		return 0;

	switch (n->type) {
	case N_SIMPLE:
		status = exec_simple(n);
		break;
	case N_PIPE:
		status = exec_pipeline(n);
		break;
	case N_SEQ: {
		size_t i;

		for (i = 0; i < n->nkids; i++) {
			if (n->kidflags[i]) {
				/* background */
				pid_t pid;

				fflush(NULL);
				pid = fork();

				if (pid == 0) {
					sh.subshell++;
					traps_reset_in_child();
					signal(SIGINT, SIG_IGN);
					signal(SIGQUIT, SIG_IGN);
					setpgid(0, 0);
					exec_node(n->kids[i]);
					exit(sh.last_status);
				}
				if (pid > 0) {
					setpgid(pid, pid);
					sh.last_bg_pid = pid;
					job_add(pid, "background job", 1);
				}
				status = 0;
				sh.last_status = 0;
			} else {
				status = exec_node(n->kids[i]);
			}
			if (sh.exit_requested || sh.returning || sh.break_count ||
			    sh.continue_count)
				break;
		}
		break;
	}
	case N_AND: {
		int saved = errexit_suspended;
		errexit_suspended = 1;
		status = exec_node(n->left);
		errexit_suspended = saved;
		if (status == 0 && !sh.exit_requested && !sh.returning)
			status = exec_node(n->right);
		sh.last_status = status;
		break;
	}
	case N_OR: {
		int saved = errexit_suspended;
		errexit_suspended = 1;
		status = exec_node(n->left);
		errexit_suspended = saved;
		if (status != 0 && !sh.exit_requested && !sh.returning)
			status = exec_node(n->right);
		sh.last_status = status;
		break;
	}
	case N_NOT: {
		int saved = errexit_suspended;
		errexit_suspended = 1;
		status = exec_node(n->left);
		errexit_suspended = saved;
		status = status == 0 ? 1 : 0;
		sh.last_status = status;
		break;
	}
	case N_SUBSHELL:
		status = exec_subshell(n);
		break;
	case N_GROUP: {
		int ok = 1;
		SavedFds *saved;

		fflush(NULL);
		saved = redir_apply(n->redir, &ok);
		if (ok)
			status = exec_node(n->body);
		else
			status = 1;
		fflush(NULL);
		redir_restore(saved);
		sh.last_status = status;
		break;
	}
	case N_IF: {
		int c = cond_status(n->left);

		if (sh.exit_requested || sh.returning)
			break;
		if (c == 0)
			status = exec_node(n->body);
		else if (n->els)
			status = exec_node(n->els);
		else
			status = 0;
		sh.last_status = status;
		break;
	}
	case N_WHILE:
		status = exec_while(n, 0);
		break;
	case N_UNTIL:
		status = exec_while(n, 1);
		break;
	case N_FOR:
		status = exec_for(n);
		break;
	case N_FOR_ARITH:
		status = exec_for_arith(n);
		break;
	case N_CASE:
		status = exec_case(n);
		break;
	case N_SELECT:
		status = exec_select(n);
		break;
	case N_FUNC:
		func_define(n->name, n->body);
		n->body = NULL;
		status = 0;
		sh.last_status = 0;
		break;
	case N_ARITH: {
		int ok = 1;
		long v = arith_eval(n->a1, &ok);

		status = (!ok || v == 0) ? 1 : 0;
		sh.last_status = status;
		break;
	}
	case N_COND: {
		int ok = 1;
		SavedFds *saved = redir_apply(n->redir, &ok);

		status = ok ? cond_eval(&n->cond) : 1;
		redir_restore(saved);
		sh.last_status = status;
		break;
	}
	case N_TIME: {
		struct timeval start;
		struct rusage before;

		gettimeofday(&start, NULL);
		getrusage(RUSAGE_CHILDREN, &before);
		status = exec_node(n->left);
		report_time(&start, &before);
		break;
	}
	default:
		status = 0;
		break;
	}

	if (status != 0 && sh.opt.errexit && !errexit_suspended && !sh.exit_requested) {
		if (n->type != N_IF && n->type != N_WHILE && n->type != N_UNTIL) {
			sh.exit_requested = 1;
			sh.exit_code = status;
		}
	}
	return status;
}

/* ----------------------------------------------------------- entry points */

int exec_string(const char *src, const char *origin)
{
	Parser *p = parser_new(src, origin);
	int status = sh.last_status;

	for (;;) {
		char *err = NULL;
		Node *n = parser_next(p, &err);

		if (err) {
			fprintf(stderr, "crish: %s\n", err);
			free(err);
			status = 2;
			sh.last_status = 2;
			break;
		}
		if (!n)
			break;
		status = exec_node(n);
		node_free(n);
		if (sh.exit_requested || sh.returning)
			break;
	}
	parser_free(p);
	return status;
}

static char *read_whole_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	Buf b;
	char chunk[8192];
	size_t n;

	if (!f)
		return NULL;
	buf_init(&b);
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_put(&b, chunk, n);
	fclose(f);
	return buf_take(&b);
}

int exec_file(const char *path, Vec *args)
{
	char *src = read_whole_file(path);
	const char *save_name = sh.script_name;
	int status;

	if (!src) {
		fprintf(stderr, "crish: %s: %s\n", path, strerror(errno));
		return 127;
	}
	sh.script_name = path;
	if (args) {
		Vec copy = vec_clone(args);
		Vec saved = *args_vec();

		vec_init(args_vec());
		args_set(&copy);
		status = exec_string(src, path);
		vec_free(args_vec());
		*args_vec() = saved;
	} else {
		status = exec_string(src, path);
	}
	sh.script_name = save_name;
	free(src);
	return status;
}

/* ---------------------------------------------------- command substitution */

char *exec_capture(Node *n, int *status)
{
	int fds[2];
	pid_t pid;
	Buf out;
	char chunk[4096];
	ssize_t got;
	int wstatus = 0;

	if (pipe(fds) < 0) {
		shell_error("pipe: %s", strerror(errno));
		if (status)
			*status = 1;
		return xstrdup("");
	}
	fflush(NULL);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		shell_error("fork: %s", strerror(errno));
		if (status)
			*status = 1;
		return xstrdup("");
	}
	if (pid == 0) {
		sh.subshell++;
		traps_reset_in_child();
		signal(SIGINT, SIG_DFL);
		close(fds[0]);
		dup2(fds[1], 1);
		close(fds[1]);
		redir_sync_std(1);
		exec_node(n);
		fflush(stdout);
		exit(sh.last_status);
	}
	close(fds[1]);
	buf_init(&out);
	while ((got = read(fds[0], chunk, sizeof chunk)) > 0)
		buf_put(&out, chunk, (size_t)got);
	close(fds[0]);
	waitpid(pid, &wstatus, 0);
	if (status)
		*status = WIFEXITED(wstatus) ? WEXITSTATUS(wstatus) : 128 + WTERMSIG(wstatus);

	/* trailing newlines are stripped, as everywhere else */
	while (out.len && out.b[out.len - 1] == '\n')
		out.b[--out.len] = '\0';
	return buf_take(&out);
}

char *capture_string(const char *src, int *status)
{
	char *err = NULL;
	Node *n = parse_string(src, "command substitution", &err);
	char *result;

	if (err) {
		fprintf(stderr, "crish: %s\n", err);
		free(err);
		node_free(n);
		if (status)
			*status = 2;
		return xstrdup("");
	}
	result = exec_capture(n, status);
	node_free(n);
	return result;
}
