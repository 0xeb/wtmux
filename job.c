/* $OpenBSD: job.c,v 1.75 2026/07/12 20:35:52 nicm Exp $ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "platform/tmux-platform.h"

/*
 * Job scheduling. Run queued commands in the background and record their
 * output.
 */

static void	job_read_callback(struct bufferevent *, void *);
static void	job_write_callback(struct bufferevent *, void *);
static void	job_error_callback(struct bufferevent *, short, void *);

/* A single job. */
struct job {
	enum {
		JOB_RUNNING,
		JOB_DEAD,
		JOB_CLOSED
	} state;

	int			 flags;

	char			*cmd;
	pid_t			 pid;
	char			 tty[TTY_NAME_MAX];
	int			 status;

	int			 fd;
	struct bufferevent	*event;
	struct win32_pty	*native_pty;

	job_update_cb		 updatecb;
	job_complete_cb		 completecb;
	job_free_cb		 freecb;
	void			*data;

	LIST_ENTRY(job)		 entry;
};

/* All jobs list. */
static LIST_HEAD(joblist, job) all_jobs = LIST_HEAD_INITIALIZER(all_jobs);

/* Start a job running. */
struct job *
job_run(const char *cmd, int argc, char **argv, struct environ *e,
    struct session *s, const char *cwd, job_update_cb updatecb,
    job_complete_cb completecb, job_free_cb freecb, void *data, int flags,
    int sx, int sy)
{
	struct job	 *job;
	struct environ	 *env;
	pid_t		  pid;
	int		  out[2];
	const char	 *shell;
	char		 *cmdline, *argv0, *envblock, **native_argv;
	struct options	 *oo;
	struct win32_pty *pty = NULL;

	/*
	 * Do not set TERM during .tmux.conf (second argument here), it is nice
	 * to be able to use if-shell to decide on default-terminal based on
	 * outside TERM.
	 */
	env = environ_for_session(s, !cfg_finished);
	if (e != NULL)
		environ_copy(e, env);

	flags |= JOB_DEFAULTSHELL;
	if (~flags & JOB_DEFAULTSHELL)
		shell = _PATH_BSHELL;
	else {
		if (s != NULL)
			oo = s->options;
		else
			oo = global_s_options;
		shell = options_get_string(oo, "default-shell");
		if (!checkshell(shell))
			shell = _PATH_BSHELL;
	}
	argv0 = shell_argv0(shell, 0);

	if (cmd == NULL) {
		cmd_log_argv(argc, argv, "%s:", __func__);
		log_debug("%s: cwd=%s, shell=%s", __func__,
		    cwd == NULL ? "" : cwd, shell);
	} else {
		log_debug("%s: cmd=%s, cwd=%s, shell=%s", __func__, cmd,
		    cwd == NULL ? "" : cwd, shell);
	}

	if (cmd != NULL) {
		environ_set(env, "SHELL", 0, "%s", shell);
		cmdline = tmux_windows_shell_command(shell, cmd);
	} else {
		native_argv = cmd_copy_argv(argc, argv);
		cmdline = tmux_windows_program_command_line(
		    (const char *const *)native_argv);
		cmd_free_argv(argc, native_argv);
	}
	if (cmdline == NULL)
		goto fail;
	envblock = environ_to_win32_block(env);
	if (envblock == NULL) {
		free(cmdline);
		goto fail;
	}
	if (flags & JOB_PTY)
		pty = win32_pty_spawn(cmdline, cwd, envblock, sx, sy, CP_UTF8,
		    &pid);
	else
		pty = win32_pipe_spawn(cmdline, cwd, envblock, &pid);
	free(envblock);
	free(cmdline);
	if (pty == NULL)
		goto fail;

	environ_free(env);
	free(argv0);

	job = xcalloc(1, sizeof *job);
	job->state = JOB_RUNNING;
	job->flags = flags;

	if (cmd != NULL)
		job->cmd = xstrdup(cmd);
	else
		job->cmd = cmd_stringify_argv(argc, argv);
	job->pid = pid;
	job->status = 0;
	job->native_pty = pty;
	win32_process_watch(win32_pty_get_process(pty), pid);

	LIST_INSERT_HEAD(&all_jobs, job, entry);

	job->updatecb = updatecb;
	job->completecb = completecb;
	job->freecb = freecb;
	job->data = data;

	job->fd = win32_pty_get_fd(pty);
	setblocking(job->fd, 0);

	job->event = bufferevent_new(job->fd, job_read_callback,
	    job_write_callback, job_error_callback, job);
	if (job->event == NULL)
		fatalx("out of memory");
	bufferevent_enable(job->event, EV_READ|EV_WRITE);

	log_debug("run job %p: %s, pid %ld", job, job->cmd, (long)job->pid);
	return (job);

fail:
	environ_free(env);
	free(argv0);
	return (NULL);
}

