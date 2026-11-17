/* $OpenBSD: cmd-pipe-pane.c,v 1.65 2026/10/09 13:12:14 nicm Exp $ */

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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"
#include "platform/tmux-platform.h"

/*
 * Open pipe to redirect pane output. If already open, close first.
 */

static enum cmd_retval	cmd_pipe_pane_exec(struct cmd *, struct cmdq_item *);

static void cmd_pipe_pane_read_callback(struct bufferevent *, void *);
static void cmd_pipe_pane_write_callback(struct bufferevent *, void *);
static void cmd_pipe_pane_error_callback(struct bufferevent *, short, void *);

const struct cmd_entry cmd_pipe_pane_entry = {
	.name = "pipe-pane",
	.description = "Pipe pane input or output to a command.",
	.alias = "pipep",

	.args = { "IOot:", 0, 1, NULL },
	.usage = "[-IOo] " CMD_TARGET_PANE_USAGE " [shell-command]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_pipe_pane_exec
};

static enum cmd_retval
cmd_pipe_pane_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		*target = cmdq_get_target(item);
	struct client			*tc = cmdq_get_target_client(item);
	struct window_pane		*wp = target->wp;
	struct session			*s = target->s;
	struct winlink			*wl = target->wl;
	struct window_pane_offset	*wpo = &wp->pipe_offset;
	char				*cmd;
	int				 old_fd, pipe_fd[2], in, out;
	struct format_tree		*ft;

	/* Do nothing if pane is dead. */
	if (window_pane_exited(wp)) {
		cmdq_error(item, "target pane has exited");
		return (CMD_RETURN_ERROR);
	}

	/* Destroy the old pipe. */
	old_fd = wp->pipe_fd;
	if (wp->pipe_fd != -1) {
		window_pane_close_pipe(wp);

		if (window_pane_destroy_ready(wp)) {
			server_destroy_pane(wp, 1);
			return (CMD_RETURN_NORMAL);
		}
	}

	/* If no pipe command, that is enough. */
	if (args_count(args) == 0 || *args_string(args, 0) == '\0')
		return (CMD_RETURN_NORMAL);

	/*
	 * With -o, only open the new pipe if there was no previous one. This
	 * allows a pipe to be toggled with a single key, for example:
	 *
	 *	bind ^p pipep -o 'cat >>~/output'
	 */
	if (args_has(args, 'o') && old_fd != -1)
		return (CMD_RETURN_NORMAL);

	/* What do we want to do? Neither -I or -O is -O. */
	if (args_has(args, 'I')) {
		in = 1;
		out = args_has(args, 'O');
	} else {
		in = 0;
		out = 1;
	}

	/* Expand the command. */
	ft = format_create(cmdq_get_client(item), item, FORMAT_NONE, 0);
	format_defaults(ft, tc, s, wl, wp);
	cmd = format_expand_time(ft, args_string(args, 0));
	format_free(ft);

	{
		struct win32_pty	*child = NULL;
		struct environ		*env;
		const char		*shell;
		char			*envblock, *command;
		pid_t			 child_pid;

		shell = options_get_string(s->options, "default-shell");
		if (!checkshell(shell))
			shell = _PATH_BSHELL;
		env = environ_for_session(s, 0);
		environ_set(env, "SHELL", 0, "%s", shell);
		envblock = environ_to_win32_block(env);
		command = tmux_windows_shell_command(shell, cmd);
		if (envblock != NULL && command != NULL) {
			child = win32_pipe_spawn(command, wp->cwd, envblock,
			    &child_pid);
		}
		free(command);
		free(envblock);
		environ_free(env);
		if (child == NULL) {
			cmdq_error(item, "pipe spawn failed (error %lu)",
			    (unsigned long)GetLastError());
			free(cmd);
			return (CMD_RETURN_ERROR);
		}
		wp->win32_pipe_child = child;
		wp->pipe_fd = win32_pty_get_fd(child);
		win32_process_watch(win32_pty_get_process(child), child_pid);
		memcpy(wpo, &wp->offset, sizeof *wpo);

		setblocking(wp->pipe_fd, 0);
		wp->pipe_event = bufferevent_new(wp->pipe_fd,
		    cmd_pipe_pane_read_callback,
		    cmd_pipe_pane_write_callback,
		    cmd_pipe_pane_error_callback,
		    wp);
		if (wp->pipe_event == NULL)
			fatalx("out of memory");
		if (out)
			bufferevent_enable(wp->pipe_event, EV_WRITE);
		if (in)
			bufferevent_enable(wp->pipe_event, EV_READ);

		free(cmd);
		return (CMD_RETURN_NORMAL);
	}
}

static void
cmd_pipe_pane_read_callback(__unused struct bufferevent *bufev, void *data)
{
	struct window_pane	*wp = data;
	struct evbuffer		*evb;
	size_t			 available;

	if (wp->pipe_event == NULL)
		return;
	evb = wp->pipe_event->input;

	available = EVBUFFER_LENGTH(evb);
	log_debug("%%%u pipe read %zu", wp->id, available);

	bufferevent_write(wp->event, EVBUFFER_DATA(evb), available);
	evbuffer_drain(evb, available);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
cmd_pipe_pane_write_callback(__unused struct bufferevent *bufev, void *data)
{
	struct window_pane	*wp = data;

	log_debug("%%%u pipe empty", wp->id);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}

static void
cmd_pipe_pane_error_callback(__unused struct bufferevent *bufev,
    __unused short what, void *data)
{
	struct window_pane	*wp = data;

	log_debug("%%%u pipe error", wp->id);

	window_pane_close_pipe(wp);

	if (window_pane_destroy_ready(wp))
		server_destroy_pane(wp, 1);
}
