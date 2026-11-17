/* $OpenBSD: client.c,v 1.169 2026/10/27 08:38:40 nicm Exp $ */

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
#include <stdlib.h>
#include <string.h>

#include "tmux.h"
#include "platform/win32-internal.h"

static struct tmuxproc	*client_proc;
static struct tmuxpeer	*client_peer;
static uint64_t		 client_flags;
static int		 client_suspended;
static int		 client_tty_fd = -1;
static struct event	 client_tty_event;
static HANDLE		 client_stdin_thread_h = NULL;
static volatile int	 client_tty_running = 0;
static int		 client_tty_relayed = 0;
static char		*client_start_error;
static enum {
	CLIENT_EXIT_NONE,
	CLIENT_EXIT_DETACHED,
	CLIENT_EXIT_DETACHED_HUP,
	CLIENT_EXIT_LOST_TTY,
	CLIENT_EXIT_TERMINATED,
	CLIENT_EXIT_LOST_SERVER,
	CLIENT_EXIT_EXITED,
	CLIENT_EXIT_SERVER_EXITED,
	CLIENT_EXIT_MESSAGE_PROVIDED
} client_exitreason = CLIENT_EXIT_NONE;
static int		 client_exitflag;
static int		 client_exitval;
static enum msgtype	 client_exittype;
static const char	*client_exitsession;
static char		*client_exitmessage;
static const char	*client_execshell;
static const char	*client_execcmd;
static int		 client_attached;
static struct client_files client_files = RB_INITIALIZER(&client_files);

static __dead void	 client_exec(const char *,const char *);
static int		 client_get_lock(char *);
static int		 client_connect(struct event_base *, const char *,
			     uint64_t);
static void		 client_send_identify(const char *, const char *,
			     char **, u_int, const char *, int);
static void		 client_signal(int);
static void		 client_dispatch(struct imsg *, void *);
static void		 client_dispatch_attached(struct imsg *);
static void		 client_dispatch_wait(struct imsg *);
static const char	*client_exit_message(void);
static void		 client_start_tty_relay(void);
static void		 client_drain_tty_relay(void);
static void		 client_stop_tty_relay(void);

/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
	int lockfd;

	log_debug("lock file is %s", lockfile);

	if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
		log_debug("open failed: %s", strerror(errno));
		return (-1);
	}

	if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
		log_debug("flock failed: %s", strerror(errno));
		if (errno != EAGAIN)
			return (lockfd);
		while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
			/* nothing */;
		close(lockfd);
		return (-2);
	}
	log_debug("flock succeeded");

	return (lockfd);
}

static char *
client_read_errors(HANDLE errors)
{
	char	 buf[4096], *line, *end;
	DWORD	 n, used = 0;

	if (errors == NULL)
		return (NULL);
	while (used < sizeof buf - 1 &&
	    ReadFile(errors, buf + used, sizeof buf - 1 - used, &n, NULL) &&
	    n != 0)
		used += n;
	buf[used] = '\0';
	while (used != 0 && (buf[used - 1] == '\n' || buf[used - 1] == '\r'))
		buf[--used] = '\0';
	if (used == 0)
		return (NULL);
	line = buf;
	while ((end = strchr(line, '\n')) != NULL && end[1] != '\0')
		line = end + 1;
	line[strcspn(line, "\r\n")] = '\0';
	return (xstrdup(line));
}

