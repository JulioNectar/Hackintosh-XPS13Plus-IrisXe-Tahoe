/*
 * xe-fp — minimal libfprint CLI for the Goodix MOC sensor (27c6:63bc) on macOS.
 *
 *   xe-fp list                         device info + prints stored on the chip
 *   xe-fp enroll <finger> [--user U]   enroll a finger (stored ON the sensor)
 *   xe-fp verify [--user U] [--timeout S]   match against U's xe-fp prints (exit 0 = match)
 *   xe-fp identify [--timeout S]       match against ALL prints on the chip (incl. foreign/Windows)
 *   xe-fp delete <finger|#index> [--user U]   delete ONE print created by xe-fp for U
 *
 * Safety: this tool never calls clear_storage; delete refuses prints not created by
 * libfprint/xe-fp (e.g. Windows Hello templates) unless --force-foreign is given.
 * Exit codes: 0 ok/match, 1 no match / not found, 2 error, 3 timeout/cancelled,
 *             4 no prints enrolled for the user.
 */
#include <fprint.h>
#include <glib.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <time.h>

static const struct { const char *name; FpFinger f; } fingers[] = {
  { "left-thumb", FP_FINGER_LEFT_THUMB }, { "left-index", FP_FINGER_LEFT_INDEX },
  { "left-middle", FP_FINGER_LEFT_MIDDLE }, { "left-ring", FP_FINGER_LEFT_RING },
  { "left-little", FP_FINGER_LEFT_LITTLE }, { "right-thumb", FP_FINGER_RIGHT_THUMB },
  { "right-index", FP_FINGER_RIGHT_INDEX }, { "right-middle", FP_FINGER_RIGHT_MIDDLE },
  { "right-ring", FP_FINGER_RIGHT_RING }, { "right-little", FP_FINGER_RIGHT_LITTLE },
};

static const char *finger_name (FpFinger f)
{
  for (guint i = 0; i < G_N_ELEMENTS (fingers); i++)
    if (fingers[i].f == f) return fingers[i].name;
  return "unknown";
}

static FpFinger finger_parse (const char *s)
{
  for (guint i = 0; i < G_N_ELEMENTS (fingers); i++)
    if (g_ascii_strcasecmp (fingers[i].name, s) == 0) return fingers[i].f;
  return FP_FINGER_UNKNOWN;
}

/* Prints created by libfprint carry a user-id "FP1-YYYYMMDD-F-RAND-username" which
 * fills username/finger. Anything else (e.g. Windows) has no username. */
static gboolean print_is_ours (FpPrint *p)
{
  const char *d = fp_print_get_description (p);
  return d && g_str_has_prefix (d, "FP1-") && fp_print_get_username (p) != NULL;
}

static gboolean print_belongs_to (FpPrint *p, const char *user)
{
  return print_is_ours (p) && g_strcmp0 (fp_print_get_username (p), user) == 0;
}

static GCancellable *cancellable;
static gboolean timed_out;

static gboolean on_sigint (gpointer d) { g_cancellable_cancel (cancellable); return G_SOURCE_CONTINUE; }
static gboolean on_timeout (gpointer d) { timed_out = TRUE; g_cancellable_cancel (cancellable); return G_SOURCE_REMOVE; }

/* The *_sync API iterates the default main context, so these sources fire during sync calls. */
static void arm_timeout (int seconds)
{
  if (seconds > 0) g_timeout_add_seconds (seconds, on_timeout, NULL);
}

static FpDevice *open_device (FpContext *ctx, GError **error)
{
  GPtrArray *devs = fp_context_get_devices (ctx);
  if (!devs || devs->len == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no supported fingerprint device found");
      return NULL;
    }
  FpDevice *dev = g_object_ref (g_ptr_array_index (devs, 0));
  if (!fp_device_open_sync (dev, cancellable, error))
    {
      g_object_unref (dev);
      return NULL;
    }
  return dev;
}

static void print_line (guint idx, FpPrint *p)
{
  const GDate *date = fp_print_get_enroll_date (p);
  char dbuf[32] = "-";
  if (date && g_date_valid (date)) g_date_strftime (dbuf, sizeof dbuf, "%Y-%m-%d", date);
  printf ("  #%u  %-13s user=%-12s date=%s  %s  id=\"%s\"\n", idx,
          finger_name (fp_print_get_finger (p)),
          fp_print_get_username (p) ? fp_print_get_username (p) : "-",
          dbuf, print_is_ours (p) ? "[libfprint]" : "[FOREIGN e.g. Windows]",
          fp_print_get_description (p) ? fp_print_get_description (p) : "");
}

static void enroll_progress (FpDevice *dev, gint stage, FpPrint *p, gpointer ud, GError *err)
{
  if (err)
    printf ("  stage %d/%d: retry (%s) — lift and place the finger again\n", stage,
            fp_device_get_nr_enroll_stages (dev), err->message);
  else
    printf ("  stage %d/%d ok — lift and place the finger again\n", stage, fp_device_get_nr_enroll_stages (dev));
  fflush (stdout);
}

