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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "ce.h"

#define HIST_MODE_READ		1
#define HIST_MODE_APPEND	2

static void	hist_file_read(void);
static int	hist_list_append(const char *);
static void	hist_file_append(const char *);

static struct ce_histlist	cmdhist;
static time_t			histtime = 0;
static struct cehist		*histcur = NULL;
static struct cehist		*histpos = NULL;
static struct cehist		*histmatch = NULL;
static char			*histsearch = NULL;

void
ce_hist_init(void)
{
	TAILQ_INIT(&cmdhist);
	hist_file_read();
}

void
ce_hist_add(const char *cmd)
{
	hist_list_append(cmd);
	hist_file_append(cmd);
}

void
ce_hist_autocomplete(int up)
{
	size_t			len;
	u_int8_t		*ptr;
	struct cebuf		*buf;
	struct celine		*line;
	struct cehist		*hist;

	buf = ce_buffer_active();
	line = ce_buffer_line_current(buf);

	if (line->length == 0)
		return;

	if ((hist = ce_hist_lookup(line->data, line->length - 1, up)) == NULL)
		return;

	len = strlen(hist->cmd);

	if (!(line->flags & CE_LINE_ALLOCATED))
		line->flags |= CE_LINE_ALLOCATED;
	else
		free(line->data);

	line->maxsz = len;
	line->length = len + 1;
	if ((line->data = malloc(len + 1)) == NULL)
		fatal("%s: calloc: %s", __func__, errno_s);

	memcpy(line->data, hist->cmd, len);

	ptr = line->data;
	ptr[len] = '\n';

	ce_buffer_line_columns(line);
	buf->loff = len;
	buf->column = line->columns;

	ce_editor_dirty();
}

struct cehist *
ce_hist_lookup(const void *buf, size_t len, int up)
{
	int			match;
	struct cehist		*hist;

	if (histsearch == NULL) {
		hist_file_read();
		if ((histcur = TAILQ_FIRST(&cmdhist)) == NULL)
			return (NULL);
		if ((histsearch = malloc(len + 1)) == NULL)
			fatal("%s: malloc: %s", __func__, errno_s);
		memcpy(histsearch, buf, len);
		histsearch[len] = '\0';
	}

	match = 0;
	hist = histcur;

	for (;;) {
		if (histmatch != hist && strstr(hist->cmd, histsearch)) {
			match = 1;
			histmatch = hist;
		}

		if (!up) {
			histcur = TAILQ_PREV(histcur, ce_histlist, list);
			if (histcur == NULL) {
				histcur = TAILQ_FIRST(&cmdhist);
				break;
			}
			hist = histcur;
		} else {
			histcur = TAILQ_NEXT(histcur, list);
			if (histcur == NULL) {
				histcur = hist;
				break;
			}
			hist = histcur;
		}

		if (match)
			break;
	}

	if (match)
		return (histmatch);

	return (NULL);
}

struct cehist *
ce_hist_next(void)
{
	struct cehist	*hist;

	if (TAILQ_FIRST(&cmdhist) == NULL)
		hist_file_read();

	if ((hist = TAILQ_FIRST(&cmdhist)) == NULL)
		return (NULL);

	if (histpos == NULL) {
		histpos = hist;
		return (hist);
	}

	hist = TAILQ_NEXT(histpos, list);
	if (hist == NULL)
		hist = histpos;
	else
		histpos = hist;

	return (hist);
}

struct cehist *
ce_hist_prev(void)
{
	struct cehist	*hist;

	if (TAILQ_FIRST(&cmdhist) == NULL)
		hist_file_read();

	hist = NULL;

	if (histpos != NULL) {
		hist = TAILQ_PREV(histpos, ce_histlist, list);
		if (hist == NULL)
			hist = histpos;
		else
			histpos = hist;
	}

	return (hist);
}

void
ce_hist_autocomplete_reset(struct cehist **out)
{
	if (out != NULL)
		*out = histmatch;

	free(histsearch);

	histpos = NULL;
	histcur = NULL;
	histmatch = NULL;
	histsearch = NULL;
}

static FILE *
hist_file_open(int mode)
{
	struct stat	st;
	FILE		*fp;
	char		*ftype, path[PATH_MAX];
	int		flags, ltype, len, fd, tries;

	len = snprintf(path, sizeof(path), "%s/.cehist", ce_editor_home());
	if (len == -1 || (size_t)len >= sizeof(path))
		fatal("failed to construct path to history file");

	switch (mode) {
	case HIST_MODE_APPEND:
		ftype = "a";
		ltype = LOCK_EX;
		flags = O_CREAT | O_APPEND | O_WRONLY;
		break;
	case HIST_MODE_READ:
		ftype = "r";
		ltype = LOCK_SH;
		flags = O_RDONLY;
		break;
	default:
		fatal("%s: unknown mode %d", __func__, mode);
	}

	if ((fd = open(path, flags, 0600)) == -1) {
		if (errno != ENOENT)
			ce_editor_message("can't reload histfile: %s", errno_s);
		return (NULL);
	}

	if (fstat(fd, &st) == -1) {
		ce_editor_message("histfile fstat: %s", errno_s);
		(void)close(fd);
		return (NULL);
	}

	if (mode == HIST_MODE_READ && st.st_mtime == histtime) {
		(void)close(fd);
		return (NULL);
	}

	histtime = st.st_mtime;

	tries = 0;
	while (flock(fd, ltype) == -1 && tries < 5) {
		usleep(50000);
		tries++;
	}

	if (tries == 5) {
		ce_editor_message("can't open histfile: failed to lock");
		(void)close(fd);
		return (NULL);
	}

	if ((fp = fdopen(fd, ftype)) == NULL) {
		ce_editor_message("can't open histfile: %s", errno_s);
		(void)close(fd);
		return (NULL);
	}

	return (fp);
}

static void
hist_file_read(void)
{
	FILE			*fp;
	struct cehist		*hist;
	char			*p, buf[2048];

	if ((fp = hist_file_open(HIST_MODE_READ)) == NULL)
		return;

	while ((hist = TAILQ_FIRST(&cmdhist)) != NULL) {
		TAILQ_REMOVE(&cmdhist, hist, list);
		free(hist->cmd);
		free(hist);
	}

	TAILQ_INIT(&cmdhist);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		buf[strcspn(buf, "\n")] = '\0';

		p = &buf[0];
		while (isspace(*(unsigned char *)p))
			p++;

		if (buf[0] == '0')
			continue;

		(void)hist_list_append(p);
	}

	fclose(fp);
}

void
hist_file_append(const char *cmd)
{
	FILE		*fp;
	size_t		len;

	if ((fp = hist_file_open(HIST_MODE_APPEND)) == NULL)
		return;

	len = strlen(cmd);

	if (fprintf(fp, "%s\n", cmd) != (int)len + 1)
		fatal("%s: not all data written", __func__);

	fclose(fp);
}

static int
hist_list_append(const char *cmd)
{
	struct cehist		*hist;

	TAILQ_FOREACH(hist, &cmdhist, list) {
		if (!strcmp(hist->cmd, cmd)) {
			TAILQ_REMOVE(&cmdhist, hist, list);
			TAILQ_INSERT_HEAD(&cmdhist, hist, list);
			return (0);
		}
	}

	if ((hist = calloc(1, sizeof(*hist))) == NULL)
		fatal("%s: calloc: %s", __func__, errno_s);

	hist->cmd = ce_strdup(cmd);

	TAILQ_INSERT_HEAD(&cmdhist, hist, list);

	return (-1);
}
