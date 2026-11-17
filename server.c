/* $OpenBSD: server.c,v 1.217 2026/10/23 06:45:15 nicm Exp $ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tmux.h"

/*
 * Main server functions.
 */

struct clients		 clients;

struct tmuxproc		*server_proc;
static int		 server_fd = -1;
static uint64_t		 server_client_flags;
static int		 server_exit;
static struct event	 server_ev_accept;
static struct event	 server_ev_tidy;

#define MAX_PENDING_TTYS    16
#define PENDING_TTY_EXPIRY  30

struct pending_tty {
	char			 token[65];
	int			 fd;
	time_t			 created;
	TAILQ_ENTRY(pending_tty) entry;
};
static TAILQ_HEAD(, pending_tty) pending_ttys =
    TAILQ_HEAD_INITIALIZER(pending_ttys);

void
server_add_pending_tty(const char *token, int fd)
{
	struct pending_tty	*pt, *pt_next;
	time_t			 now;
	u_int			 count = 0;

	now = time(NULL);

	TAILQ_FOREACH_SAFE(pt, &pending_ttys, entry, pt_next) {
		if (now - pt->created >= PENDING_TTY_EXPIRY) {
			log_debug("expiring pending tty: token=%s fd=%d",
			    pt->token, pt->fd);
			close(pt->fd);
			TAILQ_REMOVE(&pending_ttys, pt, entry);
			free(pt);
		} else
			count++;
	}

	if (count >= MAX_PENDING_TTYS) {
		log_debug("pending tty queue full, rejecting fd=%d", fd);
		close(fd);
		return;
	}

	pt = xcalloc(1, sizeof *pt);
	strlcpy(pt->token, token, sizeof pt->token);
	pt->fd = fd;
	pt->created = now;
	TAILQ_INSERT_TAIL(&pending_ttys, pt, entry);
	log_debug("added pending tty: token=%s fd=%d", token, fd);
}

int
server_get_pending_tty(const char *token)
{
	struct pending_tty	*pt, *pt_next;
	int			 fd;

	TAILQ_FOREACH_SAFE(pt, &pending_ttys, entry, pt_next) {
		if (strcmp(pt->token, token) == 0) {
			fd = pt->fd;
			TAILQ_REMOVE(&pending_ttys, pt, entry);
			free(pt);
			log_debug("matched pending tty: token=%s fd=%d",
			    token, fd);
			return (fd);
		}
	}
	return (-1);
}

struct cmd_find_state	 marked_pane;

static u_int		 message_next;
struct message_list	 message_log;

time_t			 current_time;

static int	server_loop(void);
static void	server_stop_listening(void);
static void	server_send_exit(void);

#define SERVER_LAUNCHER_ENV "TMUX_WIN32_LAUNCHER_PID"
static pid_t		server_launcher_pid;
static struct event	server_launcher_timer;

static void
server_launcher_timeout(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
	log_debug("launching client %ld did not arrive",
	    (long)server_launcher_pid);
	server_launcher_pid = 0;
}

static void
server_launcher_init(void)
{
	char		value[32];
	struct timeval	tv = { .tv_sec = 10 };
	DWORD		n;

	n = GetEnvironmentVariableA(SERVER_LAUNCHER_ENV, value, sizeof value);
	SetEnvironmentVariableA(SERVER_LAUNCHER_ENV, NULL);
	environ_unset(global_environ, SERVER_LAUNCHER_ENV);
	if (n == 0 || n >= sizeof value)
		return;
	server_launcher_pid = (pid_t)strtol(value, NULL, 10);
	log_debug("waiting for launching client %ld",
	    (long)server_launcher_pid);
	evtimer_set(&server_launcher_timer, server_launcher_timeout, NULL);
	evtimer_add(&server_launcher_timer, &tv);
}

void
server_launcher_arrived(pid_t pid)
{
	if (server_launcher_pid == 0 || pid != server_launcher_pid)
		return;
	log_debug("launching client %ld arrived", (long)pid);
	server_launcher_pid = 0;
	evtimer_del(&server_launcher_timer);
}

struct server_pane_exit {
	u_int	id;
	pid_t	pid;
};

static void
server_pane_exit_timer(__unused evutil_socket_t fd, __unused short events,
    void *arg)
{
	struct server_pane_exit	*pe = arg;
	struct window_pane	*wp;

	wp = window_pane_find_by_id(pe->id);
	if (wp != NULL && wp->pid == pe->pid && (~wp->flags & PANE_EXITED)) {
		log_debug("%%%u output did not end; finishing", wp->id);
		wp->flags |= PANE_EXITED;
		if (window_pane_destroy_ready(wp))
			server_destroy_pane(wp, 1);
	}
	free(pe);
}

