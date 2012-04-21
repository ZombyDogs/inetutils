/*
  Copyright (C) 1993, 1994, 1995, 1996, 1997, 1998, 1999, 2000, 2001,
  2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012 Free
  Software Foundation, Inc.

  This file is part of GNU Inetutils.

  GNU Inetutils is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or (at
  your option) any later version.

  GNU Inetutils is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see `http://www.gnu.org/licenses/'. */

/*
 * Copyright (c) 1983, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* TODO: Implement PAM support as `rexec' service.
 *       Pending with Mats Erik Andersson.
 */

#include <config.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include <netinet/in.h>

#include <errno.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <grp.h>
#ifdef HAVE_CRYPT_H
# include <crypt.h>
#endif
#include <sys/select.h>
#include <stdarg.h>
#ifdef HAVE_SHADOW_H
# include <shadow.h>
#endif
#include <syslog.h>
#include <progname.h>
#include <argp.h>
#include <error.h>
#include "libinetutils.h"

void die (int code, const char *fmt, ...);
int doit (int f, struct sockaddr *fromp, socklen_t fromlen);

static int logging = 0;

static struct argp_option options[] = {
  { "logging", 'l', NULL, 0, "logging of requests and errors" },
  { NULL }
};

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'l':
      ++logging;
      break;

    default:
      return ARGP_ERR_UNKNOWN;
    }
  return 0;
}

const char doc[] = "remote execution daemon";

static struct argp argp = {
  options,
  parse_opt,
  NULL,
  doc,
  NULL,
  NULL,
  NULL
};

/*
 * remote execute server:
 *	username\0
 *	password\0
 *	command\0
 *	data
 */
int
main (int argc, char **argv)
{
  struct sockaddr_storage from;
  socklen_t fromlen;
  int sockfd = STDIN_FILENO;
  int index;

  set_program_name (argv[0]);

  iu_argp_init ("rexecd", default_program_authors);
  argp_parse (&argp, argc, argv, 0, &index, NULL);

  openlog ("rexecd", LOG_PID | LOG_ODELAY | LOG_NOWAIT, LOG_DAEMON);

  if (argc > index)
    /* Record this complaint locally.  */
    syslog (LOG_NOTICE, "%d extra arguments", argc - index);

  fromlen = sizeof (from);
  if (getpeername (sockfd, (struct sockaddr *) &from, &fromlen) < 0)
    {
      syslog (LOG_ERR, "getpeername: %m");
      error (EXIT_FAILURE, errno, "getpeername");
    }

  doit (sockfd, (struct sockaddr *) &from, fromlen);
  exit (EXIT_SUCCESS);
}

/* Set lengths of USER and LOGNAME longer than
 * the REXEC protocol prescribes.  */
char username[32 + sizeof ("USER=")] = "USER=";
char logname[32 + sizeof ("LOGNAME=")] = "LOGNAME=";	/* Identical to USER.  */
char homedir[256 + sizeof ("HOME=")] = "HOME=";
char shell[256 + sizeof ("SHELL=")] = "SHELL=";
char path[sizeof (PATH_DEFPATH) + sizeof ("PATH=")] = "PATH=";
char *envinit[] = { homedir, shell, path, username, logname, NULL };
extern char **environ;

char *getstr (const char *);

static char *
get_user_password (struct passwd *pwd)
{
  char *pw_text = pwd->pw_passwd;
#ifdef HAVE_SHADOW_H
  struct spwd *spwd = getspnam (pwd->pw_name);
  if (spwd)
    pw_text = spwd->sp_pwdp;
#endif
  return pw_text;
}