/* Connect client to server. */
static int
client_connect(struct event_base *base, const char *path, uint64_t flags)
{
	int	fd;
	HANDLE	server = NULL, errors = NULL;

	log_debug("connecting to server %s", path);

	if (win32_remote_native_active()) {
		fd = win32_remote_native_connect(path,
		    (flags & CLIENT_STARTSERVER) &&
		    (~flags & CLIENT_NOSTARTSERVER));
		if (fd != -1)
			setblocking(fd, 0);
		return (fd);
	}

	fd = win32_ipc_connect(path);
	if (fd != -1) {
		setblocking(fd, 0);
		return (fd);
	}

	log_debug("connect failed, starting server");

	if (errno != ECONNREFUSED)
		return (-1);
	if (flags & CLIENT_NOSTARTSERVER)
		return (-1);
	if (~flags & CLIENT_STARTSERVER)
		return (-1);

	if (flags & CLIENT_NOFORK) {
		fd = server_start(client_proc, flags, base, -1, NULL);
		setblocking(fd, 0);
		return (fd);
	}

	{
		const char	**extra;
		u_int		  i, n = 0;
		int		  level;

		extra = xcalloc(2 * cfg_nfiles + log_get_level() + 1,
		    sizeof *extra);
		if (!cfg_quiet) {
			for (i = 0; i < cfg_nfiles; i++) {
				extra[n++] = "-f";
				extra[n++] = cfg_files[i];
			}
		}
		for (level = log_get_level(); level > 0; level--)
			extra[n++] = "-v";
		extra[n] = NULL;
		{
			char	pidstr[32];

			snprintf(pidstr, sizeof pidstr, "%lu",
			    (unsigned long)GetCurrentProcessId());
			SetEnvironmentVariableA("TMUX_WIN32_LAUNCHER_PID",
			    pidstr);
			server = win32_launch_server(path, extra, &errors);
			SetEnvironmentVariableA("TMUX_WIN32_LAUNCHER_PID",
			    NULL);
		}
		free(extra);
	}
	{
		ULONGLONG	start = GetTickCount64(), exited = 0;
		int		retries;

		for (retries = 1;; retries++) {
			Sleep(100);
			fd = win32_ipc_connect(path);
			if (fd != -1) {
				log_debug("server connected after %d retries",
				    retries);
				setblocking(fd, 0);
				break;
			}
			if (exited == 0 && (server == NULL ||
			    WaitForSingleObject(server, 0) == WAIT_OBJECT_0)) {
				if (server != NULL) {
					client_start_error =
					    client_read_errors(errors);
				}
				exited = GetTickCount64();
			}
			if (exited != 0 && !win32_ipc_server_alive(path))
				break;
			if (exited != 0 ? GetTickCount64() - exited >= 5000 :
			    GetTickCount64() - start >= 30000)
				break;
		}
	}
	if (server != NULL)
		CloseHandle(server);
	if (errors != NULL)
		CloseHandle(errors);
	if (fd == -1)
		log_debug("server did not start");
	return (fd);
}

/* Get exit string from reason number. */
const char *
client_exit_message(void)
{
	static char msg[256];

	switch (client_exitreason) {
	case CLIENT_EXIT_NONE:
		break;
	case CLIENT_EXIT_DETACHED:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached");
	case CLIENT_EXIT_DETACHED_HUP:
		if (client_exitsession != NULL) {
			xsnprintf(msg, sizeof msg, "detached and SIGHUP "
			    "(from session %s)", client_exitsession);
			return (msg);
		}
		return ("detached and SIGHUP");
	case CLIENT_EXIT_LOST_TTY:
		return ("lost tty");
	case CLIENT_EXIT_TERMINATED:
		return ("terminated");
	case CLIENT_EXIT_LOST_SERVER:
		return ("server exited unexpectedly");
	case CLIENT_EXIT_EXITED:
		return ("exited");
	case CLIENT_EXIT_SERVER_EXITED:
		return ("server exited");
	case CLIENT_EXIT_MESSAGE_PROVIDED:
		return (client_exitmessage);
	}
	return ("unknown reason");
}

/* Exit if all streams flushed. */
static void
client_exit(void)
{
	if (!file_write_left(&client_files))
		proc_exit(client_proc);
}

