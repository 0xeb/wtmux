# wtmux tutorials

Step-by-step guides for wtmux. [README.md](README.md)
is the reference: requirements, build options and every difference from
tmux on Unix. tmux itself is documented in its manual page (`tmux.1`).

1. [Build and install](#1-build-and-install)
2. [A first session](#2-a-first-session)
3. [Configuration](#3-configuration)
4. [Over SSH](#4-over-ssh)
5. [Sessions on the desktop from SSH: wtmux-service](#5-sessions-on-the-desktop-from-ssh-wtmux-service)
6. [Remote attach without SSH: tmux://](#6-remote-attach-without-ssh-tmux)
7. [Sharing a session with another user](#7-sharing-a-session-with-another-user)
8. [When something goes wrong](#8-when-something-goes-wrong)

## 1. Build and install

You need Windows 10 1809 or later and Visual Studio 2022 or later with the C++ workload
(it includes CMake, Ninja and vcpkg). From a **Developer PowerShell (or
Command Prompt) for Visual Studio**, in the source tree:

~~~powershell
cmake --preset msvc-release
cmake --build --preset msvc-release
~~~

The first configure downloads and builds libevent and utf8proc through vcpkg,
and the ConPTY package from NuGet; later builds reuse them.

To install, from an **elevated** prompt:

~~~powershell
cmake --install out\build\msvc-release --prefix "C:\Program Files\wtmux"
~~~

This puts `tmux.exe`, `wtmux-service.exe`, `conpty.dll` and `<arch>\OpenConsole.exe`
in `C:\Program Files\wtmux\bin`. Add that directory to your `PATH` (System
Properties, Environment Variables), or call `tmux.exe` by its full path. Any
directory works for `tmux.exe` alone; the service insists on one that only
administrators can change, such as this one.

## 2. A first session

Open Windows Terminal (or any console) and run:

~~~
tmux new -s work
~~~

The commands and keys are tmux's own; the prefix is `C-b` (Ctrl+B):

| Keys | Does |
|---|---|
| `C-b %` / `C-b "` | Split left/right, top/bottom |
| `C-b` arrow | Move between panes |
| `C-b c`, `C-b n`, `C-b p` | New window, next, previous |
| `C-b d` | Detach (everything keeps running) |
| `C-b [` | Copy mode (scroll back; `q` leaves) |
| `C-b ?` | List every key binding |

After detaching, `tmux ls` lists the sessions and `tmux attach -t work`
returns to one. Closing the terminal window detaches too; the session lives on
in the tmux server until you `exit` its last pane or run `tmux kill-server`.

To make tmux the start of every Windows Terminal tab, add a profile whose
command line is:

~~~
"C:\Program Files\wtmux\bin\tmux.exe" new -A -s main
~~~

(`-A` attaches to `main` if it exists, else creates it.)

## 3. Configuration

tmux reads `%USERPROFILE%\.tmux.conf` (and `%ProgramData%\tmux\tmux.conf` for
every user), with the usual syntax. Panes run `%ComSpec%` (cmd.exe) unless
`default-shell` says otherwise. A starting point:

~~~
# PowerShell 7 in new panes. Backslash is tmux's escape character:
# write Windows paths with / or in single quotes.
set -g default-shell "C:/Program Files/PowerShell/7/pwsh.exe"

set -g mouse on
set -g history-limit 50000

# Split in the current pane's directory.
bind % split-window -h -c "#{pane_current_path}"
bind '"' split-window -v -c "#{pane_current_path}"
~~~

`tmux source-file ~/.tmux.conf` reloads it in a running server.
`#{pane_current_path}` follows cmd.exe and PowerShell when they report their
directory; see "Information tmux reads from a pane" in README.md for
the PowerShell prompt snippet that makes it exact.

## 4. Over SSH

With OpenSSH Server on the Windows machine (Settings, Optional features), tmux
works over SSH as on Unix:

~~~
ssh me@host
tmux new -s build        # start something long
                         # C-b d, or simply lose the connection
ssh me@host
tmux attach -t build     # it is still there
~~~

Or let tmux run ssh for you with `-H`, from your own machine:

~~~
tmux -H me@host new -s build      # a new session there, attached
tmux -H me@host ls
tmux -H me@host attach -t build
tmux -H me@host:2222 ...          # another port; host aliases from ~/.ssh/config work
~~~

`-H` goes with tmux's other global flags (`tmux -L work -H host attach`) and
passes everything else to the tmux on the other machine, which may run Windows
or Unix. Nothing new listens on the network: it is your ssh, with your keys and
`~/.ssh/config`. When an argument needs quoting, tmux asks the host once which
kind of shell it has and remembers it in `%LOCALAPPDATA%\tmux\remote-hosts`.
`TMUX_SSH_COMMAND` gives another ssh command line (`ssh -i C:\keys\work`), as
`GIT_SSH_COMMAND` does for git.

The server outlives the SSH connection. One thing differs from a desktop:
everything started over SSH runs in Windows **session 0**, which has no visible
desktop. Console programs are fine; a graphical program started in such a pane
runs where no screen will ever show it, and with a key logon your network
credentials are not available either. The next tutorial is the way around that,
and `tmux -H host new` uses it on its own when the host runs wtmux-service.

## 5. Sessions on the desktop from SSH: wtmux-service

`wtmux-service.exe` is a small Windows service that starts tmux **inside a
logged-on user's own session**, as if that user had started it at the
keyboard. Programs in its panes are on that user's desktop: `notepad` opens a
window there, the clipboard and mapped drives are the user's, and network
shares use the user's logon. Attaching needs nothing special, because a user's
tmux socket is the same in every session.

### Install

From an elevated prompt (after step 1):

~~~
"C:\Program Files\wtmux\bin\wtmux-service.exe" install
~~~

It installs and starts the service `wtmux-service` (automatic start, runs as
LocalSystem). It refuses to install from a directory that non-administrators
can change, since the service starts the programs beside it. To remove it:
`wtmux-service uninstall`.

### Use it for yourself

You are away from the machine; it is locked, with you logged on. Over SSH:

~~~
wtmux-service ls
wtmux-service new -s desk
tmux attach -t desk
~~~

`ls` shows the logged-on users' Windows sessions (active, locked or
disconnected) and your tmux sessions. `new` starts `desk` in your desktop
session; `tmux attach` reaches it from SSH. Run `notepad report.txt`
in it: it opens on your desktop and is there when you unlock the machine
(while locked, windows exist but are not drawn).

`new` takes tmux's usual choices: `-s name`, `-L socket-name`, and a command
after `--`:

~~~
wtmux-service new -s notes -- notepad report.txt
~~~

### Open a window on the desktop too

`-w` also opens a terminal attached to the new session on that desktop:
Windows Terminal if the user has it, otherwise a console window.

~~~
wtmux-service new -w -s shared
~~~

Whoever sits at the machine sees the session; you see the same one over SSH.

### For another user (administrators)

An administrator (elevated; SSH sessions of administrators are) may start a
session for any logged-on user. The reply names the socket, and the
administrator is let into that user's server:

~~~
wtmux-service new -w -u alice -s help
  started help for HOST\alice in Windows session 2 (active)
  socket: C:/Users/alice/AppData/Local/Temp/tmux-1004/default
  attach: tmux -S "C:/Users/alice/AppData/Local/Temp/tmux-1004/default" attach -t "help"
~~~

A terminal opens on Alice's screen; the administrator attaches with the
`attach` line. Both type into the same session, and everything in it runs as
Alice, not elevated. A user who is not logged on is refused: there is no
desktop to start on.

### Good to know

- **"the tmux server for this socket is already running in Windows session 0"**:
  you already have a tmux server, started over SSH, for that socket name; the
  new session would go to it, off the desktop. Use another socket name
  (`wtmux-service new -L desk ...`, then `tmux -L desk attach`) or end that
  server with `tmux kill-server`.
- **"permission denied: only an administrator (elevated)..."**: asking for
  another user needs an elevated administrator; a non-elevated one is treated
  as a standard user.
- `-i <id>` picks one of a user's sessions when they have several (see `ls`).
- Every request is logged to `%ProgramData%\wtmux-service\service.log`
  (administrators only) and to the Application event log (source
  `wtmux-service`).
- The service listens only on a local pipe and rejects remote pipe clients;
  remote use is through SSH, which does the authentication, unless you turn
  on its own listener (next tutorial).

## 6. Remote attach without SSH: tmux://

If the Windows machine you want to reach has no SSH server, wtmux-service can
let tmux clients in by itself, over TLS, after a Windows or key logon. Prefer
SSH where you have it; this is for where you do not. The client must be
Windows too.

### On the host (elevated, once)

~~~
wtmux-service install                    # if it is not installed yet
wtmux-service listen -a 0.0.0.0          # every address; plain "listen" is loopback only
netsh advfirewall firewall add rule name=wtmux-service dir=in action=allow protocol=TCP localport=7380
wtmux-service fingerprint                # note it: SHA256:...
~~~

`listen` prints the `netsh` line itself; it never opens the firewall on its
own. `-p` picks another port, `-auth keys` or `-auth windows` allows one kind
of logon only, `-no-ntlm` accepts Windows logons by Kerberos only.
`wtmux-service listen off` closes the port again.

### From your machine

~~~
tmux -H tmux://alice@host attach -t work    # as alice: her password is asked
tmux -H tmux://host ls                      # as yourself (in a domain: no password)
tmux -H tmux://alice@host new -s build      # a new session on alice's desktop
tmux -H tmux://alice@host:7381 ...          # another port
~~~

The first time, tmux shows the host's fingerprint and asks whether to trust
it; compare it with the one `wtmux-service fingerprint` printed. It is then
kept in `%LOCALAPPDATA%\tmux\known_hosts`. If the host's key changes later
(wtmux-service reinstalled, or someone in the middle), tmux refuses and says
so; remove the old line once you know why.

Everything else is as for a local client, as alice: her sessions, her
`server-access` rules (`-S` with another user's socket path works if that
user let alice in), and a server started this way runs on her desktop,
so she must be logged on there to start one.

### With a key instead of a password

On your machine, at your desktop (not over SSH: Windows protects the key with
your logon password):

~~~
wtmux-service keygen         # once; prints a line: tmux-ecdsa-p256 BHjQ... you@pc
~~~

On the host, as alice, put that line in `%USERPROFILE%\.tmux\authorized_keys`.
The folder and file must be alice's alone (plus SYSTEM and Administrators),
as they are when she creates them in her profile. Then
`tmux -H tmux://alice@host attach` logs on without asking. As with an SSH
key, such a logon has no password, so network shares that need alice's
credentials do not open from it.

### Good to know

- The tmux on the host sees only your terminal type and locale, not your
  other environment variables or current directory, and cannot open files on
  your machine: `save-buffer -` and `load-buffer -` work, a path is refused.
  Nor does it run programs on your machine: `lock-client` does not run the
  lock command, and `detach-client -E` only detaches. Your clipboard is not
  shared.
- Failed logons slow down further tries from that address or for that
  account; logons and failures are in `%ProgramData%\wtmux-service\service.log`
  and the event log.
- A logon must finish within two minutes of connecting.

## 7. Sharing a session with another user

The owner of a server lets another Windows user in, read-write or read-only:

~~~
tmux server-access -a OTHERUSER          # or DOMAIN\user
tmux server-access -a -r OTHERUSER       # read-only
tmux display -p "#{socket_path}"         # tell them this path
~~~

The other user attaches by that path:

~~~
tmux -S C:/Users/owner/AppData/Local/Temp/tmux-1001/default attach
~~~

`tmux server-access -d OTHERUSER` removes them again (and detaches them).

## 8. When something goes wrong

- **Garbled drawing or keys** in the system console host: the bundled ConPTY
  is the default when the build has it; `TMUX_WINDOWS_CONPTY=package` (or
  `inbox`) in the environment of the client that starts the server picks one
  explicitly. Windows 10's own console host has known limits, listed in
  README.md.
- **A program's output is mangled** by the console: run it in a pane created
  with `new-window -e TMUX_WINDOWS_PANE=pipe ...`, which gives it plain pipes.
- **Logs**: `tmux -vv new` writes `tmux-server-<pid>.log` and
  `tmux-client-<pid>.log` to the current directory. A running server toggles
  its log on the named event `Local\tmux-<pid>-USR2` (`#{pid}`):
  `[Threading.EventWaitHandle]::OpenExisting("tmux-$pid-USR2").Set()` in
  PowerShell.
- **"no server running on ..."**: the server for that socket name is not
  running (or was started by another user); `tmux ls` and `tmux -L name ls`
  check, and the socket files are in `%LOCALAPPDATA%\Temp\tmux-<uid>\`.