int
doit (int f, struct sockaddr *fromp, socklen_t fromlen)
{
  char *cmdbuf, *cp, *namep;
  char *user, *pass, *pw_password;
  struct passwd *pwd;
  char rhost[INET6_ADDRSTRLEN];
  int s, ret;
  in_port_t port;
  int pv[2], pid, cc;
  fd_set readfrom, ready;
  char buf[BUFSIZ], sig;
  int one = 1;

  signal (SIGINT, SIG_DFL);
  signal (SIGQUIT, SIG_DFL);
  signal (SIGTERM, SIG_DFL);
#ifdef DEBUG
  {
    int t = open (_PATH_TTY, O_RDWR);
    if (t >= 0)
      {
	ioctl (t, TIOCNOTTY, (char *) 0);
	close (t);
      }
  }
#endif
  if (f != STDIN_FILENO)
    {
      dup2 (f, STDIN_FILENO);
      dup2 (f, STDOUT_FILENO);
      dup2 (f, STDERR_FILENO);
    }

  ret = getnameinfo (fromp, fromlen, rhost, sizeof (rhost),
		     NULL, 0, NI_NUMERICHOST);
  if (ret)
    {
      syslog (LOG_WARNING, "getnameinfo: %m");
      strncpy (rhost, "(unknown)", sizeof (rhost));
    }

  if (logging > 1)
    syslog (LOG_INFO, "request from \"%s\"", rhost);

  alarm (60);
  port = 0;
  for (;;)
    {
      char c;
      if (read (f, &c, 1) != 1)
	{
	  if (logging)
	    syslog (LOG_ERR, "main socket: %m");
	  exit (EXIT_FAILURE);
	}
      if (c == 0)
	break;
      port = port * 10 + c - '0';
    }
  alarm (0);

  if (port != 0)
    {
      s = socket (fromp->sa_family, SOCK_STREAM, 0);
      if (s < 0)
	{
	  if (logging)
	    syslog (LOG_ERR, "stderr socket: %m");
	  exit (EXIT_FAILURE);
	}
      setsockopt (s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
      alarm (60);
      switch (fromp->sa_family)
	{
	case AF_INET:
	  ((struct sockaddr_in *) fromp)->sin_port = htons (port);
	  break;
	case AF_INET6:
	  ((struct sockaddr_in6 *) fromp)->sin6_port = htons (port);
	  break;
	default:
	  syslog (LOG_ERR, "unknown address family %d", fromp->sa_family);
	  exit (EXIT_FAILURE);
	}
      if (connect (s, fromp, fromlen) < 0)
	{
	  /* Use LOG_NOTICE since the remote part might cause
	   * this error by blocking.  We are less probable.
	   */
	  if (logging)
	    syslog (LOG_NOTICE, "connect: %m");
	  exit (EXIT_FAILURE);
	}
      alarm (0);
    }

  user = getstr ("username");
  pass = getstr ("password");
  cmdbuf = getstr ("command");

  setpwent ();

  pwd = getpwnam (user);
  if (pwd == NULL)
    {
      if (logging)
	syslog (LOG_WARNING, "no user named \"%s\"", user);
      die (EXIT_FAILURE, "Login incorrect.");
    }

  endpwent ();

  /* Last need of elevated privilege.  */
  pw_password = get_user_password (pwd);
  if (*pw_password != '\0')
    {
      namep = crypt (pass, pw_password);
      if (strcmp (namep, pw_password))
	{
	  if (logging)
	    syslog (LOG_WARNING, "password failure for \"%s\"", user);
	  die (EXIT_FAILURE, "Password incorrect.");
	}
    }

  /* Step down from superuser personality.
   *
   * The changing of group membership will seldomly
   * fail, but a relevant message is passed just in
   * case.  These messages are non-standard.
   */
  if (setgid ((gid_t) pwd->pw_gid) < 0)
    {
      syslog (LOG_DEBUG, "setgid(gid = %d): %m", pwd->pw_gid);
      die (EXIT_FAILURE, "Failed group protections.");
    }

#ifdef HAVE_INITGROUPS
  if (initgroups (pwd->pw_name, pwd->pw_gid) < 0)
    {
      syslog (LOG_DEBUG, "initgroups(%s, %s): %m",
	      pwd->pw_name, pwd->pw_gid);
      die (EXIT_FAILURE, "Failed group protections.");
    }
#endif

  if (setuid ((uid_t) pwd->pw_uid) < 0)
    {
      syslog (LOG_DEBUG, "setuid(uid = %d): %m", pwd->pw_uid);
      die (EXIT_FAILURE, "Failed user identity.");
    }

  if (port)
    {
      pipe (pv);
      pid = fork ();
      if (pid == -1)
	{
	  if (logging)
	    syslog (LOG_ERR, "forking for \"%s\": %m", user);
	  die (EXIT_FAILURE, "Try again.");
	}

      if (pid)
	{
	  close (STDIN_FILENO);
	  close (STDOUT_FILENO);
	  close (STDERR_FILENO);
	  close (f);
	  close (pv[1]);
	  FD_ZERO (&readfrom);
	  FD_SET (s, &readfrom);
	  FD_SET (pv[0], &readfrom);
	  ioctl (pv[1], FIONBIO, (char *) &one);
	  /* should set s nbio! */
	  do
	    {
	      int maxfd = s;
	      ready = readfrom;
	      if (pv[0] > maxfd)
		maxfd = pv[0];
	      select (maxfd + 1, (fd_set *) & ready,
		      (fd_set *) NULL, (fd_set *) NULL,
		      (struct timeval *) NULL);
	      if (FD_ISSET (s, &ready))
		{
		  if (read (s, &sig, 1) <= 0)
		    FD_CLR (s, &readfrom);
		  else
		    killpg (pid, sig);
		}
	      if (FD_ISSET (pv[0], &ready))
		{
		  cc = read (pv[0], buf, sizeof (buf));
		  if (cc <= 0)
		    {
		      shutdown (s, 1 + 1);
		      FD_CLR (pv[0], &readfrom);
		    }
		  else
		    write (s, buf, cc);
		}
	    }
	  while (FD_ISSET (pv[0], &readfrom) || FD_ISSET (s, &readfrom));
	  exit (EXIT_SUCCESS);
	}
#ifdef HAVE_SETPGID
      setpgid (0, getpid ());
#endif
      close (s);
      close (pv[0]);
      dup2 (pv[1], STDERR_FILENO);
    }

  if (f > 2)
    close (f);

  /* Last point of failure due to incorrect user settings.  */
  if (chdir (pwd->pw_dir) < 0)
    {
      if (logging)
	syslog (LOG_NOTICE, "\"%s\" uses invalid \"%s\"", user, pwd->pw_dir);
      die (EXIT_FAILURE, "No remote directory.");
    }

  strcat (path, PATH_DEFPATH);
  environ = envinit;
  strncat (homedir, pwd->pw_dir, sizeof (homedir) - sizeof ("HOME=") - 1);
  strncat (shell, pwd->pw_shell, sizeof (shell) - sizeof ("SHELL=") - 1);
  strncat (username, pwd->pw_name, sizeof (username) - sizeof ("USER=") - 1);
  strncat (logname, pwd->pw_name, sizeof (logname) - sizeof ("LOGNAME=") - 1);

  if (*pwd->pw_shell == '\0')
    pwd->pw_shell = PATH_BSHELL;

  cp = strrchr (pwd->pw_shell, '/');
  if (cp)
    cp++;
  else
    cp = pwd->pw_shell;

  /* This is the 7th step in the protocol standard.
   * All authentication has been successful, and the
   * execution can be handed over to the requested shell.
   */
  write (STDERR_FILENO, "\0", 1);
  if (logging)
    syslog (LOG_INFO, "accepted user \"%s\" from %s", user, rhost);

  execl (pwd->pw_shell, cp, "-c", cmdbuf, NULL);
  if (logging)
    syslog (LOG_ERR, "execl fails for \"%s\": %m", user);
  error (EXIT_FAILURE, errno, "executing %s", pwd->pw_shell);

  return -1;
}

void
die (int code, const char *fmt, ...)
{
  va_list ap;
  char buf[BUFSIZ];
  int n;

  va_start (ap, fmt);
  buf[0] = 1;
  n = vsnprintf (buf + 1, sizeof buf - 1, fmt, ap);
  va_end (ap);
  if (n > sizeof buf - 1)
    n = sizeof buf - 1;
  buf[n++] = '\n';
  write (STDERR_FILENO, buf, n);
  exit (code);
}

char *
getstr (const char *err)
{
  size_t buf_len = 100;
  char *buf = malloc (buf_len), *end = buf;

  if (!buf)
    die (EXIT_FAILURE, "Out of space reading %s", err);

  do
    {
      /* Oh this is efficient, oh yes.  [But what can be done?] */
      int rd = read (STDIN_FILENO, end, 1);
      if (rd <= 0)
	{
	  if (rd == 0)
	    die (EXIT_FAILURE, "EOF reading %s", err);
	  else
	    error (EXIT_FAILURE, 0, "%s", err);
	}

      end += rd;
      if ((buf + buf_len - end) < (buf_len >> 3))
	{
	  /* Not very much room left in our buffer, grow it. */
	  size_t end_offs = end - buf;
	  buf_len += buf_len;
	  buf = realloc (buf, buf_len);
	  if (!buf)
	    die (EXIT_FAILURE, "Out of space reading %s", err);
	  end = buf + end_offs;
	}
    }
  while (*(end - 1));

  return buf;
}