/* Client main loop. */
int
client_main(struct event_base *base, int argc, char **argv, uint64_t flags,
    int feat)
{
	struct cmd_parse_result	*pr;
	struct msg_command	*data;
	int			 fd, i;
	const char		*ttynam, *termname, *cwd;
	pid_t			 ppid;
	enum msgtype		 msg;
	struct termios		 tio, saved_tio;
	size_t			 size;
	char			**caps = NULL, *cause;
	u_int			 ncaps = 0;
	struct args_value	*values;

	/* Set up the initial command. */
	if (shell_command != NULL) {
		msg = MSG_SHELL;
		flags |= CLIENT_STARTSERVER;
	} else if (argc == 0) {
		msg = MSG_COMMAND;
		flags |= CLIENT_STARTSERVER;
	} else {
		msg = MSG_COMMAND;

		/*
		 * It's annoying parsing the command string twice (in client
		 * and later in server) but it is necessary to get the start
		 * server flag.
		 */
		values = args_from_vector(argc, argv);
		pr = cmd_parse_from_arguments(values, argc, NULL);
		if (pr->status == CMD_PARSE_SUCCESS) {
			if (cmd_list_any_have(pr->cmdlist, CMD_STARTSERVER))
				flags |= CLIENT_STARTSERVER;
			cmd_list_free(pr->cmdlist);
		} else
			free(pr->error);
		args_free_values(values, argc);
		free(values);
	}

	/* Create client process structure (starts logging). */
	client_proc = proc_start("client");
	proc_set_signals(client_proc, client_signal);

	/* Save the flags. */
	client_flags = flags|CLIENT_WRITE_ACK;
	log_debug("flags are %#llx", (unsigned long long)client_flags);

	/* Initialize the client socket and start the server. */
	fd = client_connect(base, socket_path, client_flags);
	if (fd == -1 && client_start_error != NULL) {
		if (client_flags & CLIENT_CONTROL)
			printf("%%exit %s\n", client_start_error);
		else
			fprintf(stderr, "%s\n", client_start_error);
		return (1);
	}
	if (fd == -1) {
		if (errno == ECONNREFUSED) {
			fprintf(stderr, "no server running on %s\n",
			    socket_path);
		} else {
			fprintf(stderr, "error connecting to %s (%s)\n",
			    socket_path, strerror(errno));
		}
		return (1);
	}
	client_peer = proc_add_peer(client_proc, fd, client_dispatch, NULL);

	/* Save these before pledge(). */
	if ((cwd = find_cwd()) == NULL && (cwd = find_home()) == NULL)
		cwd = "C:\\";
	if ((ttynam = ttyname(STDIN_FILENO)) == NULL)
		ttynam = "";
	if ((termname = getenv("TERM")) == NULL)
		termname = "xterm-256color";

	/*
	 * Drop privileges for client. "proc exec" is needed for -c and for
	 * locking (which uses system(3)).
	 *
	 * "tty" is needed to restore termios(4) and also for some reason -CC
	 * does not work properly without it (input is not recognised).
	 *
	 * "sendfd" is dropped later in client_dispatch_wait().
	 */
	if (pledge(
	    "stdio rpath wpath cpath unix sendfd proc exec tty",
	    NULL) != 0)
		fatal("pledge failed");

	/* Load terminfo entry if any. */
	if (isatty(STDIN_FILENO) &&
	    *termname != '\0' &&
	    tty_term_read_list(termname, STDIN_FILENO, &caps, &ncaps,
	    &cause) != 0) {
		fprintf(stderr, "%s\n", cause);
		free(cause);
		return (1);
	}

	/* Free stuff that is not used in the client. */
	if (ptm_fd != -1)
		close(ptm_fd);
	options_free(global_options);
	options_free(global_s_options);
	options_free(global_w_options);
	environ_free(global_environ);

	/* Set up control mode. */
	if (client_flags & CLIENT_CONTROLCONTROL) {
		if (tcgetattr(STDIN_FILENO, &saved_tio) != 0) {
			fprintf(stderr, "tcgetattr failed: %s\n",
			    strerror(errno));
			return (1);
		}
		memcpy(&tio, &saved_tio, sizeof tio);
		cfmakeraw(&tio);
		tio.c_iflag = ICRNL|IXANY;
		tio.c_oflag = OPOST|ONLCR;
#ifdef NOKERNINFO
		tio.c_lflag = NOKERNINFO;
#endif
		tio.c_cflag = CREAD|CS8|HUPCL;
		tio.c_cc[VMIN] = 1;
		tio.c_cc[VTIME] = 0;
		cfsetispeed(&tio, cfgetispeed(&saved_tio));
		cfsetospeed(&tio, cfgetospeed(&saved_tio));
		tcsetattr(STDIN_FILENO, TCSANOW, &tio);
	}

	/* Send identify messages. */
	client_send_identify(ttynam, termname, caps, ncaps, cwd, feat);
	tty_term_free_list(caps, ncaps);
	proc_flush_peer(client_peer);
	if (client_flags & CLIENT_CONTROL)
		client_start_tty_relay();

	/* Send first command. */
	if (msg == MSG_COMMAND) {
		/* How big is the command? */
		size = 0;
		for (i = 0; i < argc; i++)
			size += strlen(argv[i]) + 1;
		if (size > MAX_IMSGSIZE - (sizeof *data)) {
			fprintf(stderr, "command too long\n");
			return (1);
		}
		data = xmalloc((sizeof *data) + size);

		/* Prepare command for server. */
		data->argc = argc;
		if (cmd_pack_argv(argc, argv, (char *)(data + 1), size) != 0) {
			fprintf(stderr, "command too long\n");
			free(data);
			return (1);
		}
		size += sizeof *data;

		/* Send the command. */
		if (proc_send(client_peer, msg, -1, data, size) != 0) {
			fprintf(stderr, "failed to send command\n");
			free(data);
			return (1);
		}
		free(data);
	} else if (msg == MSG_SHELL)
		proc_send(client_peer, msg, -1, NULL, 0);

	/* Start main loop. */
	proc_loop(client_proc, NULL);

	win32_fd_bridges_finish();
	client_drain_tty_relay();
	client_stop_tty_relay();

	/* Run command if user requested exec, instead of exiting. */
	if (client_exittype == MSG_EXEC) {
		if (client_flags & CLIENT_CONTROLCONTROL)
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		client_exec(client_execshell, client_execcmd);
	}

	/* Print the exit message, if any, and exit. */
	if (client_attached) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("[%s]\n", client_exit_message());

		ppid = getppid();
		if (client_exittype == MSG_DETACHKILL && ppid > 1)
			kill(ppid, SIGHUP);
	} else if (client_flags & CLIENT_CONTROL) {
		if (client_exitreason != CLIENT_EXIT_NONE)
			printf("%%exit %s\n", client_exit_message());
		else
			printf("%%exit\n");
		fflush(stdout);
		if (client_flags & CLIENT_CONTROL_WAITEXIT)
			control_wait_exit(STDIN_FILENO);
		if (client_flags & CLIENT_CONTROLCONTROL) {
			printf("\033\\");
			fflush(stdout);
			tcsetattr(STDOUT_FILENO, TCSAFLUSH, &saved_tio);
		}
	} else if (client_exitreason != CLIENT_EXIT_NONE)
		fprintf(stderr, "%s\n", client_exit_message());

	/* Restore the streams to blocking. */
	setblocking(STDIN_FILENO, 1);
	setblocking(STDOUT_FILENO, 1);
	setblocking(STDERR_FILENO, 1);

	return (client_exitval);
}

