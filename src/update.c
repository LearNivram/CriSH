/* update.c - `crish update`, the self-updater.
 *
 * The GitHub API and the download go through curl, which every Mac has; the
 * checksum check, the version compare and the atomic replacement are ours.
 *
 * Copyright (C) 2026 The CriSH authors.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "shell.h"

#define CRISH_REPO "LearNivram/CriSH"
#define ASSET_NAME "crish-macos-universal"

char *sha256_file(const char *path); /* gnu/hash.c */

/* Run a command and collect its standard output. */
static char *run_capture(const char *cmd, int *status)
{
	FILE *f = popen(cmd, "r");
	Buf b;
	char chunk[4096];
	size_t n;
	int rc;

	if (!f) {
		if (status)
			*status = -1;
		return NULL;
	}
	buf_init(&b);
	while ((n = fread(chunk, 1, sizeof chunk, f)) > 0)
		buf_put(&b, chunk, n);
	rc = pclose(f);
	if (status)
		*status = rc;
	return buf_take(&b);
}

/* Minimal JSON string field reader: "key": "value". */
static char *json_string(const char *json, const char *key)
{
	char needle[64];
	const char *p;

	snprintf(needle, sizeof needle, "\"%s\"", key);
	p = strstr(json, needle);
	if (!p)
		return NULL;
	p += strlen(needle);
	while (*p == ' ' || *p == ':')
		p++;
	if (*p != '"')
		return NULL;
	p++;
	{
		Buf b;

		buf_init(&b);
		while (*p && *p != '"') {
			if (*p == '\\' && p[1]) {
				p++;
				switch (*p) {
				case 'n': buf_putc(&b, '\n'); break;
				case 't': buf_putc(&b, '\t'); break;
				case 'r': buf_putc(&b, '\r'); break;
				default: buf_putc(&b, *p); break;
				}
				p++;
				continue;
			}
			buf_putc(&b, *p++);
		}
		return buf_take(&b);
	}
}

/* Compare dotted versions; returns <0, 0 or >0. */
static int version_cmp(const char *a, const char *b)
{
	while (*a == 'v')
		a++;
	while (*b == 'v')
		b++;
	for (;;) {
		long x = strtol(a, (char **)&a, 10);
		long y = strtol(b, (char **)&b, 10);

		if (x != y)
			return x < y ? -1 : 1;
		if (*a == '.')
			a++;
		else if (*b != '.')
			return 0;
		if (*b == '.')
			b++;
		if (!*a && !*b)
			return 0;
	}
}

static char *self_path(void)
{
	char buf[4096];
	uint32_t size = sizeof buf;
	char real[4096];

	if (_NSGetExecutablePath(buf, &size) != 0)
		return NULL;
	if (realpath(buf, real))
		return xstrdup(real);
	return xstrdup(buf);
}

/* Fetch the releases list and pick one. */
static char *fetch_releases(int prereleases)
{
	char cmd[512];
	int status = 0;
	char *json;

	snprintf(cmd, sizeof cmd,
		 "curl -fsSL -H 'Accept: application/vnd.github+json' "
		 "'https://api.github.com/repos/%s/releases%s' 2>/dev/null",
		 CRISH_REPO, prereleases ? "?per_page=30" : "/latest");
	json = run_capture(cmd, &status);
	if (status != 0 || !json || !*json) {
		free(json);
		return NULL;
	}
	return json;
}