static void
server_pane_exit_later(struct window_pane *wp)
{
	struct server_pane_exit	*pe;
	struct timeval		 tv = { .tv_sec = 2 };

	if (wp->fd == -1) {
		wp->flags |= PANE_EXITED;
		if (window_pane_destroy_ready(wp))
			server_destroy_pane(wp, 1);
		return;
	}
	pe = xmalloc(sizeof *pe);
	pe->id = wp->id;
	pe->pid = wp->pid;
	event_once(-1, EV_TIMEOUT, server_pane_exit_timer, pe, &tv);
}
static void	server_accept(evutil_socket_t, short, void *);
static void	server_signal(int);
static void	server_child_signal(void);
static void	server_child_exited(pid_t, int);
static void	server_child_stopped(pid_t, int);

/* Set marked pane. */
void
server_set_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	cmd_find_clear_state(&marked_pane, 0);
	marked_pane.s = s;
	marked_pane.wl = wl;
	if (wl != NULL)
		marked_pane.w = wl->window;
	marked_pane.wp = wp;
}

/* Clear marked pane. */
void
server_clear_marked(void)
{
	cmd_find_clear_state(&marked_pane, 0);
}

/* Is this the marked pane? */
int
server_is_marked(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	if (s == NULL || wl == NULL || wp == NULL)
		return (0);
	if (marked_pane.s != s || marked_pane.wl != wl)
		return (0);
	if (marked_pane.wp != wp)
		return (0);
	return (server_check_marked());
}

/* Check if the marked pane is still valid. */
int
server_check_marked(void)
{
	return (cmd_find_valid_state(&marked_pane));
}

/* Create server socket. */
int
server_create_socket(uint64_t flags, char **cause)
{
	int	fd;

	(void)flags;
	fd = win32_ipc_create_server(socket_path);
	if (fd == -1) {
		if (cause != NULL)
			xasprintf(cause, "error creating %s (%s)", socket_path,
			    strerror(errno));
		return (-1);
	}
	setblocking(fd, 0);
	win32_signal_events();
	return (fd);
}

/* Tidy up every hour. */
static void
server_tidy_event(__unused evutil_socket_t fd, __unused short events,
    __unused void *data)
{
    struct timeval	tv = { .tv_sec = 3600 };
    uint64_t		t = get_timer();

    format_tidy_jobs();

    log_debug("%s: took %llu milliseconds", __func__,
        (unsigned long long)(get_timer() - t));
    evtimer_add(&server_ev_tidy, &tv);
}

/* Fork new server. */
int
server_start(struct tmuxproc *client, uint64_t flags, struct event_base *base,
    int lockfd, char *lockfile)
{
	int		 fd;
	struct client	*c = NULL;
	char		*cause = NULL;
	struct timeval	 tv = { .tv_sec = 3600 };

	if (~flags & CLIENT_NOFORK) {
		if (proc_fork_and_daemon(&fd) != 0) {
			return (fd);
		}
	}
	proc_clear_signals(client, 0);
	server_client_flags = flags;

	if (event_reinit(base) != 0)
		fatalx("event_reinit failed");
	server_proc = proc_start("server");

	proc_set_signals(server_proc, server_signal);

	if (log_get_level() > 1)
		tty_create_log();
	if (pledge("stdio rpath wpath cpath fattr unix getpw recvfd proc exec "
	    "tty ps", NULL) != 0)
		fatal("pledge failed");

	input_key_build();
	utf8_update_width_cache();
	RB_INIT(&windows);
	RB_INIT(&all_window_panes);
	TAILQ_INIT(&clients);
	RB_INIT(&sessions);
	key_bindings_init();
	control_build_events();
	hooks_build_events();
	TAILQ_INIT(&message_log);
	gettimeofday(&start_time, NULL);

	server_fd = server_create_socket(flags, &cause);
	if (server_fd != -1)
		server_update_socket();
	if (~flags & CLIENT_NOFORK)
		c = server_client_create(fd);

	if (lockfd >= 0) {
		unlink(lockfile);
		free(lockfile);
		close(lockfd);
	}

	if (cause != NULL) {
		if (c != NULL) {
			c->exit_message = cause;
			c->retval = 1;
			c->flags |= CLIENT_EXIT;
		} else {
			fprintf(stderr, "%s\n", cause);
			exit(1);
		}
	}

	evtimer_set(&server_ev_tidy, server_tidy_event, NULL);
	evtimer_add(&server_ev_tidy, &tv);

	server_acl_init();

	server_add_accept(0);
	server_launcher_init();
	proc_loop(server_proc, server_loop);

	server_stop_listening();
	job_kill_all();
	prompt_save_history();

	exit(0);
}