/* Send identify messages to server. */
static void
client_send_identify(const char *ttynam, const char *termname, char **caps,
    u_int ncaps, const char *cwd, int feat)
{
	char	**ss;
	size_t	  sslen;
	int	  fd;
	uint64_t  flags = client_flags;
	pid_t	  pid;
	u_int	  i;
	char	  tty_token[65] = "";

	{
		DWORD	mode;

		if ((client_flags & CLIENT_CONTROL) ||
		    win32_test_terminal() != NULL ||
		    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode))
			win32_generate_tty_token(tty_token, sizeof tty_token);
	}
	if (tty_token[0] != '\0')
		client_tty_fd = win32_ipc_connect_tty(socket_path, tty_token);

	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &flags, sizeof flags);
	proc_send(client_peer, MSG_IDENTIFY_LONGFLAGS, -1, &client_flags,
	    sizeof client_flags);

	proc_send(client_peer, MSG_IDENTIFY_TERM, -1, termname,
	    strlen(termname) + 1);
	proc_send(client_peer, MSG_IDENTIFY_FEATURES, -1, &feat, sizeof feat);

	proc_send(client_peer, MSG_IDENTIFY_TTYNAME, -1, ttynam,
	    strlen(ttynam) + 1);
	if (win32_remote_native_active())
		cwd = "";
	proc_send(client_peer, MSG_IDENTIFY_CWD, -1, cwd, strlen(cwd) + 1);

	for (i = 0; i < ncaps; i++) {
		proc_send(client_peer, MSG_IDENTIFY_TERMINFO, -1,
		    caps[i], strlen(caps[i]) + 1);
	}

	proc_send(client_peer, MSG_IDENTIFY_STDIN, -1, NULL, 0);
	proc_send(client_peer, MSG_IDENTIFY_STDOUT, -1, NULL, 0);
	if (client_tty_fd != -1) {
		proc_send(client_peer, MSG_IDENTIFY_TTYTOKEN, -1,
		    tty_token, strlen(tty_token) + 1);
	}

	pid = getpid();
	proc_send(client_peer, MSG_IDENTIFY_CLIENTPID, -1, &pid, sizeof pid);

	for (ss = environ; *ss != NULL; ss++) {
		sslen = strlen(*ss) + 1;
		if (sslen > MAX_IMSGSIZE - IMSG_HEADER_SIZE)
			continue;
		if (win32_remote_native_active() &&
		    !win32_remote_environ_allowed(*ss))
			continue;
		proc_send(client_peer, MSG_IDENTIFY_ENVIRON, -1, *ss, sslen);
	}

	proc_send(client_peer, MSG_IDENTIFY_DONE, -1, NULL, 0);

	{
		struct msg_resize_data	rd;
		int			cols = 80, rows = 24;

		win32_tty_get_size(&cols, &rows);
		rd.sx = cols;
		rd.sy = rows;
		proc_send(client_peer, MSG_RESIZE, -1, &rd, sizeof rd);
	}
}

