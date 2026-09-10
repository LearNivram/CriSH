/* shell.h - the syntax tree and the shell's global state.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CRISH_SHELL_H
#define CRISH_SHELL_H

#include <setjmp.h>
#include <signal.h>
#include <sys/types.h>

#include "util.h"

#define CRISH_VERSION "0.1.0"

/* ------------------------------------------------------------------ words */

/* A word carries its source text with quotes intact; quoting is interpreted
 * during expansion so that "$a" and $a can share one representation. */
typedef struct {
	char *text;
} Word;

/* Expansion produces characters plus one flag byte per character.  The flags
 * survive until field splitting and globbing are done, then are dropped. */
#define XF_QUOTED 0x01 /* came from quotes: no splitting, no globbing */
#define XF_SPLIT  0x02 /* a forced field break between "$@" elements */
#define XF_EMPTY  0x04 /* zero-width marker: a quoted run happened here */

typedef struct {
	Buf s;
	Buf f;
} XBuf;

void xbuf_init(XBuf *x);
void xbuf_free(XBuf *x);
void xbuf_putc(XBuf *x, int c, unsigned flag);
void xbuf_puts(XBuf *x, const char *s, unsigned flag);
void xbuf_put(XBuf *x, const char *s, size_t n, unsigned flag);

/* ------------------------------------------------------------ redirections */

typedef enum {
	R_IN,        /* < file        */
	R_OUT,       /* > file        */
	R_APPEND,    /* >> file       */
	R_CLOBBER,   /* >| file       */
	R_RDWR,      /* <> file       */
	R_DUP_IN,    /* <&n  <&-      */
	R_DUP_OUT,   /* >&n  >&-      */
	R_HEREDOC,   /* << word       */
	R_HERESTR,   /* <<< word      */
	R_ALL_OUT,   /* &> file       */
	R_ALL_APPEND /* &>> file      */
} RedirType;

typedef struct Redir {
	RedirType type;
	int fd;          /* the descriptor being redirected, -1 for the default */
	char *word;      /* target text, or heredoc body */
	char *varname;   /* {name}> form, else NULL */
	int here_quoted; /* heredoc delimiter was quoted: no expansion */
	struct Redir *next;
} Redir;

/* -------------------------------------------------------------------- tree */

typedef enum {
	N_SIMPLE,
	N_PIPE,
	N_SEQ,
	N_AND,
	N_OR,
	N_NOT,
	N_SUBSHELL,
	N_GROUP,
	N_IF,
	N_WHILE,
	N_UNTIL,
	N_FOR,
	N_FOR_ARITH,
	N_CASE,
	N_SELECT,
	N_FUNC,
	N_ARITH, /* (( ... )) */
	N_COND,  /* [[ ... ]] */
	N_TIME
} NodeType;

typedef struct CaseItem {
	Vec patterns;      /* raw pattern words, unexpanded */
	struct Node *body; /* may be NULL for an empty branch */
	int fallthrough;   /* ;& runs the next body, ;;& retests from the next */
	int retest;
	struct CaseItem *next;
} CaseItem;

typedef struct Node {
	NodeType type;

	Vec words;   /* N_SIMPLE: command words, unexpanded */
	Vec assigns; /* N_SIMPLE: leading NAME=..., NAME+=..., NAME=(...) */
	Redir *redir;

	struct Node **kids; /* N_PIPE, N_SEQ */
	int *kidflags;      /* N_PIPE: 1 when the stage was joined with |& */
	                    /* N_SEQ:  1 when the element ends in & */
	size_t nkids;

	struct Node *left;  /* N_AND/N_OR left, N_NOT body, condition of if/while */
	struct Node *right; /* N_AND/N_OR right */
	struct Node *body;  /* loop or branch body */
	struct Node *els;   /* else branch */

	char *name;   /* N_FUNC name, N_FOR/N_SELECT variable */
	Vec items;    /* N_FOR/N_SELECT word list; empty means "$@" */
	int no_items; /* for x; do ... : iterate over "$@" */

	char *a1, *a2, *a3; /* N_FOR_ARITH sections, N_ARITH expression */
	char *word;         /* N_CASE subject */
	CaseItem *cases;

	Vec cond; /* N_COND: the raw tokens between [[ and ]] */

	int lineno;
} Node;

Node *node_new(NodeType type);
void node_free(Node *n);

/* ------------------------------------------------------------------ parser */

typedef struct Parser Parser;

