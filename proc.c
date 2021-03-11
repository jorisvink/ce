/*
 * Copyright (c) 2021 Joris Vink <joris@coders.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "ce.h"

struct proc {
	pid_t			pid;
	int			ifd;
	int			ofd;
	size_t			idx;
	struct cebuf		*buf;
};

static void	proc_split_cmdline(char *, char **, size_t);

/* The only running background process. */
static struct proc	*active = NULL;

void
ce_proc_run(char *cmd, struct cebuf *buf)
{
	pid_t		pid;
	char		*argv[32];
	int		flags, idx, in_pipe[2], out_pipe[2];

	if (active != NULL) {
		ce_editor_message("execute failed, another proc is pending");
		return;
	}

	if (pipe(in_pipe) == -1) {
		ce_editor_message("%s: pipe: %s", __func__, errno_s);
		return;
	}

	if (pipe(out_pipe) == -1) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		ce_editor_message("%s: pipe: %s", __func__, errno_s);
		return;
	}

	if ((pid = fork()) == -1) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		ce_editor_message("failed to run '%s': %s", cmd, errno_s);
		return;
	}

	if (pid == 0) {
		close(in_pipe[1]);
		close(out_pipe[0]);

		if (dup2(out_pipe[1], STDOUT_FILENO) == -1 ||
		    dup2(out_pipe[1], STDERR_FILENO) == -1 ||
		    dup2(in_pipe[0], STDIN_FILENO) == -1)
			fatal("dup2: %s", errno_s);

		proc_split_cmdline(cmd, argv, 32);

		for (idx = 0; argv[idx] != NULL; idx++)
			ce_debug("%s", argv[idx]);

		execvp(argv[0], argv);
		ce_debug("execvp: %s", errno_s);

		exit(1);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);

	if ((active = calloc(1, sizeof(*active))) == NULL)
		fatal("%s: calloc: %s", __func__, errno_s);

	active->buf = buf;
	active->pid = pid;
	active->ifd = in_pipe[1];
	active->ofd = out_pipe[0];

	if (fcntl(active->ofd, F_GETFL, &flags) == -1)
		fatal("%s: fcntl(get): %s", __func__, errno_s);

	flags |= O_NONBLOCK;

	if (fcntl(active->ofd, F_SETFL, &flags) == -1)
		fatal("%s: fcntl(set): %s", __func__, errno_s);

	ce_buffer_appendl(buf, " ", 1);
	active->idx = buf->lcnt;
	ce_buffer_jump_line(buf, buf->lcnt, TERM_CURSOR_MIN);

	ce_debug("proc %d started", active->pid);
}

int
ce_proc_stdout(void)
{
	return (active ? active->ofd : -1);
}

void
ce_proc_read(void)
{
	int		iter;
	ssize_t		ret, idx;
	u_int8_t	buf[4096];

	if (active == NULL)
		fatal("%s: called without active proc", __func__);

	for (iter = 0; iter < 5; iter++) {
		ret = read(active->ofd, buf, sizeof(buf));
		if (ret == -1) {
			if (errno == EINTR)
				return;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			fatal("%s: read: %s", __func__, errno_s);
		}

		ce_debug("read %zd bytes from process", ret);
		ce_editor_set_pasting(1);

		if (ce_buffer_active() != active->buf)
			ce_buffer_activate(active->buf);

		for (idx = 0; idx < ret; idx++)
			ce_buffer_input(active->buf, buf[idx]);

		if (ret == 0) {
			ce_proc_reap();
			break;
		}
	}

	ce_debug("reading break");
}

void
ce_proc_reap(void)
{
	pid_t		pid;
	char		buf[80];
	int		len, status;

	if (active == NULL)
		return;

	for (;;) {
		pid = waitpid(active->pid, &status, 0);
		if (pid == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: waitpid: %s", __func__, errno_s);
		}

		break;
	}

	ce_debug("proc %d completed with %d", pid, status);

	close(active->ofd);
	close(active->ifd);

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) != 0) {
			len = snprintf(buf, sizeof(buf),
			    "program exited with %d\n", WEXITSTATUS(status));
		} else {
			len = 0;
		}
	} else if (WIFSIGNALED(status)) {
		len = snprintf(buf, sizeof(buf),
		    "program exited with signal %d\n", WSTOPSIG(status));
	} else {
		len = snprintf(buf, sizeof(buf),
		    "program exited with %d\n", status);
	}

	if (len == -1 || (size_t)len >= sizeof(buf))
		fatal("%s: failed to construct status buf", __func__);

	if (len > 0)
		ce_buffer_appendl(active->buf, buf, len);

	ce_buffer_appendl(active->buf, " ", 1);
	ce_buffer_jump_line(active->buf, active->idx, TERM_CURSOR_MIN);
	ce_editor_dirty();

	free(active);
	active = NULL;

	ce_editor_set_pasting(0);
}

static void
proc_split_cmdline(char *args, char **argv, size_t elm)
{
	size_t		idx;
	char		*p, *line, *end;

	if (elm <= 1)
		fatal("not enough elements (%zu)", elm);

	idx = 0;
	line = args;

	for (p = line; *p != '\0'; p++) {
		if (idx >= elm - 1)
			break;

		if (*p == ' ') {
			*p = '\0';
			if (*line != '\0')
				argv[idx++] = line;
			line = p + 1;
			continue;
		}

		if (*p != '"')
			continue;

		line = p + 1;
		if ((end = strchr(line, '"')) == NULL)
			break;

		*end = '\0';
		argv[idx++] = line;
		line = end + 1;

		while (isspace(*(unsigned char *)line))
			line++;

		p = line;
	}

	if (idx < elm - 1 && *line != '\0')
		argv[idx++] = line;

	argv[idx] = NULL;
}