static DWORD
client_run_shell(const char *shell, const char *shellcmd)
{
	STARTUPINFOW		 si;
	PROCESS_INFORMATION	 pi;
	struct tmux_os_error	 error;
	char			*cmd;
	wchar_t			*wide;
	DWORD			 code = 1;

	cmd = tmux_windows_shell_command(shell, shellcmd);
	if (cmd == NULL || (wide = tmux_win32_utf16(cmd, &error)) == NULL)
		fatalx("shell command line");
	free(cmd);
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
	if (!CreateProcessW(NULL, wide, NULL, NULL, TRUE, 0, NULL, NULL, &si,
	    &pi)) {
		log_debug("CreateProcess failed (%lu)", GetLastError());
		free(wide);
		return (1);
	}
	free(wide);
	CloseHandle(pi.hThread);
	WaitForSingleObject(pi.hProcess, INFINITE);
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	return (code);
}

/* Run command in shell; used for -c. */
static __dead void
client_exec(const char *shell, const char *shellcmd)
{
	log_debug("shell %s, command %s", shell, shellcmd);
	proc_clear_signals(client_proc, 1);
	setenv("SHELL", shell, 1);

	exit((int)client_run_shell(shell, shellcmd));
}

static void
client_tty_read_cb(evutil_socket_t fd, __unused short events,
    __unused void *arg)
{
	char	buf[8192];
	int	n;

	n = recv((SOCKET)fd, buf, sizeof buf, 0);
	if (n <= 0) {
		event_del(&client_tty_event);
		return;
	}
	win32_tty_relay_output((int)fd, buf, n);
}

