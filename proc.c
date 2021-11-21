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

static void	proc_split_cmdline(char *, char **, size_t);

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

	if (buf->proc != NULL) {
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
		close(out_pipe[0]);
		close(out_pipe[1]);
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
		printf("failed to execute '%s': %s\n", copy, errno_s);
		exit(1);
	}

	close(out_pipe[1]);

	if (add)
		ce_hist_add(copy);

	free(copy);

	if ((buf->proc = calloc(1, sizeof(struct ceproc))) == NULL)
		fatal("%s: calloc: %s", __func__, errno_s);

	buf->proc->cnt = 0;
	buf->proc->first = 1;
	buf->proc->buf = buf;
	buf->proc->pid = pid;
	buf->proc->idx = buf->lcnt;
	buf->proc->ofd = out_pipe[0];
	buf->proc->cmd = ce_strdup(cmd);
	buf->proc->flags = CE_PROC_AUTO_SCROLL;

	for (idx = 0; noscroll[idx] != NULL; idx++) {
		if (!strcmp(noscroll[idx], buf->proc->cmd)) {
			buf->proc->flags = 0;
			break;
		}
	}

	buf->selexec.set = 1;
	buf->selexec.line = buf->lcnt;

	if (fcntl(buf->proc->ofd, F_GETFL, &flags) == -1)
		fatal("%s: fcntl(get): %s", __func__, errno_s);

	flags |= O_NONBLOCK;

	if (fcntl(buf->proc->ofd, F_SETFL, &flags) == -1)
		fatal("%s: fcntl(set): %s", __func__, errno_s);
}

void
ce_proc_kill(struct ceproc *proc)
{
	if (proc == NULL)
		return;

	if (kill(proc->pid, SIGKILL) == -1) {
		ce_editor_message("failed to kill proc: %s\n", errno_s);
	} else {
		ce_proc_reap(proc);
		ce_editor_message("buffer process killed");
	}
}

void
ce_proc_read(struct ceproc *proc)
{
	ssize_t		ret, idx;
	u_int8_t	data[4096];

	ret = read(proc->ofd, data, sizeof(data));
	if (ret == -1) {
		if (errno == EINTR)
			return;
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		fatal("%s: read: %s", __func__, errno_s);
	}

	proc->cnt += ret;

	if (ret == 0) {
		ce_proc_reap(proc);
		return;
	}

	for (idx = 0; idx < ret; idx++) {
		if (data[idx] == '\n') {
			ce_buffer_appendl(proc->buf, data, idx + 1);
			memmove(&data[0], &data[idx + 1], ret - idx - 1);
			ret -= idx + 1;
			idx = -1;
		}
	}

	if (ret > 0)
		ce_buffer_appendl(proc->buf, data, ret);

	if (proc->first) {
		proc->first = 0;
		ce_buffer_center_line(proc->buf, proc->idx - 1);
		ce_buffer_top();
		if (ce_editor_mode() == CE_EDITOR_MODE_NORMAL &&
		    ce_buffer_active() != proc->buf)
			ce_buffer_activate(proc->buf);
	} else if (proc->flags & CE_PROC_AUTO_SCROLL) {
		ce_buffer_jump_line(proc->buf, proc->buf->lcnt, 0);
	}

	ce_editor_dirty();
}

void
ce_proc_reap(struct ceproc *proc)
{
	pid_t			pid;
	char			str[80];
	int			len, status;

	if (proc == NULL)
		return;

	proc->buf->proc = NULL;

	for (;;) {
		pid = waitpid(proc->pid, &status, 0);
		if (pid == -1) {
			if (errno == EINTR)
				continue;
			fatal("%s: waitpid: %s", __func__, errno_s);
		}

		break;
	}

	close(proc->ofd);

	if (WIFEXITED(status)) {
		len = snprintf(str, sizeof(str),
		    "%s exited with %d",
		    proc->cmd, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
		len = snprintf(str, sizeof(str),
		    "%s aborted due to signal %d",
		    proc->cmd, WSTOPSIG(status));
	} else {
		len = snprintf(str, sizeof(str),
		    "%s exited with status %d", proc->cmd, status);
	}

	if (len == -1 || (size_t)len >= sizeof(str))
		fatal("%s: failed to construct status buf", __func__);

	ce_editor_settings(proc->buf);
	ce_editor_message(str);

	ce_editor_dirty();

	free(proc->cmd);
	free(proc);
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
