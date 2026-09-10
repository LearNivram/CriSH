/* vars.c - variables, scopes, indexed and associative arrays.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "shell.h"

static Scope global;
static Scope *scopes = &global;
static Vec positional;
static time_t shell_start;
static unsigned long rand_state;

/* --------------------------------------------------------------- internals */

static void elem_free(AElem *e)
{
	while (e) {
		AElem *next = e->next;
		free(e->key);
		free(e->val);
		free(e);
		e = next;
	}
}

static void var_free(Var *v)
{
	free(v->name);
	free(v->val);
	elem_free(v->elems);
	free(v);
}

static Var *scope_find(Scope *s, const char *name)
{
	Var *v;

	for (v = s->vars; v; v = v->next)
		if (strcmp(v->name, name) == 0)
			return v;
	return NULL;
}

Var *var_find(const char *name)
{
	Scope *s;

	for (s = scopes; s; s = s->parent) {
		Var *v = scope_find(s, name);
		if (v)
			return v;
	}
	return NULL;
}

static Var *var_make(Scope *s, const char *name)
{
	Var *v = scope_find(s, name);

	if (v)
		return v;
	v = xcalloc(1, sizeof *v);
	v->name = xstrdup(name);
	v->next = s->vars;
	s->vars = v;
	return v;
}

/* Where an assignment lands: an existing local, else the current scope if a
 * `local` declaration created it, else the global scope. */
static Var *var_slot(const char *name)
{
	Scope *s;

	for (s = scopes; s; s = s->parent) {
		Var *v = scope_find(s, name);
		if (v)
			return v;
	}
	return var_make(&global, name);
}

/* -------------------------------------------------------- special variables */

static const char *special_value(const char *name)
{
	static char buf[64];

	if (strcmp(name, "RANDOM") == 0) {
		rand_state = rand_state * 6364136223846793005ULL + 1442695040888963407ULL;
		snprintf(buf, sizeof buf, "%u", (unsigned)((rand_state >> 33) & 0x7fff));
		return buf;
	}
	if (strcmp(name, "SECONDS") == 0) {
		snprintf(buf, sizeof buf, "%ld", (long)(time(NULL) - shell_start));
		return buf;
	}
	if (strcmp(name, "EPOCHSECONDS") == 0) {
		snprintf(buf, sizeof buf, "%lld", (long long)time(NULL));
		return buf;
	}
	if (strcmp(name, "EPOCHREALTIME") == 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		snprintf(buf, sizeof buf, "%lld.%06d", (long long)tv.tv_sec, (int)tv.tv_usec);
		return buf;
	}
	if (strcmp(name, "LINENO") == 0) {
		snprintf(buf, sizeof buf, "%d", sh.lineno);
		return buf;
	}
	if (strcmp(name, "BASHPID") == 0 || strcmp(name, "CRISHPID") == 0) {
		snprintf(buf, sizeof buf, "%ld", (long)getpid());
		return buf;
	}
	if (strcmp(name, "SRANDOM") == 0) {
		unsigned v;
		arc4random_buf(&v, sizeof v);
		snprintf(buf, sizeof buf, "%u", v);
		return buf;
	}
	return NULL;
}