/* Take job's file descriptor and free the job. */
int
job_transfer(struct job *job, pid_t *pid, char *tty, size_t ttylen)
{
	int	fd = job->fd;

	log_debug("transfer job %p: %s", job, job->cmd);

	if (pid != NULL)
		*pid = job->pid;
	if (tty != NULL)
		strlcpy(tty, job->tty, ttylen);

	LIST_REMOVE(job, entry);
	free(job->cmd);

	if (job->freecb != NULL && job->data != NULL)
		job->freecb(job->data);

	if (job->event != NULL)
		bufferevent_free(job->event);

	free(job);
	return (fd);
}

/* Kill and free an individual job. */
void
job_free(struct job *job)
{
	log_debug("free job %p: %s", job, job->cmd);

	LIST_REMOVE(job, entry);
	free(job->cmd);

	if (job->freecb != NULL && job->data != NULL)
		job->freecb(job->data);

	if (job->pid != -1)
		kill(job->pid, SIGTERM);
	if (job->event != NULL)
		bufferevent_free(job->event);
	if (job->native_pty != NULL)
		win32_pty_close(job->native_pty);
	else if (job->fd != -1)
		closesocket((SOCKET)job->fd);

	free(job);
}

/* Resize job. */
void
job_resize(struct job *job, u_int sx, u_int sy)
{

	if (job->fd == -1 || (~job->flags & JOB_PTY))
		return;

	log_debug("resize job %p: %ux%u", job, sx, sy);

	if (job->native_pty != NULL)
		win32_pty_resize(job->native_pty, (int)sx, (int)sy);
}

/* Job buffer read callback. */
static void
job_read_callback(__unused struct bufferevent *bufev, void *data)
{
	struct job	*job = data;

	if (job->updatecb != NULL)
		job->updatecb(job);
}

/*
 * Job buffer write callback. Fired when the buffer falls below watermark
 * (default is empty). If all the data has been written, disable the write
 * event.
 */
static void
job_write_callback(__unused struct bufferevent *bufev, void *data)
{
	struct job	*job = data;
	size_t		 len = EVBUFFER_LENGTH(EVBUFFER_OUTPUT(job->event));

	log_debug("job write %p: %s, pid %ld, output left %zu", job, job->cmd,
	    (long) job->pid, len);

	if (len == 0 && (~job->flags & JOB_KEEPWRITE)) {
		shutdown(job->fd, SHUT_WR);
		bufferevent_disable(job->event, EV_WRITE);
	}
}

/* Job buffer error callback. */
static void
job_error_callback(__unused struct bufferevent *bufev, __unused short events,
    void *data)
{
	struct job	*job = data;

	log_debug("job error %p: %s, pid %ld", job, job->cmd, (long) job->pid);

	if (job->state == JOB_DEAD) {
		if (job->completecb != NULL)
			job->completecb(job);
		job_free(job);
	} else {
		bufferevent_disable(job->event, EV_READ);
		job->state = JOB_CLOSED;
	}
}

/* Job died (waitpid() returned its pid). */
void
job_check_died(pid_t pid, int status)
{
	struct job	*job;

	LIST_FOREACH(job, &all_jobs, entry) {
		if (pid == job->pid)
			break;
	}
	if (job == NULL)
		return;
	if (WIFSTOPPED(status)) {
		if (WSTOPSIG(status) == SIGTTIN || WSTOPSIG(status) == SIGTTOU)
			return;
		killpg(job->pid, SIGCONT);
		return;
	}
	log_debug("job died %p: %s, pid %ld", job, job->cmd, (long) job->pid);

	job->status = status;

	if (job->state == JOB_CLOSED) {
		if (job->completecb != NULL)
			job->completecb(job);
		job_free(job);
	} else {
		job->pid = -1;
		job->state = JOB_DEAD;
	}
}

/* Get job status. */
int
job_get_status(struct job *job)
{
	return (job->status);
}

/* Get job data. */
void *
job_get_data(struct job *job)
{
	return (job->data);
}

/* Get job event. */
struct bufferevent *
job_get_event(struct job *job)
{
	return (job->event);
}

/* Kill all jobs. */
void
job_kill_all(void)
{
	struct job	*job;

	LIST_FOREACH(job, &all_jobs, entry) {
		if (job->pid != -1)
			kill(job->pid, SIGTERM);
	}
}

/* Are any jobs still running? */
int
job_still_running(void)
{
	struct job	*job;

	LIST_FOREACH(job, &all_jobs, entry) {
		if ((~job->flags & JOB_NOWAIT) && job->state == JOB_RUNNING)
			return (1);
	}
	return (0);
}

/* Print job summary. */
void
job_print_summary(struct cmdq_item *item, int blank)
{
	struct job	*job;
	u_int		 n = 0;

	LIST_FOREACH(job, &all_jobs, entry) {
		if (blank) {
			cmdq_print(item, "%s", "");
			blank = 0;
		}
		cmdq_print(item, "Job %u: %s [fd=%d, pid=%ld, status=%d]",
		    n, job->cmd, job->fd, (long)job->pid, job->status);
		n++;
	}
}