/* Server loop callback. */
static int
server_loop(void)
{
	struct client	*c;
	u_int		 items;

	current_time = time(NULL);

	do {
		items = cmdq_next(NULL);
		TAILQ_FOREACH(c, &clients, entry) {
			if (c->flags & CLIENT_IDENTIFIED)
				items += cmdq_next(c);
		}
		TAILQ_FOREACH(c, &dead_clients, entry)
			items += cmdq_next(c);
	} while (items != 0);

	server_client_loop();

	if (!options_get_number(global_options, "exit-empty") && !server_exit)
		return (0);
	if (server_launcher_pid != 0 && !server_exit)
		return (0);

	if (!options_get_number(global_options, "exit-unattached")) {
		if (!RB_EMPTY(&sessions))
			return (0);
	}

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session != NULL)
			return (0);
	}

	/*
	 * No attached clients therefore want to exit - flush any waiting
	 * clients but don't actually exit until they've gone.
	 */
	cmd_wait_for_flush();
	if (!TAILQ_EMPTY(&clients))
		return (0);

	if (job_still_running())
		return (0);
	return (1);
}

static void
server_stop_listening(void)
{
	if (event_initialized(&server_ev_accept))
		event_del(&server_ev_accept);
	if (server_fd != -1) {
		closesocket((SOCKET)server_fd);
		server_fd = -1;
	}
	win32_ipc_cleanup(socket_path);
}

/* Exit the server by killing all clients and windows. */
static void
server_send_exit(void)
{
	struct client	*c, *c1;
	struct session	*s, *s1;

	server_stop_listening();
	cmd_wait_for_flush();

	TAILQ_FOREACH_SAFE(c, &clients, entry, c1) {
		if (c->flags & CLIENT_SUSPENDED)
			server_client_lost(c);
		else {
			c->flags |= CLIENT_EXIT;
			c->exit_type = CLIENT_EXIT_SHUTDOWN;
		}
		c->session = NULL;
	}

	RB_FOREACH_SAFE(s, sessions, &sessions, s1)
		session_destroy(s, 1, __func__);
}

/* Update socket execute permissions based on whether sessions are attached. */
void
server_update_socket(void)
{
}

/* Callback for server socket. */
static void
server_accept(evutil_socket_t fd, short events, __unused void *data)
{
	struct sockaddr_storage	 sa;
	socklen_t		 slen = sizeof sa;
	int			 newfd;
	struct client		*c;

	server_add_accept(0);
	if (!(events & EV_READ))
		return;

	newfd = accept(fd, (struct sockaddr *) &sa, &slen);
	if (newfd == -1) {
		if (errno == EAGAIN || errno == EINTR || errno == ECONNABORTED)
			return;
		if (errno == ENFILE || errno == EMFILE) {
			/* Delete and don't try again for 1 second. */
			server_add_accept(1);
			return;
		}
		fatal("accept failed");
	}

	{
		char tty_token[65] = {0};
		int auth_result;

		auth_result = win32_ipc_verify_auth(newfd, socket_path,
		    tty_token, sizeof tty_token);
		if (auth_result == -1) {
			closesocket((SOCKET)newfd);
			return;
		}
		if (auth_result == 1) {
			struct client *tc;
			TAILQ_FOREACH(tc, &clients, entry) {
				if (tc->tty_token != NULL &&
				    strcmp(tc->tty_token, tty_token) == 0) {
					tc->fd = newfd;
					log_debug("matched tty fd %d for "
					    "client %p", newfd, tc);
					free(tc->tty_token);
					tc->tty_token = NULL;
					if ((tc->flags & CLIENT_IDENTIFIED) &&
					    (tc->flags & CLIENT_CONTROL) &&
					    tc->control_state == NULL)
						control_start(tc);
					if ((tc->flags & CLIENT_IDENTIFIED) &&
					    !(tc->flags &
					    (CLIENT_TERMINAL|CLIENT_CONTROL))) {
						u_int sx = tc->tty.sx;
						u_int sy = tc->tty.sy;
						if (tty_init(&tc->tty, tc) == 0) {
							if (sx == 0)
								sx = 80;
							if (sy == 0)
								sy = 24;
							tty_set_size(&tc->tty,
							    sx, sy, 0, 0);
							tc->flags |= CLIENT_TERMINAL;
							recalculate_sizes();
							server_redraw_client(tc);
						}
					}
					if (tc->flags & CLIENT_IDENTIFIED) {
						if (event_initialized(
						    &tc->tty_wait_timer))
							evtimer_del(
							    &tc->tty_wait_timer);
						if (tc->tty_wait_item != NULL) {
							cmdq_continue(
							    tc->tty_wait_item);
							tc->tty_wait_item = NULL;
						}
					}
					server_add_accept(0);
					return;
				}
			}
			server_add_pending_tty(tty_token, newfd);
			return;
		}
	}

	if (server_exit) {
		closesocket((SOCKET)newfd);
		return;
	}
	c = server_client_create(newfd);
	if (!server_acl_join(c)) {
		c->exit_message = xstrdup("access not allowed");
		c->retval = 1;
		c->flags |= CLIENT_EXIT;
	}
}