/* Parse one complete command from src.  Returns NULL at end of input.
 * *err is set to a message on a syntax error. */
Parser *parser_new(const char *src, const char *origin);
void parser_free(Parser *p);
Node *parser_next(Parser *p, char **err);
/* True when the parser stopped because the input ended mid-construct. */
int parser_incomplete(Parser *p);

/* Parse an entire string into one N_SEQ; convenience for eval/source/$(). */
Node *parse_string(const char *src, const char *origin, char **err);

/* --------------------------------------------------------------- variables */

#define V_EXPORT   0x0001
#define V_READONLY 0x0002
#define V_INTEGER  0x0004
#define V_LOWER    0x0008
#define V_UPPER    0x0010
#define V_ARRAY    0x0020
#define V_ASSOC    0x0040
#define V_NAMEREF  0x0080
#define V_SPECIAL  0x0100 /* value is computed on read (SECONDS, RANDOM, ...) */
#define V_UNSET    0x0200 /* declared without a value */
#define V_TRACE    0x0400

typedef struct AElem {
	char *key; /* index rendered as text, or the associative key */
	long idx;  /* numeric index for V_ARRAY */
	char *val;
	struct AElem *next;
} AElem;

typedef struct Var {
	char *name;
	char *val;     /* scalar value */
	AElem *elems;  /* array elements, ordered by index for V_ARRAY */
	unsigned flags;
	struct Var *next;
} Var;

typedef struct Scope {
	Var *vars;
	struct Scope *parent;
} Scope;

void vars_init(char **envp);
Var *var_find(const char *name);
const char *var_get(const char *name);
void var_set(const char *name, const char *val, unsigned add_flags);
void var_set_global(const char *name, const char *val, unsigned add_flags);
void var_unset(const char *name);
void var_export(const char *name, int on);
void var_make_local(const char *name);
char **vars_environ(void);
void vars_push_scope(void);
void vars_pop_scope(void);
/* Array access.  idx is the rendered subscript for associative arrays. */
void var_array_set(const char *name, const char *idx, const char *val, int append);
const char *var_array_get(const char *name, const char *idx);
void var_array_unset(const char *name, const char *idx);
void var_array_clear(const char *name);
void var_array_keys(const char *name, Vec *out);
void var_array_values(const char *name, Vec *out);
size_t var_array_count(const char *name);
void var_declare(const char *name, unsigned flags);
void vars_all(Vec *out);
/* Rendered "declare -x NAME=value" form used by declare -p and ${v@A}. */
char *var_render_decl(const Var *v, const char *prefix);

/* ------------------------------------------------------------- positionals */

void args_set(Vec *args); /* takes ownership of the contents */
void args_shift(int n);
size_t args_count(void);
const char *args_get(size_t i); /* 1-based */
Vec *args_vec(void);

/* -------------------------------------------------------------- shell state */

typedef struct Func {
	char *name;
	Node *body;
	struct Func *next;
} Func;

typedef struct Alias {
	char *name;
	char *value;
	struct Alias *next;
} Alias;

/* set -o options */
typedef struct {
	int errexit;
	int nounset;
	int xtrace;
	int verbose;
	int noglob;
	int noexec;
	int pipefail;
	int monitor;
	int notify;
	int noclobber;
	int allexport;
	int emacs;
	int vi;
	int posix;
	int privileged;
	int onecmd;
	int hashall;
	int histexpand;
	int history;
	int ignoreeof;
	int keyword;
} Options;

/* shopt options */
typedef struct {
	int globstar;
	int nullglob;
	int failglob;
	int dotglob;
	int extglob;
	int nocasematch;
	int nocaseglob;
	int inherit_errexit;
	int lastpipe;
	int expand_aliases;
	int checkwinsize;
	int cmdhist;
	int histappend;
	int autocd;
	int cdspell;
	int gnu_builtins; /* CriSH: use the built-in GNU utilities */
	int gnu_warn;     /* warn when a built-in shadows a PATH binary */
	int xpg_echo;
	int huponexit;
	int progcomp;
	int sourcepath;
	int interactive_comments;
} Shopt;