static DWORD WINAPI
client_stdin_thread_func(LPVOID arg)
{
	int	fd = (int)(intptr_t)arg;
	HANDLE	h = GetStdHandle(STD_INPUT_HANDLE);
	char	buf[8192];
	DWORD	nread, mode, i, n;
	int	console = GetConsoleMode(h, &mode);
	int	control = (client_flags & CLIENT_CONTROL) != 0;

	while (client_tty_running) {
		if (!ReadFile(h, buf, sizeof buf, &nread, NULL))
			break;
		if (nread == 0 && console && GetConsoleMode(h, &mode) &&
		    (~mode & ENABLE_LINE_INPUT)) {
			buf[0] = 0x1a;
			nread = 1;
		}
		if (nread == 0)
			break;
		if (control && console) {
			for (i = 0, n = 0; i < nread; i++) {
				if (buf[i] == '\r') {
					if (i + 1 < nread && buf[i + 1] == '\n')
						continue;
					buf[n++] = '\n';
				} else
					buf[n++] = buf[i];
			}
			nread = n;
			if (nread == 0)
				continue;
		}
		if (win32_tty_relay_send(fd, buf, nread) != 0)
			break;
	}
	if (client_tty_running)
		shutdown((SOCKET)fd, SD_SEND);
	return (0);
}

static void
client_start_tty_relay(void)
{
	HANDLE	in = GetStdHandle(STD_INPUT_HANDLE);
	DWORD	mode;

	if (client_tty_fd == -1 || client_tty_running)
		return;

	if (~client_flags & CLIENT_CONTROL) {
		win32_tty_raw_mode();
	} else if ((client_flags & CLIENT_CONTROLCONTROL) &&
	    GetConsoleMode(in, &mode)) {
		win32_tty_control_mode();
	}

	setblocking(client_tty_fd, 0);

	event_set(&client_tty_event, client_tty_fd,
	    EV_READ|EV_PERSIST, client_tty_read_cb, NULL);
	event_add(&client_tty_event, NULL);

	client_tty_running = 1;
	client_tty_relayed = 1;
	client_stdin_thread_h = CreateThread(NULL, 0,
	    client_stdin_thread_func, (LPVOID)(intptr_t)client_tty_fd,
	    0, NULL);
}

static void
client_drain_tty_relay(void)
{
	char		 buf[8192];
	int		 n;
	fd_set		 ready;
	struct timeval	 tv;
	ULONGLONG	 deadline = GetTickCount64() + 3000, now;

	if (client_tty_fd == -1 || !client_tty_relayed)
		return;
	if (event_initialized(&client_tty_event))
		event_del(&client_tty_event);
	for (;;) {
		now = GetTickCount64();
		if (now >= deadline)
			break;
		tv.tv_sec = 0;
		tv.tv_usec = (long)((deadline - now) * 1000);
		FD_ZERO(&ready);
		FD_SET((SOCKET)client_tty_fd, &ready);
		if (select(0, &ready, NULL, NULL, &tv) <= 0)
			break;
		n = recv((SOCKET)client_tty_fd, buf, sizeof buf, 0);
		if (n <= 0)
			break;
		win32_tty_relay_output(client_tty_fd, buf, n);
	}
}

static void
client_stop_tty_relay(void)
{
	client_tty_running = 0;

	if (event_initialized(&client_tty_event))
		event_del(&client_tty_event);

	if (client_stdin_thread_h != NULL) {
		CancelIoEx(GetStdHandle(STD_INPUT_HANDLE), NULL);
		WaitForSingleObject(client_stdin_thread_h, 1000);
		CloseHandle(client_stdin_thread_h);
		client_stdin_thread_h = NULL;
	}

	if (client_tty_fd != -1) {
		closesocket((SOCKET)client_tty_fd);
		client_tty_fd = -1;
	}

	win32_tty_restore();
}

/* Callback to handle signals in the client. */
static void
client_signal(int sig)
{

	log_debug("%s: %s", __func__, strsignal(sig));
	if (sig == SIGCHLD) {
	} else if (!client_attached) {
		if (sig == SIGTERM || sig == SIGHUP)
			proc_exit(client_proc);
	} else {
		switch (sig) {
		case SIGHUP:
			client_exitreason = CLIENT_EXIT_LOST_TTY;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGTERM:
			if (!client_suspended)
				client_exitreason = CLIENT_EXIT_TERMINATED;
			client_exitval = 1;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		case SIGWINCH:
			{
				struct msg_resize_data	rd;
				int			cols = 80, rows = 24;

				win32_tty_get_size(&cols, &rows);
				rd.sx = cols;
				rd.sy = rows;
				proc_send(client_peer, MSG_RESIZE, -1,
				    &rd, sizeof rd);
			}
			break;
		}
	}
}

