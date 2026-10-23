/*
 * pam_xefp.so — PAM auth module: accept a fingerprint from the Goodix MOC sensor
 * by running the xe-fp helper ("xe-fp verify --user <user>").
 *
 * Intended use (macOS), in /etc/pam.d/sudo_local, ABOVE nothing else needed:
 *   auth  sufficient  /usr/local/lib/pam/pam_xefp.so timeout=10
 *
 * Options:
 *   timeout=N     seconds to wait for a finger (default 10, 1..60)
 *   helper=PATH   helper binary (default /usr/local/libexec/xefp/xe-fp)
 *   allow_remote  also try when PAM_RHOST / SSH_CONNECTION is set (default: skip)
 *   debug         syslog details
 *
 * Return values: PAM_SUCCESS on match; PAM_AUTH_ERR on "no match";
 * PAM_IGNORE when the helper/device is unavailable, times out, or the session is remote.
 * With "sufficient", anything but PAM_SUCCESS falls through to the normal password prompt.
 *
 * Hardening: the module runs inside setuid sudo. It refuses a helper that is not a
 * root-owned, non-group/world-writable regular file in a root-owned, non-writable
 * directory; the helper is exec'd with a minimal, fixed environment and no user fds.
 */
#include <security/pam_modules.h>
#include <security/pam_appl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#define DEFAULT_HELPER "/usr/local/libexec/xefp/xe-fp"

static int dbg;

static void logx (int prio, const char *fmt, const char *a)
{
  if (prio == LOG_DEBUG && !dbg) return;
  syslog (LOG_AUTHPRIV | prio, fmt, a ? a : "");
}

static int secure_path (const char *path)
{
  struct stat st;
  char buf[1024], *dir;

  if (stat (path, &st) != 0 || !S_ISREG (st.st_mode) || st.st_uid != 0 ||
      (st.st_mode & (S_IWGRP | S_IWOTH)) || !(st.st_mode & S_IXUSR))
    return 0;
  /* every parent directory must be root-owned and not group/world writable */
  strncpy (buf, path, sizeof buf - 1);
  buf[sizeof buf - 1] = 0;
  for (dir = dirname (buf); ; dir = dirname (dir))
    {
      if (stat (dir, &st) != 0 || !S_ISDIR (st.st_mode) || st.st_uid != 0 ||
          (st.st_mode & (S_IWGRP | S_IWOTH)))
        return 0;
      if (strcmp (dir, "/") == 0) break;
    }
  return 1;
}

static int valid_user (const char *u)
{
  if (!u || !*u || strlen (u) > 64 || u[0] == '-') return 0;
  for (const char *p = u; *p; p++)
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
      return 0;
  return 1;
}

static void say (pam_handle_t *pamh, const char *msg)
{
  const struct pam_conv *conv = NULL;
  struct pam_message m = { PAM_TEXT_INFO, msg };
  const struct pam_message *mp = &m;
  struct pam_response *resp = NULL;

  if (pam_get_item (pamh, PAM_CONV, (const void **) &conv) == PAM_SUCCESS && conv && conv->conv)
    {
      conv->conv (1, &mp, &resp, conv->appdata_ptr);
      if (resp) { free (resp->resp); free (resp); }
    }
}

PAM_EXTERN int
pam_sm_authenticate (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  const char *user = NULL, *rhost = NULL, *helper = DEFAULT_HELPER;
  int timeout = 10, allow_remote = 0, status = 0;
  char tbuf[16];
  pid_t pid;

  for (int i = 0; i < argc; i++)
    {
      if (!strncmp (argv[i], "timeout=", 8)) timeout = atoi (argv[i] + 8);
      else if (!strncmp (argv[i], "helper=", 7)) helper = argv[i] + 7;
      else if (!strcmp (argv[i], "allow_remote")) allow_remote = 1;
      else if (!strcmp (argv[i], "debug")) dbg = 1;
    }
  if (timeout < 1 || timeout > 60) timeout = 10;

  if (pam_get_user (pamh, &user, NULL) != PAM_SUCCESS || !valid_user (user))
    return PAM_IGNORE;

  if (!allow_remote)
    {
      if ((pam_get_item (pamh, PAM_RHOST, (const void **) &rhost) == PAM_SUCCESS && rhost && *rhost) ||
          getenv ("SSH_CONNECTION") || getenv ("SSH_TTY"))
        {
          logx (LOG_DEBUG, "pam_xefp: remote session, skipping%s", NULL);
          return PAM_IGNORE;
        }
    }

  if (!secure_path (helper))
    {
      logx (LOG_ERR, "pam_xefp: helper %s missing or not root-owned/secure; skipping", helper);
      return PAM_IGNORE;
    }

  snprintf (tbuf, sizeof tbuf, "%d", timeout);
  {
    /* sudo authenticates with PAM_SILENT (sudoers pam_silent, on by default), so a PAM_TEXT_INFO never shows and the
       wait for the finger went unnoticed. Write the prompt (with a bell) straight to the controlling terminal; only
       without one fall back to the conversation function. */
    char msg[128];
    int tty = open ("/dev/tty", O_WRONLY | O_NOCTTY | O_CLOEXEC);
    snprintf (msg, sizeof msg, "Touch the fingerprint sensor (or wait %d s for the password prompt)", timeout);
    if (tty >= 0)
      {
        char line[160];
        int n = snprintf (line, sizeof line, "\a\xf0\x9f\x91\x86 %s\n", msg);
        if (n > 0) (void) write (tty, line, (size_t) n);
        close (tty);
      }
    else if (!(flags & PAM_SILENT))
      say (pamh, msg);
  }

  pid = fork ();
  if (pid < 0)
    return PAM_IGNORE;
  if (pid == 0)
    {
      char *const env[] = { "PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/var/root", NULL };
      char *const av[] = { (char *) helper, "verify", "--quiet", "--user", (char *) user,
                           "--timeout", tbuf, NULL };
      int fd = open ("/dev/null", O_RDWR);
      if (fd >= 0) { dup2 (fd, 0); dup2 (fd, 1); if (!dbg) dup2 (fd, 2); }
      for (int f = 3; f < 256; f++) close (f);
      if (geteuid () == 0) { setgid (0); setuid (0); }
      execve (helper, av, env);
      _exit (127);
    }

  while (waitpid (pid, &status, 0) < 0)
    if (errno != EINTR) return PAM_IGNORE;

  if (WIFEXITED (status))
    {
      switch (WEXITSTATUS (status))
        {
        case 0:
          logx (LOG_NOTICE, "pam_xefp: fingerprint accepted for %s", user);
          return PAM_SUCCESS;
        case 1:
          logx (LOG_NOTICE, "pam_xefp: fingerprint NOT matched for %s", user);
          {
            int tty = open ("/dev/tty", O_WRONLY | O_NOCTTY | O_CLOEXEC);
            if (tty >= 0) { static const char m[] = "Fingerprint not recognized.\n"; (void) write (tty, m, sizeof m - 1); close (tty); }
            else if (!(flags & PAM_SILENT)) say (pamh, "Fingerprint not recognized.");
          }
          return PAM_AUTH_ERR;
        default:
          logx (LOG_DEBUG, "pam_xefp: helper unavailable/timeout for %s", user);
          return PAM_IGNORE;
        }
    }
  return PAM_IGNORE;
}

PAM_EXTERN int
pam_sm_setcred (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  return PAM_SUCCESS;
}

PAM_EXTERN int
pam_sm_acct_mgmt (pam_handle_t *pamh, int flags, int argc, const char **argv)
{
  return PAM_IGNORE;
}