static int is_special_name(const char *name)
{
	static const char *const names[] = { "RANDOM",	"SECONDS",   "EPOCHSECONDS",
					     "EPOCHREALTIME", "LINENO",    "BASHPID",
					     "CRISHPID",       "SRANDOM",   NULL };
	int i;

	for (i = 0; names[i]; i++)
		if (strcmp(names[i], name) == 0)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------- basics */

const char *var_get(const char *name)
{
	Var *v;

	if (is_special_name(name)) {
		v = var_find(name);
		if (!v || (v->flags & V_SPECIAL))
			return special_value(name);
	}
	v = var_find(name);
	if (!v)
		return NULL;
	if (v->flags & V_NAMEREF) {
		if (v->val && strcmp(v->val, name) != 0)
			return var_get(v->val);
		return NULL;
	}
	if ((v->flags & (V_ARRAY | V_ASSOC)) && !v->val)
		return var_array_get(name, "0");
	if (v->flags & V_UNSET)
		return NULL;
	return v->val;
}

static char *apply_case(const char *val, unsigned flags)
{
	char *out = xstrdup(val ? val : "");
	char *p;

	if (flags & V_UPPER)
		for (p = out; *p; p++)
			*p = (char)toupper((unsigned char)*p);
	else if (flags & V_LOWER)
		for (p = out; *p; p++)
			*p = (char)tolower((unsigned char)*p);
	return out;
}

static void set_in(Var *v, const char *val, unsigned add_flags)
{
	char *cooked;

	if (v->flags & V_READONLY) {
		shell_error("%s: readonly variable", v->name);
		return;
	}
	v->flags |= add_flags;
	v->flags &= ~V_UNSET;

	if (v->flags & V_INTEGER) {
		int ok = 1;
		long n = arith_eval(val && *val ? val : "0", &ok);
		cooked = xasprintf("%ld", n);
	} else {
		cooked = apply_case(val, v->flags);
	}
	free(v->val);
	v->val = cooked;
	if (sh.opt.allexport)
		v->flags |= V_EXPORT;
}

void var_set(const char *name, const char *val, unsigned add_flags)
{
	Var *v;

	if (is_special_name(name) && !var_find(name)) {
		/* assigning to SECONDS etc. turns it into a plain variable */
		v = var_make(&global, name);
		set_in(v, val, add_flags);
		return;
	}
	v = var_slot(name);
	if (v->flags & V_NAMEREF) {
		if (v->val && strcmp(v->val, name) != 0) {
			var_set(v->val, val, add_flags);
			return;
		}
	}
	set_in(v, val, add_flags);
}

void var_set_global(const char *name, const char *val, unsigned add_flags)
{
	set_in(var_make(&global, name), val, add_flags);
}

void var_declare(const char *name, unsigned flags)
{
	Var *v = var_slot(name);

	v->flags |= flags;
	if (!v->val && !v->elems)
		v->flags |= V_UNSET;
	if (flags & (V_UPPER | V_LOWER)) {
		if (v->val) {
			char *cooked = apply_case(v->val, v->flags);
			free(v->val);
			v->val = cooked;
		}
	}
}

void var_unset(const char *name)
{
	Scope *s;

	for (s = scopes; s; s = s->parent) {
		Var **p;

		for (p = &s->vars; *p; p = &(*p)->next) {
			if (strcmp((*p)->name, name) == 0) {
				Var *v = *p;
				if (v->flags & V_READONLY) {
					shell_error("unset: %s: cannot unset: readonly variable",
						    name);
					return;
				}
				*p = v->next;
				var_free(v);
				return;
			}
		}
	}
}

void var_export(const char *name, int on)
{
	Var *v = var_slot(name);

	if (on)
		v->flags |= V_EXPORT;
	else
		v->flags &= ~V_EXPORT;
}

void var_make_local(const char *name)
{
	Var *v;

	if (scopes == &global)
		return;
	v = scope_find(scopes, name);
	if (v)
		return;
	v = var_make(scopes, name);
	v->flags |= V_UNSET;
}

void vars_push_scope(void)
{
	Scope *s = xcalloc(1, sizeof *s);

	s->parent = scopes;
	scopes = s;
}

void vars_pop_scope(void)
{
	Scope *s = scopes;
	Var *v;

	if (s == &global)
		return;
	scopes = s->parent;
	v = s->vars;
	while (v) {
		Var *next = v->next;
		var_free(v);
		v = next;
	}
	free(s);
}

/* -------------------------------------------------------------- environment */

char **vars_environ(void)
{
	static Vec env;
	Scope *s;
	Vec seen;

	vec_free(&env);
	vec_init(&seen);
	for (s = scopes; s; s = s->parent) {
		Var *v;
		for (v = s->vars; v; v = v->next) {
			size_t i;
			int dup = 0;

			if (!(v->flags & V_EXPORT) || (v->flags & (V_ARRAY | V_ASSOC)))
				continue;
			for (i = 0; i < seen.len; i++)
				if (strcmp(seen.v[i], v->name) == 0) {
					dup = 1;
					break;
				}
			if (dup)
				continue;
			vec_pushs(&seen, v->name);
			vec_push(&env, xasprintf("%s=%s", v->name, v->val ? v->val : ""));
		}
	}
	vec_free(&seen);
	return vec_argv(&env);
}

void vars_all(Vec *out)
{
	Scope *s;

	for (s = scopes; s; s = s->parent) {
		Var *v;
		for (v = s->vars; v; v = v->next) {
			size_t i;
			int dup = 0;
			for (i = 0; i < out->len; i++)
				if (strcmp(out->v[i], v->name) == 0) {
					dup = 1;
					break;
				}
			if (!dup)
				vec_pushs(out, v->name);
		}
	}
	vec_sort(out);
}

/* ------------------------------------------------------------------- arrays */

static AElem *elem_find(Var *v, const char *key)
{
	AElem *e;

	for (e = v->elems; e; e = e->next)
		if (strcmp(e->key, key) == 0)
			return e;
	return NULL;
}

/* Keep indexed arrays ordered by numeric index so ${a[@]} comes out sorted. */
static AElem *elem_insert(Var *v, const char *key)
{
	AElem *e = xcalloc(1, sizeof *e);

	e->key = xstrdup(key);
	e->idx = (v->flags & V_ASSOC) ? 0 : strtol(key, NULL, 10);

	if (v->flags & V_ASSOC) {
		AElem **p = &v->elems;
		while (*p)
			p = &(*p)->next;
		e->next = NULL;
		*p = e;
	} else {
		AElem **p = &v->elems;
		while (*p && (*p)->idx < e->idx)
			p = &(*p)->next;
		e->next = *p;
		*p = e;
	}
	return e;
}

static Var *array_slot(const char *name)
{
	Var *v = var_slot(name);

	if (!(v->flags & (V_ARRAY | V_ASSOC)))
		v->flags |= V_ARRAY;
	return v;
}

void var_array_set(const char *name, const char *idx, const char *val, int append)
{
	Var *v = array_slot(name);
	AElem *e;
	char key[32];

	if (v->flags & V_READONLY) {
		shell_error("%s: readonly variable", name);
		return;
	}
	if (!(v->flags & V_ASSOC)) {
		int ok = 1;
		long n = idx && *idx ? arith_eval(idx, &ok) : 0;
		if (n < 0) {
			long count = 0;
			AElem *it;
			for (it = v->elems; it; it = it->next)
				if (it->idx > count)
					count = it->idx;
			n = count + 1 + n;
			if (n < 0)
				n = 0;
		}
		snprintf(key, sizeof key, "%ld", n);
		idx = key;
	}
	v->flags &= ~V_UNSET;
	e = elem_find(v, idx);
	if (!e)
		e = elem_insert(v, idx);
	if (append && e->val) {
		char *joined = xasprintf("%s%s", e->val, val ? val : "");
		free(e->val);
		e->val = joined;
	} else {
		free(e->val);
		e->val = apply_case(val, v->flags);
	}
	/* index 0 doubles as the scalar value, the way bash does it */
	if (!(v->flags & V_ASSOC) && strcmp(e->key, "0") == 0) {
		free(v->val);
		v->val = xstrdup(e->val);
	}
}

const char *var_array_get(const char *name, const char *idx)
{
	Var *v = var_find(name);
	AElem *e;
	char key[32];

	if (!v)
		return NULL;
	if (!(v->flags & (V_ARRAY | V_ASSOC))) {
		int ok = 1;
		long n = idx && *idx ? arith_eval(idx, &ok) : 0;
		return n == 0 ? v->val : NULL;
	}
	if (!(v->flags & V_ASSOC)) {
		int ok = 1;
		long n = idx && *idx ? arith_eval(idx, &ok) : 0;
		if (n < 0) {
			long count = 0;
			AElem *it;
			for (it = v->elems; it; it = it->next)
				if (it->idx > count)
					count = it->idx;
			n = count + 1 + n;
		}
		snprintf(key, sizeof key, "%ld", n);
		idx = key;
	}
	e = elem_find(v, idx);
	return e ? e->val : NULL;
}

void var_array_unset(const char *name, const char *idx)
{
	Var *v = var_find(name);
	AElem **p;
	char key[32];

	if (!v)
		return;
	if (!(v->flags & V_ASSOC)) {
		int ok = 1;
		snprintf(key, sizeof key, "%ld", idx && *idx ? arith_eval(idx, &ok) : 0);
		idx = key;
	}
	for (p = &v->elems; *p; p = &(*p)->next) {
		if (strcmp((*p)->key, idx) == 0) {
			AElem *e = *p;
			*p = e->next;
			e->next = NULL;
			elem_free(e);
			return;
		}
	}
}

void var_array_clear(const char *name)
{
	Var *v = var_find(name);

	if (!v)
		return;
	elem_free(v->elems);
	v->elems = NULL;
	free(v->val);
	v->val = NULL;
}

void var_array_keys(const char *name, Vec *out)
{
	Var *v = var_find(name);
	AElem *e;

	if (!v)
		return;
	if (!(v->flags & (V_ARRAY | V_ASSOC))) {
		if (v->val)
			vec_pushs(out, "0");
		return;
	}
	for (e = v->elems; e; e = e->next)
		vec_pushs(out, e->key);
}

void var_array_values(const char *name, Vec *out)
{
	Var *v = var_find(name);
	AElem *e;

	if (!v)
		return;
	if (!(v->flags & (V_ARRAY | V_ASSOC))) {
		if (v->val)
			vec_pushs(out, v->val);
		return;
	}
	for (e = v->elems; e; e = e->next)
		vec_pushs(out, e->val ? e->val : "");
}

size_t var_array_count(const char *name)
{
	Var *v = var_find(name);
	AElem *e;
	size_t n = 0;

	if (!v)
		return 0;
	if (!(v->flags & (V_ARRAY | V_ASSOC)))
		return v->val ? 1 : 0;
	for (e = v->elems; e; e = e->next)
		n++;
	return n;
}

char *var_render_decl(const Var *v, const char *prefix)
{
	Buf b;
	char flags[16];
	size_t fi = 0;

	buf_init(&b);
	if (v->flags & V_ASSOC)
		flags[fi++] = 'A';
	else if (v->flags & V_ARRAY)
		flags[fi++] = 'a';
	if (v->flags & V_INTEGER)
		flags[fi++] = 'i';
	if (v->flags & V_LOWER)
		flags[fi++] = 'l';
	if (v->flags & V_UPPER)
		flags[fi++] = 'u';
	if (v->flags & V_READONLY)
		flags[fi++] = 'r';
	if (v->flags & V_TRACE)
		flags[fi++] = 't';
	if (v->flags & V_EXPORT)
		flags[fi++] = 'x';
	flags[fi] = '\0';

	buf_printf(&b, "%s -%s %s", prefix, fi ? flags : "-", v->name);
	if (v->flags & (V_ARRAY | V_ASSOC)) {
		AElem *e;
		buf_puts(&b, "=(");
		for (e = v->elems; e; e = e->next) {
			char *q = shell_quote(e->val ? e->val : "");
			if (e != v->elems)
				buf_putc(&b, ' ');
			if (v->flags & V_ASSOC) {
				char *k = shell_quote(e->key);
				buf_printf(&b, "[%s]=%s", k, q);
				free(k);
			} else {
				buf_printf(&b, "[%s]=%s", e->key, q);
			}
			free(q);
		}
		buf_putc(&b, ')');
	} else if (!(v->flags & V_UNSET) && v->val) {
		char *q = shell_quote(v->val);
		buf_printf(&b, "=%s", q);
		free(q);
	}
	return buf_take(&b);
}

/* -------------------------------------------------------------- positionals */

void args_set(Vec *args)
{
	vec_free(&positional);
	positional = *args;
	vec_init(args);
}

void args_shift(int n)
{
	while (n-- > 0 && positional.len)
		free(vec_remove(&positional, 0));
}

size_t args_count(void)
{
	return positional.len;
}

const char *args_get(size_t i)
{
	if (i == 0 || i > positional.len)
		return NULL;
	return positional.v[i - 1];
}

Vec *args_vec(void)
{
	return &positional;
}

/* --------------------------------------------------------------------- init */

void vars_init(char **envp)
{
	char cwd[4096];
	const char *p;
	int i;

	shell_start = time(NULL);
	rand_state = (unsigned long)getpid() * 2654435761u + (unsigned long)time(NULL);
	vec_init(&positional);

	for (i = 0; envp && envp[i]; i++) {
		char *eq = strchr(envp[i], '=');
		char *name;

		if (!eq)
			continue;
		name = xstrndup(envp[i], (size_t)(eq - envp[i]));
		if (is_valid_name(name))
			var_set_global(name, eq + 1, V_EXPORT);
		free(name);
	}

	if (getcwd(cwd, sizeof cwd))
		var_set_global("PWD", cwd, V_EXPORT);
	var_set_global("IFS", " \t\n", 0);
	var_set_global("PS1", "\\u@\\h \\W \\$ ", 0);
	var_set_global("PS2", "> ", 0);
	var_set_global("PS4", "+ ", 0);
	var_set_global("CRISH_VERSION", CRISH_VERSION, 0);
	var_set_global("SHELL_TYPE", "crish", 0);

	{
		char buf[32];
		snprintf(buf, sizeof buf, "%ld", (long)getuid());
		var_set_global("UID", buf, 0);
		snprintf(buf, sizeof buf, "%ld", (long)geteuid());
		var_set_global("EUID", buf, 0);
		snprintf(buf, sizeof buf, "%ld", (long)getppid());
		var_set_global("PPID", buf, 0);
	}
	p = var_get("SHLVL");
	{
		long lvl = p ? strtol(p, NULL, 10) : 0;
		char buf[32];
		snprintf(buf, sizeof buf, "%ld", lvl + 1);
		var_set_global("SHLVL", buf, V_EXPORT);
	}
	if (!var_get("PATH"))
		var_set_global("PATH", "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", V_EXPORT);
	if (!var_get("HOSTNAME")) {
		char host[256];
		if (gethostname(host, sizeof host) == 0) {
			char *dot = strchr(host, '.');
			if (dot)
				*dot = '\0';
			var_set_global("HOSTNAME", host, 0);
		}
	}
	{
		char buf[32];
		snprintf(buf, sizeof buf, "%ld", (long)getpid());
		var_set_global("$", buf, 0);
	}
	/* declare the computed variables so `set` lists them */
	var_declare("RANDOM", V_SPECIAL);
	var_declare("SECONDS", V_SPECIAL);
	var_declare("EPOCHSECONDS", V_SPECIAL);
	var_declare("EPOCHREALTIME", V_SPECIAL);
	var_declare("SRANDOM", V_SPECIAL);
	var_declare("LINENO", V_SPECIAL);
}