static void match_cb (FpDevice *dev, FpPrint *match, FpPrint *print, gpointer ud, GError *err)
{
  if (err) printf ("  scan problem: %s (try again)\n", err->message);
}

static const char *current_user (void)
{
  struct passwd *pw = getpwuid (getuid ());
  return pw ? pw->pw_name : g_get_user_name ();
}

static int usage (void)
{
  fprintf (stderr,
           "usage: xe-fp list [--debug]\n"
           "       xe-fp enroll <finger> [--user U]\n"
           "       xe-fp verify   [--user U] [--timeout S] [--tries N] [--quiet]\n"
           "       xe-fp identify [--timeout S] [--tries N]\n"
           "       xe-fp delete <finger|#index> [--user U] [--force-foreign]\n"
           "fingers: left|right-thumb|index|middle|ring|little\n");
  return 2;
}

int main (int argc, char **argv)
{
  const char *cmd, *arg = NULL, *user = NULL;
  int timeout = 0, quiet = 0, force_foreign = 0, debug = 0, tries = 3;
  g_autoptr(GError) error = NULL;

  if (argc < 2) return usage ();
  cmd = argv[1];
  for (int i = 2; i < argc; i++)
    {
      if (!strcmp (argv[i], "--user") && i + 1 < argc) user = argv[++i];
      else if (!strcmp (argv[i], "--timeout") && i + 1 < argc) timeout = atoi (argv[++i]);
      else if (!strcmp (argv[i], "--tries") && i + 1 < argc) tries = MAX (1, atoi (argv[++i]));
      else if (!strcmp (argv[i], "--quiet")) quiet = 1;
      else if (!strcmp (argv[i], "--debug")) debug = 1;
      else if (!strcmp (argv[i], "--force-foreign")) force_foreign = 1;
      else if (argv[i][0] != '-' && !arg) arg = argv[i];
      else return usage ();
    }
  if (strcmp (cmd, "list") && strcmp (cmd, "enroll") && strcmp (cmd, "verify") &&
      strcmp (cmd, "identify") && strcmp (cmd, "delete"))
    return usage ();
  if ((!strcmp (cmd, "enroll") && (!arg || finger_parse (arg) == FP_FINGER_UNKNOWN)) ||
      (!strcmp (cmd, "delete") && !arg))
    return usage ();
  if (!user) user = current_user ();
  if (debug) g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

  /* Note: like upstream libfprint, every open re-sends the fixed sensor config with the
   * "write to flash" flag. The non-persisting variant (XEFP_CFG_NO_FLASH=1) gets no reply
   * from firmware 01010278 (tested: 5 s data timeout), so it is not the default. */

  cancellable = g_cancellable_new ();
  g_unix_signal_add (SIGINT, on_sigint, NULL);
  g_unix_signal_add (SIGTERM, on_sigint, NULL);

  g_autoptr(FpContext) ctx = fp_context_new ();
  g_autoptr(FpDevice) dev = open_device (ctx, &error);
  if (!dev)
    {
      fprintf (stderr, "xe-fp: cannot open device: %s\n", error->message);
      return 2;
    }
  if (!quiet)
    printf ("device: %s  driver=%s  id=%s  enroll-stages=%d  storage=%s\n",
            fp_device_get_name (dev), fp_device_get_driver (dev), fp_device_get_device_id (dev),
            fp_device_get_nr_enroll_stages (dev),
            fp_device_has_feature (dev, FP_DEVICE_FEATURE_STORAGE) ? "on-chip" : "host");

  int rc = 2;
  g_autoptr(GPtrArray) prints = NULL;

  if (!strcmp (cmd, "list"))
    {
      prints = fp_device_list_prints_sync (dev, cancellable, &error);
      if (!prints) { fprintf (stderr, "xe-fp: list failed: %s\n", error->message); goto out; }
      printf ("%u print(s) stored on the sensor:\n", prints->len);
      for (guint i = 0; i < prints->len; i++) print_line (i, g_ptr_array_index (prints, i));
      rc = 0;
    }
  else if (!strcmp (cmd, "enroll"))
    {
      FpFinger f = arg ? finger_parse (arg) : FP_FINGER_UNKNOWN;
      if (f == FP_FINGER_UNKNOWN) { rc = usage (); goto out; }
      /* fp_print_new() returns a FLOATING ref and fp_device_enroll_sync() takes it
       * (transfer floating: it ref_sinks the template and may return the very same
       * object as the result). So we must NOT unref tmpl ourselves: doing so (the old
       * g_autoptr here) double-freed the print after a successful enroll -> segfault. */
      FpPrint *tmpl = fp_print_new (dev);
      g_autoptr(GDate) date = g_date_new ();
      g_date_set_time_t (date, time (NULL));
      fp_print_set_finger (tmpl, f);
      fp_print_set_username (tmpl, user);
      fp_print_set_enroll_date (tmpl, date);
      printf ("Enrolling %s for user '%s' — touch the sensor repeatedly (%d stages). Ctrl-C aborts.\n",
              finger_name (f), user, fp_device_get_nr_enroll_stages (dev));
      g_autoptr(FpPrint) res = fp_device_enroll_sync (dev, tmpl, cancellable, enroll_progress, NULL, &error);
      if (!res)
        {
          fprintf (stderr, "xe-fp: enroll failed: %s\n", error->message);
          if (g_error_matches (error, FP_DEVICE_ERROR, FP_DEVICE_ERROR_DATA_FULL))
            fprintf (stderr, "  sensor storage is full. xe-fp will NOT free space automatically; "
                             "use `xe-fp list` and `xe-fp delete` deliberately.\n");
          rc = g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ? 3 : 2;
          goto out;
        }
      printf ("enrolled: "); print_line (0, res);
      rc = 0;
    }
  else if (!strcmp (cmd, "verify") || !strcmp (cmd, "identify"))
    {
      gboolean all = !strcmp (cmd, "identify");
      g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (g_object_unref);
      prints = fp_device_list_prints_sync (dev, cancellable, &error);
      if (!prints) { fprintf (stderr, "xe-fp: list failed: %s\n", error->message); goto out; }
      for (guint i = 0; i < prints->len; i++)
        {
          FpPrint *p = g_ptr_array_index (prints, i);
          if (all || print_belongs_to (p, user)) g_ptr_array_add (gallery, g_object_ref (p));
        }
      if (gallery->len == 0)
        {
          fprintf (stderr, "xe-fp: no enrolled prints for user '%s' (run: xe-fp enroll right-index)\n", user);
          rc = 4; goto out;
        }
      if (!quiet) { printf ("Touch the sensor%s...\n", timeout ? "" : " (Ctrl-C aborts)"); fflush (stdout); }
      arm_timeout (timeout);
      g_autoptr(FpPrint) match = NULL;
      /* identify on the chip: the sensor matches against its own DB; libfprint then
       * checks the returned template id is in our gallery. Retry up to --tries times. */
      for (int attempt = 1; attempt <= tries; attempt++)
        {
          g_autoptr(FpPrint) scan = NULL;
          g_clear_object (&match);
          g_clear_error (&error);
          if (!fp_device_identify_sync (dev, gallery, cancellable, match_cb, NULL, &match, &scan, &error))
            {
              /* FP_DEVICE_RETRY = bad scan (too short, finger moved...): try again */
              if (error->domain == FP_DEVICE_RETRY && attempt < tries)
                {
                  if (!quiet) { printf ("  %s — try again (%d/%d)\n", error->message, attempt, tries); fflush (stdout); }
                  continue;
                }
              break;
            }
          if (match) break;
          if (attempt < tries && !quiet) { printf ("  no match, try again (%d/%d)\n", attempt, tries); fflush (stdout); }
        }
      if (error)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            { if (!quiet) fprintf (stderr, "xe-fp: %s\n", timed_out ? "timeout" : "cancelled"); rc = 3; }
          else
            { fprintf (stderr, "xe-fp: %s failed: %s\n", cmd, error->message); rc = 2; }
          goto out;
        }
      if (match)
        {
          if (!quiet) { printf ("MATCH: "); print_line (0, match); }
          rc = 0;
        }
      else
        {
          if (!quiet) printf ("NO MATCH\n");
          rc = 1;
        }
    }
  else if (!strcmp (cmd, "delete"))
    {
      if (!arg) { rc = usage (); goto out; }
      prints = fp_device_list_prints_sync (dev, cancellable, &error);
      if (!prints) { fprintf (stderr, "xe-fp: list failed: %s\n", error->message); goto out; }
      FpPrint *target = NULL;
      if (arg[0] == '#')
        {
          guint idx = atoi (arg + 1);
          if (idx < prints->len) target = g_ptr_array_index (prints, idx);
        }
      else
        {
          FpFinger f = finger_parse (arg);
          for (guint i = 0; i < prints->len && f != FP_FINGER_UNKNOWN; i++)
            {
              FpPrint *p = g_ptr_array_index (prints, i);
              if (print_belongs_to (p, user) && fp_print_get_finger (p) == f) { target = p; break; }
            }
        }
      if (!target) { fprintf (stderr, "xe-fp: no matching print\n"); rc = 1; goto out; }
      if (!print_is_ours (target) && !force_foreign)
        {
          fprintf (stderr, "xe-fp: refusing to delete a FOREIGN print (probably Windows Hello). "
                           "Use --force-foreign if you really mean it.\n");
          rc = 2; goto out;
        }
      if (print_is_ours (target) && !print_belongs_to (target, user) && !force_foreign)
        {
          fprintf (stderr, "xe-fp: print belongs to user '%s', not '%s'\n", fp_print_get_username (target), user);
          rc = 2; goto out;
        }
      printf ("deleting: "); print_line (0, target);
      if (!fp_device_delete_print_sync (dev, target, cancellable, &error))
        { fprintf (stderr, "xe-fp: delete failed: %s\n", error->message); goto out; }
      printf ("deleted.\n");
      rc = 0;
    }
  else
    rc = usage ();

out:
  {
    g_autoptr(GError) cerr = NULL;
    if (!fp_device_close_sync (dev, NULL, &cerr) && !quiet)
      fprintf (stderr, "xe-fp: close: %s\n", cerr->message);
  }
  return rc;
}