typedef struct Shell {
	const char *argv0;
	int interactive;
	int login;
	int reading_stdin;
	int last_status;
	int exit_requested;
	int exit_code;
	pid_t pid;
	pid_t last_bg_pid;
	int depth;         /* nesting of function calls */
	int subshell;      /* non-zero inside a forked subshell */
	int loop_depth;
	int break_count;   /* pending break levels */
	int continue_count;
	int returning;     /* a return builtin fired */
	int return_value;
	int in_function;
	Options opt;
	Shopt shopt;
	Func *funcs;
	Alias *aliases;
	Vec dirstack;
	const char *script_name;
	int lineno;
	char *tmpdir;
} Shell;

extern Shell sh;

void shell_init(int argc, char **argv, char **envp);
void shell_error(const char *fmt, ...);
void shell_fatal(const char *fmt, ...);
/* Report a syntax or runtime error the way bash does and, in a script,
 * make the shell exit. */
void shell_error_at(const char *where, const char *fmt, ...);

Func *func_find(const char *name);
void func_define(const char *name, Node *body);
void func_undefine(const char *name);

const char *alias_get(const char *name);
void alias_set(const char *name, const char *value);
void alias_unset(const char *name);
void alias_list(Vec *out);

/* -------------------------------------------------------------- execution */

int exec_node(Node *n);
int exec_string(const char *src, const char *origin);
int exec_file(const char *path, Vec *args);
/* Run n with output captured; used by $(...) and process substitution. */
char *exec_capture(Node *n, int *status);
char *capture_string(const char *src, int *status);

/* Find a command on PATH; returns an owned string or NULL. */
char *path_lookup(const char *name);
void path_hash_clear(void);

/* Longjmp target used by exit/return inside builtins. */
extern sigjmp_buf *exec_jmp;

/* --------------------------------------------------------------- builtins */

typedef int (*BuiltinFn)(int argc, char **argv);

typedef struct {
	const char *name;
	BuiltinFn fn;
	const char *usage;
	const char *summary;
} Builtin;

const Builtin *builtin_find(const char *name);
const Builtin *builtin_table(size_t *count);

/* GNU-compatible utilities that live inside the binary. */
typedef int (*GnuFn)(int argc, char **argv);
typedef struct {
	const char *name;
	GnuFn fn;
	const char *summary;
} GnuTool;

const GnuTool *gnu_find(const char *name);
const GnuTool *gnu_table(size_t *count);

/* --------------------------------------------------------------- expansion */

/* Expand one word into zero or more fields. */
void expand_word(const char *text, Vec *out);
/* Expand for a context that does not split or glob (assignments, heredocs). */
char *expand_to_string(const char *text);
/* Expand exactly like a here-string / case subject: quotes removed, no split. */
char *expand_unsplit(const char *text);
/* Expand a whole word list. */
void expand_words(const Vec *in, Vec *out);
/* Expand for pattern matching: quoted metacharacters come back escaped. */
char *expand_pattern(const char *text);
char *expand_regex(const char *text);
/* Remove quotes without performing any expansion. */
char *unquote(const char *text);

long arith_eval(const char *expr, int *ok);

/* Pattern matching (glob syntax, not regex). */
int glob_match(const char *pattern, const char *string, int nocase);
/* Expand a glob pattern against the file system; returns the match count. */
size_t glob_expand(const char *pattern, Vec *out);

/* ---------------------------------------------------------------- redirect */

typedef struct SavedFds SavedFds;
SavedFds *redir_apply(Redir *r, int *ok);
void redir_restore(SavedFds *s);

/* -------------------------------------------------------------------- jobs */

void jobs_init(void);
int job_add(pid_t pgid, const char *cmd, int background);
int job_wait(pid_t pid, int *status);
void jobs_notify(void);
void jobs_list(int mode);
int job_fg(const char *spec);
int job_bg(const char *spec);
void jobs_reap(void);
int jobs_count_running(void);

/* ------------------------------------------------------------------- traps */

void trap_set(int sig, const char *action);
const char *trap_get(int sig);
void trap_run_pending(void);
void trap_run_exit(void);
int trap_signal_number(const char *name);
const char *trap_signal_name(int sig);
void traps_reset_in_child(void);
extern volatile sig_atomic_t trap_pending;

/* ------------------------------------------------------- interactive layer */

char *line_read(const char *prompt, const char *prompt2);
void line_init(void);
void line_cleanup(void);

void history_init(void);
void history_add(const char *line);
void history_save(void);
size_t history_count(void);
const char *history_get(size_t i);
void history_clear(void);

char *prompt_render(const char *format);

void config_load(void);

int cmd_update(int argc, char **argv);

#endif /* CRISH_SHELL_H */