static int
client_remote_file_refused(struct imsg *imsg, int reply)
{
	struct msg_read_open	*msg = imsg->data;
	struct msg_read_done	 done;
	size_t			 msglen = imsg->hdr.len - IMSG_HEADER_SIZE;

	if (!win32_remote_native_active() || msglen < sizeof *msg ||
	    msg->fd != -1)
		return (0);
	done.stream = msg->stream;
	done.error = EACCES;
	proc_send(client_peer, reply, -1, &done, sizeof done);
	return (1);
}
/* Callback for file write error or close. */
static void
client_file_check_cb(__unused struct client *c, __unused const char *path,
    __unused int error, __unused int closed, __unused struct evbuffer *buffer,
    __unused void *data)
{
	if (client_exitflag)
		client_exit();
}

/* Callback for client read events. */
static void
client_dispatch(struct imsg *imsg, __unused void *arg)
{
	if (imsg == NULL) {
		if (!client_exitflag) {
			client_exitreason = CLIENT_EXIT_LOST_SERVER;
			client_exitval = 1;
		}
		proc_exit(client_proc);
		return;
	}

	if (client_attached)
		client_dispatch_attached(imsg);
	else
		client_dispatch_wait(imsg);
}

/* Process an exit message. */
static void
client_dispatch_exit_message(char *data, size_t datalen)
{
	int	retval;

	if (datalen < sizeof retval && datalen != 0)
		fatalx("bad MSG_EXIT size");

	if (datalen >= sizeof retval) {
		memcpy(&retval, data, sizeof retval);
		client_exitval = retval;
	}

	if (datalen > sizeof retval) {
		datalen -= sizeof retval;
		data += sizeof retval;

		client_exitmessage = xmalloc(datalen);
		memcpy(client_exitmessage, data, datalen);
		client_exitmessage[datalen - 1] = '\0';

		client_exitreason = CLIENT_EXIT_MESSAGE_PROVIDED;
	}
}

/* Dispatch imsgs when in wait state (before MSG_READY). */
static void
client_dispatch_wait(struct imsg *imsg)
{
	char		*data;
	ssize_t		 datalen;
	static int	 pledge_applied;

	/*
	 * "sendfd" is no longer required once all of the identify messages
	 * have been sent. We know the server won't send us anything until that
	 * point (because we don't ask it to), so we can drop "sendfd" once we
	 * get the first message from the server.
	 */
	if (!pledge_applied) {
		if (pledge(
		    "stdio rpath wpath cpath unix proc exec tty",
		    NULL) != 0)
			fatal("pledge failed");
		pledge_applied = 1;
	}

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_EXIT:
	case MSG_SHUTDOWN:
		client_dispatch_exit_message(data, datalen);
		client_exitflag = 1;
		client_exit();
		break;
	case MSG_READY:
		if (datalen != 0)
			fatalx("bad MSG_READY size");

		client_attached = 1;
		client_start_tty_relay();
		proc_send(client_peer, MSG_RESIZE, -1, NULL, 0);
		break;
	case MSG_VERSION:
		if (datalen != 0)
			fatalx("bad MSG_VERSION size");

		fprintf(stderr, "protocol version mismatch "
		    "(client %d, server %u)\n", PROTOCOL_VERSION,
		    imsg->hdr.peerid & 0xff);
		client_exitval = 1;
		proc_exit(client_proc);
		break;
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_SHELL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_SHELL string");

		if (win32_remote_native_active()) {
			fprintf(stderr, "a remote server cannot run a shell "
			    "here\n");
			client_exitval = 1;
			proc_exit(client_proc);
			break;
		}
		client_exec(data, shell_command);
		/* NOTREACHED */
	case MSG_DETACH:
	case MSG_DETACHKILL:
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		proc_exit(client_proc);
		break;
	case MSG_READ_OPEN:
		if (client_remote_file_refused(imsg, MSG_READ_DONE))
			break;
		file_read_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_READ_CANCEL:
		file_read_cancel(&client_files, imsg);
		break;
	case MSG_WRITE_OPEN:
		if (client_remote_file_refused(imsg, MSG_WRITE_READY))
			break;
		file_write_open(&client_files, client_peer, imsg, 1,
		    !(client_flags & CLIENT_CONTROL), client_file_check_cb,
		    NULL);
		break;
	case MSG_WRITE:
		file_write_data(&client_files, imsg);
		break;
	case MSG_WRITE_CLOSE:
		file_write_close(&client_files, imsg);
		break;
	case MSG_OLDSTDERR:
	case MSG_OLDSTDIN:
	case MSG_OLDSTDOUT:
		fprintf(stderr, "server version is too old for client\n");
		proc_exit(client_proc);
		break;
	default:
		log_debug("unknown message type %u", imsg->hdr.type);
		break;
	}
}