static int install_release(const char *tag, const char *url)
{
	char *tmpdir = xstrdup("/tmp/crish-update-XXXXXX");
	char *binary, *sums, *cmd, *target, *staged;
	char *want = NULL, *have = NULL;
	int rc = 1;
	int status = 0;

	if (!mkdtemp(tmpdir)) {
		shell_error("update: cannot create a temporary directory: %s", strerror(errno));
		free(tmpdir);
		return 1;
	}
	binary = xasprintf("%s/%s", tmpdir, ASSET_NAME);
	sums = xasprintf("%s/checksums.txt", tmpdir);
	target = self_path();
	if (!target) {
		shell_error("update: cannot locate the running binary");
		goto out;
	}

	printf("crish: downloading %s\n", tag);
	cmd = xasprintf("curl -fsSL -o '%s' '%s'", binary, url);
	free(run_capture(cmd, &status));
	free(cmd);
	if (status != 0) {
		shell_error("update: download failed");
		goto out;
	}

	cmd = xasprintf("curl -fsSL -o '%s' "
			"'https://github.com/%s/releases/download/%s/checksums.txt' 2>/dev/null",
			sums, CRISH_REPO, tag);
	free(run_capture(cmd, &status));
	free(cmd);

	have = sha256_file(binary);
	if (status == 0) {
		FILE *f = fopen(sums, "r");
		char line[512];

		if (f) {
			while (fgets(line, sizeof line, f)) {
				if (strstr(line, ASSET_NAME)) {
					char *sp = strchr(line, ' ');
					if (sp) {
						*sp = '\0';
						want = xstrdup(line);
					}
					break;
				}
			}
			fclose(f);
		}
	}
	if (want && have && strcmp(want, have) != 0) {
		shell_error("update: checksum mismatch");
		shell_error("  expected %s", want);
		shell_error("  got      %s", have);
		goto out;
	}
	if (!want)
		printf("crish: no checksums.txt in the release, sha256 is %s\n",
		       have ? have : "?");

	chmod(binary, 0755);

	/* Replace ourselves atomically: rename inside the same directory. */
	staged = xasprintf("%s.new", target);
	cmd = xasprintf("cp '%s' '%s'", binary, staged);
	free(run_capture(cmd, &status));
	free(cmd);
	if (status != 0) {
		shell_error("update: cannot write next to %s (try sudo)", target);
		free(staged);
		goto out;
	}
	chmod(staged, 0755);
	if (rename(staged, target) != 0) {
		shell_error("update: cannot replace %s: %s", target, strerror(errno));
		unlink(staged);
		free(staged);
		goto out;
	}
	free(staged);
	printf("crish: updated to %s\n", tag);
	rc = 0;

out:
	{
		char *rm = xasprintf("rm -rf '%s'", tmpdir);
		int ignored = system(rm);
		(void)ignored;
		free(rm);
	}
	free(binary);
	free(sums);
	free(target);
	free(want);
	free(have);
	free(tmpdir);
	return rc;
}

int cmd_update(int argc, char **argv)
{
	int check_only = 0, prereleases = 0, selector = 0;
	int i;
	char *json, *tag, *url;
	int rc;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--check") == 0)
			check_only = 1;
		else if (strcmp(argv[i], "--pre") == 0)
			prereleases = 1;
		else if (strcmp(argv[i], "--selector") == 0)
			selector = 1;
		else if (strcmp(argv[i], "--help") == 0) {
			printf("usage: crish update [--check] [--pre] [--selector]\n"
			       "  --check     only report whether a newer release exists\n"
			       "  --pre       consider prereleases too\n"
			       "  --selector  choose from the list of releases\n");
			return 0;
		} else {
			shell_error("update: %s: unknown option", argv[i]);
			return 2;
		}
	}

	json = fetch_releases(prereleases || selector);
	if (!json) {
		shell_error("update: cannot reach the GitHub API");
		return 1;
	}

	tag = json_string(json, "tag_name");
	if (!tag) {
		shell_error("update: no release found");
		free(json);
		return 1;
	}

	if (selector) {
		/* list what is available and let the user name one */
		const char *p = json;
		Vec tags;
		size_t k;
		char line[64];

		vec_init(&tags);
		while ((p = strstr(p, "\"tag_name\""))) {
			char *t = json_string(p, "tag_name");
			if (t)
				vec_push(&tags, t);
			p += 10;
		}
		for (k = 0; k < tags.len; k++)
			printf("%2zu) %s\n", k + 1, tags.v[k]);
		printf("release? ");
		fflush(stdout);
		if (fgets(line, sizeof line, stdin)) {
			size_t pick = (size_t)strtoul(line, NULL, 10);
			if (pick >= 1 && pick <= tags.len) {
				free(tag);
				tag = xstrdup(tags.v[pick - 1]);
			}
		}
		vec_free(&tags);
	}

	if (version_cmp(tag, CRISH_VERSION) <= 0 && !selector) {
		printf("crish %s is the newest release\n", CRISH_VERSION);
		free(tag);
		free(json);
		return check_only ? 1 : 0;
	}
	printf("crish %s is available (this is %s)\n", tag, CRISH_VERSION);
	if (check_only) {
		free(tag);
		free(json);
		return 0;
	}

	url = xasprintf("https://github.com/%s/releases/download/%s/%s", CRISH_REPO, tag,
			ASSET_NAME);
	rc = install_release(tag, url);
	free(url);
	free(tag);
	free(json);
	return rc;
}
