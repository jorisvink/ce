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

#include <sys/param.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "ce.h"

#define PROC_AUTO_SCROLL	(1 << 1)

struct proc {
	pid_t			pid;
	int			ofd;
	int			first;
	int			flags;
	size_t			idx;
	size_t			cnt;
	char			*cmd;
	struct cebuf		*buf;
};

static void	proc_split_cmdline(char *, char **, size_t);

/* The only running background process. */
static struct proc	*active = NULL;

/*
 * Processes where we shouldn't autoscroll.
 */
static const char *noscroll[] = {
	"rg",
	"grep",
	"git",
	"svn",
	NULL
};

void
ce_proc_run(char *cmd, struct cebuf *buf, int add)
{
	pid_t		pid;
	char		*argv[32], *copy;
	int		flags, idx, out_pipe[2];

	if (active != NULL) {
		ce_editor_message("execute failed, another proc is pending");
		return;
	}

	if (pipe(out_pipe) == -1) {
		ce_editor_message("%s: pipe: %s", __func__, errno_s);
		return;
	}

	while (isspace(*(unsigned char *)cmd))
		cmd++;

	if (*cmd == '!')
		cmd++;

	if (strlen(cmd) == 0) {
		ce_editor_message("%s: refusing empty command", __func__);
		return;
	}

	copy = ce_strdup(cmd);
	proc_split_cmdline(cmd, argv, 32);

	if ((pid = fork()) == -1) {
		close(out_pipe[0]);
		close(out_pipe[1]);
		ce_editor_message("failed to run '%s': %s", cmd, errno_s);
		return;
	}

	if (pid == 0) {
		close(out_pipe[0]);

		if (dup2(out_pipe[1], STDOUT_FILENO) == -1 ||
		    dup2(out_pipe[1], STDERR_FILENO) == -1)
			fatal("dup2: %s", errno_s);

		(void)close(STDIN_FILENO);

		execvp(argv[0], argv);
		exit(1);
	}

	close(out_pipe[1]);

	if ((active = calloc(1, sizeof(*active))) == NULL)
		fatal("%s: calloc: %s", __func__, errno_s);

	if (add)
		ce_hist_add(copy);

	free(copy);

	active->cnt = 0;
	active->first = 1;
	active->buf = buf;
	active->pid = pid;
	active->idx = buf->lcnt;
	active->ofd = out_pipe[0];

	if ((active->cmd = strdup(cmd)) == NULL)
		fatal("%s: strdup: %s", __func__, errno_s);

	active->flags = PROC_AUTO_SCROLL;
	for (idx = 0; noscroll[idx] != NULL; idx++) {
		if (!strcmp(noscroll[idx], active->cmd)) {
			active->flags = 0;
			break;
		}
	}

	buf->selexec.set = 1;
	buf->selexec.line = buf->lcnt;

	if (fcntl(active->ofd, F_GETFL, &flags) == -1)
		fatal("%s: fcntl(get): %s", __func__, errno_s);

	flags |= O_NONBLOCK;

	if (fcntl(active->ofd, F_SETFL, &flags) == -1)
		fatal("%s: fcntl(set): %s", __func__, errno_s);
}

void
ce_proc_cleanup(void)
{
	if (active == NULL)
		return;

	if (kill(active->pid, SIGKILL) == -1)
		printf("warning: failed to kill proc: %s\n", errno_s);

	ce_proc_reap();
	ce_editor_message("active process killed");
}

int
ce_proc_stdout(void)
{
	return (active ? active->ofd : -1);
}

void
ce_proc_read(void)
{
	ssize_t		ret, idx;
	u_int8_t	buf[4096];

	if (active == NULL)
		fatal("%s: called without active proc", __func__);

	ret = read(active->ofd, buf, sizeof(buf));
	if (ret == -1) {
		if (errno == EINTR)
			return;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		fatal("%s: read: %s", __func__, errno_s);
	}

	active->cnt += ret;

	if (ret == 0) {
		ce_proc_reap();
		return;
	}

	if (ce_editor_mode() == CE_EDITOR_MODE_NORMAL &&
	    ce_buffer_active() != active->buf)
		ce_buffer_activate(active->buf);

	for (idx = 0; idx < ret; idx++) {
		if (buf[idx] == '\n') {
			ce_buffer_appendl(active->buf, buf, idx + 1);
			memmove(&buf[0], &buf[idx + 1], ret - idx - 1);
			ret -= idx + 1;
			idx = -1;
		}
	}

	if (ret > 0)
		ce_buffer_appendl(active->buf, buf, ret);

	if (active->first) {
		active->first = 0;
		ce_buffer_center_line(active->buf, active->idx - 1);
		ce_buffer_top();
	} else if (active->flags & PROC_AUTO_SCROLL) {
		ce_buffer_jump_line(active->buf, active->buf->lcnt, 0);
	}

	ce_editor_dirty();
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

	close(active->ofd);

	if (WIFEXITED(status)) {
		len = snprintf(buf, sizeof(buf),
		    "%s exited with %d",
		    active->cmd, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		len = snprintf(buf, sizeof(buf),
		    "%s aborted due to signal %d",
		    active->cmd, WSTOPSIG(status));
	} else {
		len = snprintf(buf, sizeof(buf),
		    "%s exited with status %d", active->cmd, status);
	}

	if (len == -1 || (size_t)len >= sizeof(buf))
		fatal("%s: failed to construct status buf", __func__);

	ce_editor_settings(active->buf);
	ce_editor_message(buf);

	ce_editor_dirty();

	free(active->cmd);
	free(active);

	active = NULL;
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