/*
 * Add accept event. If timeout is nonzero, add as a timeout instead of a read
 * event - used to backoff when running out of file descriptors.
 */
void
server_add_accept(int timeout)
{
	struct timeval tv = { timeout, 0 };

	if (server_fd == -1)
		return;

	if (event_initialized(&server_ev_accept))
		event_del(&server_ev_accept);

	if (timeout == 0) {
		event_set(&server_ev_accept, server_fd, EV_READ, server_accept,
		    NULL);
		event_add(&server_ev_accept, NULL);
	} else {
		event_set(&server_ev_accept, server_fd, EV_TIMEOUT,
		    server_accept, NULL);
		event_add(&server_ev_accept, &tv);
	}
}

/* Signal handler. */
static void
server_signal(int sig)
{
	int	fd;

	log_debug("%s: %s", __func__, strsignal(sig));
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		server_exit = 1;
		server_send_exit();
		break;
	case SIGCHLD:
		server_child_signal();
		break;
	case SIGUSR1:
		event_del(&server_ev_accept);
		fd = server_create_socket(server_client_flags, NULL);
		if (fd != -1) {
			closesocket((SOCKET)server_fd);
			server_fd = fd;
			server_update_socket();
		}
		server_add_accept(0);
		break;
	case SIGUSR2:
		proc_toggle_log(server_proc);
		break;
	case SIGWINCH:
		break;
	}
}

/* Handle SIGCHLD. */
static void
server_child_signal(void)
{
	int	 status;
	pid_t	 pid;

	for (;;) {
		switch (pid = waitpid(WAIT_ANY, &status, WNOHANG|WUNTRACED)) {
		case -1:
			if (errno == ECHILD)
				return;
			return;
		case 0:
			return;
		}
		if (WIFSTOPPED(status))
			server_child_stopped(pid, status);
		else if (WIFEXITED(status) || WIFSIGNALED(status))
			server_child_exited(pid, status);
	}
}

/* Handle exited children. */
static void
server_child_exited(pid_t pid, int status)
{
	struct window		*w, *w1;
	struct window_pane	*wp;

	RB_FOREACH_SAFE(w, windows, &windows, w1) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				wp->status = status;
				wp->flags |= PANE_STATUSREADY;

				log_debug("%%%u exited", wp->id);
				server_pane_exit_later(wp);
				break;
				wp->flags |= PANE_EXITED;

				window_pane_wait_finish(wp);
				spawn_editor_finish(wp);

				if (window_pane_destroy_ready(wp))
					server_destroy_pane(wp, 1);
				break;
			}
		}
	}
	job_check_died(pid, status);
}

/* Handle stopped children. */
static void
server_child_stopped(pid_t pid, int status)
{
	struct window		*w;
	struct window_pane	*wp;

	if (WSTOPSIG(status) == SIGTTIN || WSTOPSIG(status) == SIGTTOU)
		return;

	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			if (wp->pid == pid) {
				if (killpg(pid, SIGCONT) != 0)
					kill(pid, SIGCONT);
			}
		}
	}
	job_check_died(pid, status);
}

/* Add to message log. */
void
server_add_message(const char *fmt, ...)
{
	struct message_entry	*msg, *msg1;
	char			*s;
	va_list			 ap;
	u_int			 limit;

	va_start(ap, fmt);
	xvasprintf(&s, fmt, ap);
	va_end(ap);

	log_debug("message: %s", s);

	msg = xcalloc(1, sizeof *msg);
	gettimeofday(&msg->msg_time, NULL);
	msg->msg_num = message_next++;
	msg->msg = s;
	TAILQ_INSERT_TAIL(&message_log, msg, entry);

	limit = options_get_number(global_options, "message-limit");
	TAILQ_FOREACH_SAFE(msg, &message_log, entry, msg1) {
		if (msg->msg_num + limit >= message_next)
			break;
		free(msg->msg);
		TAILQ_REMOVE(&message_log, msg, entry);
		free(msg);
	}
}