/* Dispatch imsgs in attached state (after MSG_READY). */
static void
client_dispatch_attached(struct imsg *imsg)
{
	struct sigaction	 sigact;
	char			*data;
	ssize_t			 datalen;
	const char		*shell;

	data = imsg->data;
	datalen = imsg->hdr.len - IMSG_HEADER_SIZE;

	switch (imsg->hdr.type) {
	case MSG_FLAGS:
		if (datalen != sizeof client_flags)
			fatalx("bad MSG_FLAGS string");

		memcpy(&client_flags, data, sizeof client_flags);
		log_debug("new flags are %#llx",
		    (unsigned long long)client_flags);
		break;
	case MSG_DETACH:
	case MSG_DETACHKILL:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_DETACH string");

		client_exitsession = xstrdup(data);
		client_exittype = imsg->hdr.type;
		if (imsg->hdr.type == MSG_DETACHKILL)
			client_exitreason = CLIENT_EXIT_DETACHED_HUP;
		else
			client_exitreason = CLIENT_EXIT_DETACHED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXEC:
		if (datalen == 0 || data[datalen - 1] != '\0' ||
		    strlen(data) + 1 == (size_t)datalen)
			fatalx("bad MSG_EXEC string");
		if (win32_remote_native_active()) {
			client_exitreason = CLIENT_EXIT_DETACHED;
			proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
			break;
		}
		client_execcmd = xstrdup(data);
		client_execshell = xstrdup(data + strlen(data) + 1);

		client_exittype = imsg->hdr.type;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXIT:
		client_dispatch_exit_message(data, datalen);
		if (client_exitreason == CLIENT_EXIT_NONE)
			client_exitreason = CLIENT_EXIT_EXITED;
		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		break;
	case MSG_EXITED:
		if (datalen != 0)
			fatalx("bad MSG_EXITED size");

		proc_exit(client_proc);
		break;
	case MSG_SHUTDOWN:
		if (datalen != 0)
			fatalx("bad MSG_SHUTDOWN size");

		proc_send(client_peer, MSG_EXITING, -1, NULL, 0);
		client_exitreason = CLIENT_EXIT_SERVER_EXITED;
		client_exitval = 1;
		break;
	case MSG_SUSPEND:
		if (datalen != 0)
			fatalx("bad MSG_SUSPEND size");

		proc_send(client_peer, MSG_WAKEUP, -1, NULL, 0);
		break;
	case MSG_LOCK:
		if (datalen == 0 || data[datalen - 1] != '\0')
			fatalx("bad MSG_LOCK string");

		shell = getenv("SHELL");
		if (!checkshell(shell))
			shell = _PATH_BSHELL;
		if (!win32_remote_native_active())
			client_run_shell(shell, data);
		proc_send(client_peer, MSG_UNLOCK, -1, NULL, 0);
		break;
	default:
		log_debug("unknown message type %u", imsg->hdr.type);
		break;
	}
}
