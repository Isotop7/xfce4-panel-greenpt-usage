/*
 * GreenPT Credits — xfce4-panel plugin
 *
 * Polls the GreenPT API for the remaining API credit and shows it in the
 * panel. The balance is read from the `x-credits-remaining` response
 * header. The cheapest request that returns this header is a
 * chat-completions call with a bogus model name: it fails with 400
 * "Unsupported model" and is not billed.
 */

#include <math.h>
#include <string.h>

#include <curl/curl.h>
#include <gtk/gtk.h>

#include <libxfce4panel/libxfce4panel.h>
#include <libxfce4util/libxfce4util.h>

#define POLL_MODEL "__balance_poll__"
#define REQUEST_TIMEOUT_MS 15000
#define MIN_REFRESH_SECONDS 10
#define MAX_REFRESH_SECONDS 86400
#define DEFAULT_REFRESH_SECONDS 300

typedef enum
{
  REGION_EU = 0,
  REGION_US = 1,
} Region;

typedef struct
{
  XfcePanelPlugin *plugin;

  GtkWidget *box;
  GtkWidget *hbox;
  GtkWidget *image;
  GtkWidget *label;
  GtkCssProvider *css_provider;

  gchar *api_key;        /* key stored in the rc file */
  gboolean use_env_token;
  gint refresh_seconds;
  Region region;
  gdouble low_threshold;

  guint refresh_timeout_id;
  guint result_idle_id;  /* idle source id written by the worker before it
                            exits; read on the main thread only after the join */
  GThread *worker_thread;     /* non-NULL while a poll is in flight */
  gboolean worker_running;
  gint shutting_down;  /* atomic; set on teardown so the worker aborts fast */

  CURL *curl;  /* poll handle, created lazily on the worker thread and reused;
                  teardown cleans it up only after joining the worker */

  GtkWidget *configure_dialog;  /* non-NULL while the properties dialog runs */
  gboolean configure_destroyed;  /* set by the dialog destroy handler */

  gboolean have_balance;
  gdouble balance;
  gchar *last_update_time;  /* timestamp of the last successful update */
} GreenptPlugin;

/* raw result produced by the poll worker thread */
typedef struct
{
  GreenptPlugin *owner;     /* plugin instance that started the poll */
  Region region;            /* snapshot taken on the main thread before spawning */
  gchar *balance_header;    /* captured `x-credits-remaining` value or NULL */
  glong http_status;
  CURLcode curl_code;
} PollResult;

static void greenpt_poll_result_free (PollResult *poll_result);
static gboolean greenpt_poll_finished (gpointer user_data);

/* per-poll input snapshot created on the main thread; the worker reads only
   this, never the live plugin fields, because the configure dialog can mutate
   them from its nested main loop while a poll runs */
typedef struct
{
  GreenptPlugin *owner;
  gchar *api_key;   /* resolved key, owned by the request */
  Region region;
} PollRequest;

/* parsed result handed to the GUI callback */
typedef struct
{
  gchar *formatted;    /* label_text to show on success, NULL on failure */
  gdouble balance;
  gchar *error;
  Region region;       /* region the poll actually hit */
  gboolean invalid_key;  /* 401 */
  gboolean no_credits;   /* 402 */
  gboolean raw_balance;  /* header was not numeric; `formatted` is the raw string */
} UpdateResult;

static const gchar *REGION_BASE[] = {
  "https://api.greenpt.ai",
  "https://api.us.greenpt.ai",
};

static const gchar *REGION_NAME[] = {
  "EU",
  "US",
};

static const gchar *LABEL_MISSING_KEY = "Missing API Key";
static const gchar *LABEL_DASH = "\xE2\x80\x94";
static const gchar *LABEL_EURO = "\xE2\x82\xAC";
static const gchar *TOOLTIP_LAST_UPDATE = "\nLast update: ";
static const gchar *CSS_CLASS_LOW = "greenpt-low";
static const gchar *CSS_LOW_VALUE_RULE = ".greenpt-low { color: #ff4040; }";
static const gchar *DATA_KEY_EDITED = "greenpt-key-edited";
static const gchar *CONFIG_GROUP = "greenpt";
static const gchar *CONFIG_KEY_API = "api_key";
static const gchar *CONFIG_KEY_REGION = "region";
static const gchar *CONFIG_KEY_REFRESH_SECONDS = "refresh_seconds";
static const gchar *CONFIG_KEY_LOW_THRESHOLD = "low_threshold";
static const gchar *CONFIG_KEY_USE_ENV_TOKEN = "use_env_token";
static const gchar *ENV_TOKEN_VAR = "GREENPT_API_TOKEN";
static const gchar *TOOLTIP_ENV_TOKEN_UNSET =
  "GreenPT: $GREENPT_API_TOKEN is not set and no API key is "
  "stored — set the variable or enter a key in Properties";
static const gchar *TOOLTIP_MISSING_KEY =
  "GreenPT: no API key set — open Properties to configure";
static const gchar *TOOLTIP_INVALID_KEY = "GreenPT: invalid API key (401)";
static const gchar *TOOLTIP_NO_CREDITS = "GreenPT: no credits left (402)";
static const gchar *TOOLTIP_UNSAFE_KEY =
  "GreenPT: the API key contains control or non-ASCII characters and "
  "was not sent \xE2\x80\x94 fix $GREENPT_API_TOKEN or the key in Properties";
static const gchar *TOOLTIP_UPDATE_FAILED = "GreenPT: update failed (%s)%s%s";
static const gchar *ERROR_HTTP_NO_HEADER = "HTTP %ld without balance header";
static const gchar *ERROR_UNKNOWN = "unknown error";

/* number of plugin instances that did curl_global_init */
static gint greenpt_curl_ref_count = 0;

static void greenpt_schedule_refresh (GreenptPlugin *greenpt);
static void greenpt_config_save (GreenptPlugin *greenpt);
static gboolean greenpt_key_is_safe (const gchar *key);



/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

static void
greenpt_config_load (GreenptPlugin *greenpt)
{
  GKeyFile *config;
  gchar *file;

  greenpt->api_key = g_strdup ("");
  greenpt->use_env_token = FALSE;
  greenpt->refresh_seconds = DEFAULT_REFRESH_SECONDS;
  greenpt->region = REGION_EU;
  greenpt->low_threshold = 5.0;

  file = xfce_panel_plugin_save_location (greenpt->plugin, FALSE);
  if (file == NULL)
    return;

  config = g_key_file_new ();
  if (g_key_file_load_from_file (config, file, G_KEY_FILE_NONE, NULL))
    {
      gchar *stored_key = g_key_file_get_string (config, CONFIG_GROUP, CONFIG_KEY_API, NULL);
      if (stored_key != NULL)
        {
          g_free (greenpt->api_key);
          greenpt->api_key = stored_key;
        }

      {
        gchar *region = g_key_file_get_string (config, CONFIG_GROUP, CONFIG_KEY_REGION, NULL);
        if (region != NULL && g_ascii_strcasecmp (region, "us") == 0)
          greenpt->region = REGION_US;
        g_free (region);
      }

      greenpt->use_env_token = g_key_file_get_boolean (config, CONFIG_GROUP,
                                                   CONFIG_KEY_USE_ENV_TOKEN, NULL);

      {
        gint seconds = g_key_file_get_integer (config, CONFIG_GROUP,
                                               CONFIG_KEY_REFRESH_SECONDS, NULL);
        if (seconds >= MIN_REFRESH_SECONDS)
          greenpt->refresh_seconds = MIN (seconds, MAX_REFRESH_SECONDS);
      }

      {
        GError *error = NULL;
        gdouble threshold = g_key_file_get_double (config, CONFIG_GROUP, CONFIG_KEY_LOW_THRESHOLD, &error);
        if (error == NULL && threshold >= 0.0)
          greenpt->low_threshold = threshold;
        g_clear_error (&error);
      }
    }

  g_key_file_free (config);
  g_free (file);
}

static void
greenpt_config_save (GreenptPlugin *greenpt)
{
  GKeyFile *config;
  gchar *file;
  gchar *data;
  gsize length;
  GError *error = NULL;

  file = xfce_panel_plugin_save_location (greenpt->plugin, TRUE);
  if (file == NULL)
    return;

  config = g_key_file_new ();
  g_key_file_set_string (config, CONFIG_GROUP, CONFIG_KEY_API, greenpt->api_key);
  g_key_file_set_string (config, CONFIG_GROUP, CONFIG_KEY_REGION, REGION_NAME[greenpt->region]);
  g_key_file_set_integer (config, CONFIG_GROUP, CONFIG_KEY_REFRESH_SECONDS, greenpt->refresh_seconds);
  g_key_file_set_double (config, CONFIG_GROUP, CONFIG_KEY_LOW_THRESHOLD, greenpt->low_threshold);
  g_key_file_set_boolean (config, CONFIG_GROUP, CONFIG_KEY_USE_ENV_TOKEN, greenpt->use_env_token);

  data = g_key_file_to_data (config, &length, NULL);
  if (data != NULL)
    {
      /* 0600: the file holds the plaintext API key */
      if (!g_file_set_contents_full (file, data, length,
                                     G_FILE_SET_CONTENTS_DURABLE, 0600, &error))
        {
          g_warning ("greenpt: failed to save config: %s", error->message);
          g_error_free (error);
        }
      g_free (data);
    }

  g_key_file_free (config);
  g_free (file);
}



/* ------------------------------------------------------------------ */
/* Worker thread                                                       */
/* ------------------------------------------------------------------ */

/* effective key: $GREENPT_API_TOKEN when enabled and set, else the stored key */
static const gchar *
greenpt_resolve_key (GreenptPlugin *greenpt)
{
  if (greenpt->use_env_token)
    {
      const gchar *env_token = g_getenv (ENV_TOKEN_VAR);
      if (env_token != NULL && *env_token != '\0')
        return env_token;
    }
  return greenpt->api_key;
}

static void
greenpt_show_missing_key (GreenptPlugin *greenpt)
{
  if (greenpt->use_env_token)
    {
      const gchar *env_token = g_getenv (ENV_TOKEN_VAR);
      if (env_token == NULL || *env_token == '\0')
        {
          gtk_label_set_text (GTK_LABEL (greenpt->label), _(LABEL_MISSING_KEY));
          gtk_widget_set_tooltip_text (greenpt->box, TOOLTIP_ENV_TOKEN_UNSET);
          return;
        }
    }

  gtk_label_set_text (GTK_LABEL (greenpt->label), _(LABEL_MISSING_KEY));
  gtk_widget_set_tooltip_text (greenpt->box, TOOLTIP_MISSING_KEY);
}

static size_t
greenpt_balance_header_callback (gchar *buffer, size_t size, size_t nitems, gpointer user_data)
{
  PollResult *poll_result = user_data;
  size_t total = size * nitems;
  const gchar name[] = "x-credits-remaining:";
  const size_t name_len = sizeof (name) - 1;

  if (poll_result->balance_header == NULL && total > name_len &&
      g_ascii_strncasecmp (buffer, name, name_len) == 0)
    {
      gchar *balance = g_strndup (buffer + name_len, total - name_len);
      g_strstrip (balance);
      if (*balance != '\0')
        poll_result->balance_header = balance;
      else
        g_free (balance);
    }

  return total;
}

/* called by curl during the transfer; returning non-zero aborts it, which lets
   teardown interrupt a hung poll instead of blocking the panel for the full
   request timeout */
static int
greenpt_shutdown_check_callback (void *clientp, curl_off_t dltotal G_GNUC_UNUSED,
                 curl_off_t dlnow G_GNUC_UNUSED, curl_off_t ultotal G_GNUC_UNUSED,
                 curl_off_t ulnow G_GNUC_UNUSED)
{
  GreenptPlugin *greenpt = clientp;
  return g_atomic_int_get (&greenpt->shutting_down) ? 1 : 0;
}

static gpointer
greenpt_poll_thread (gpointer data)
{
  PollRequest *request = data;
  GreenptPlugin *greenpt = request->owner;
  PollResult poll_result;
  gchar *url;
  gchar *auth_header;
  gchar *request_body;
  struct curl_slist *headers = NULL;
  CURL *curl;
  CURLcode curl_code = CURLE_FAILED_INIT;

  memset (&poll_result, 0, sizeof (poll_result));
  poll_result.owner = greenpt;
  poll_result.region = request->region;

  url = g_strdup_printf ("%s/v1/chat/completions", REGION_BASE[poll_result.region]);
  auth_header = g_strdup_printf ("Authorization: Bearer %s", request->api_key);
  request_body = g_strdup_printf ("{\"model\":\"%s\",\"messages\":[]}", POLL_MODEL);

  /* the handle is created lazily on this thread and reused by later polls,
     so teardown must only clean it up after joining the worker */
  if (greenpt->curl == NULL)
    greenpt->curl = curl_easy_init ();
  curl = greenpt->curl;
  if (curl != NULL)
    {
      headers = curl_slist_append (NULL, auth_header);
      headers = curl_slist_append (headers, "Content-Type: application/json");

      curl_easy_setopt (curl, CURLOPT_URL, url);
      curl_easy_setopt (curl, CURLOPT_POSTFIELDS, request_body);
      curl_easy_setopt (curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt (curl, CURLOPT_HEADERFUNCTION, greenpt_balance_header_callback);
      curl_easy_setopt (curl, CURLOPT_HEADERDATA, &poll_result);
      curl_easy_setopt (curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt (curl, CURLOPT_TIMEOUT_MS, (long) REQUEST_TIMEOUT_MS);
      /* short connect timeout so a stalled connection cannot keep a teardown
         join (or the panel) blocked for the full request timeout */
      curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT_MS, 3000L);
      /* no FOLLOWLOCATION: custom headers (the Authorization bearer key) are
         re-sent to every host in a redirect chain, and the endpoint never
         redirects anyway */
      curl_easy_setopt (curl, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt (curl, CURLOPT_XFERINFOFUNCTION, greenpt_shutdown_check_callback);
      curl_easy_setopt (curl, CURLOPT_XFERINFODATA, greenpt);

      curl_code = curl_easy_perform (curl);
      if (curl_code == CURLE_OK)
        curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &poll_result.http_status);

      curl_easy_setopt (curl, CURLOPT_HTTPHEADER, NULL);
    }

  curl_slist_free_all (headers);
  g_free (url);
  g_free (auth_header);
  g_free (request_body);

  poll_result.curl_code = curl_code;

  g_free (request->api_key);
  g_free (request);

  /* hand the result to the main loop; the join in the callback returns
     immediately because this thread finishes right after */
  greenpt->result_idle_id = g_idle_add_full (G_PRIORITY_DEFAULT, greenpt_poll_finished,
                                 g_memdup2 (&poll_result, sizeof (poll_result)),
                                 (GDestroyNotify) greenpt_poll_result_free);
  return NULL;
}

static void
greenpt_poll_result_free (PollResult *poll_result)
{
  g_free (poll_result->balance_header);
  g_free (poll_result);
}

static gboolean
greenpt_poll_finished (gpointer user_data)
{
  PollResult *poll_result = user_data;
  GreenptPlugin *greenpt = poll_result->owner;
  UpdateResult *result;
  GThread *thread;
  GDateTime *now;
  gchar *label_text;
  gchar *tooltip;
  GtkStyleContext *style_ctx;

  /* the worker_thread scheduled this callback immediately before returning, so
     the join below completes without blocking */
  thread = greenpt->worker_thread;
  greenpt->worker_thread = NULL;
  greenpt->worker_running = FALSE;
  greenpt->result_idle_id = 0;

  if (thread != NULL)
    g_thread_join (thread);   /* joins the thread and drops our reference */

  result = g_new0 (UpdateResult, 1);
  result->region = poll_result->region;

  if (poll_result->curl_code != CURLE_OK)
    {
      result->error = g_strdup (curl_easy_strerror (poll_result->curl_code));
    }
  else if (poll_result->http_status == 401)
    {
      result->invalid_key = TRUE;
    }
  else if (poll_result->http_status == 402)
    {
      result->no_credits = TRUE;
    }
  else if (poll_result->balance_header != NULL)
    {
      gdouble balance;
      gchar *parse_end = NULL;
      balance = g_ascii_strtod (poll_result->balance_header, &parse_end);
      /* require a numeric start so "nan"/"inf"/hex never become the label;
         the digit check alone is not enough for hex, which strtod accepts */
      if (parse_end != poll_result->balance_header && *parse_end == '\0' &&
          isfinite (balance) &&
          strchr (poll_result->balance_header, 'x') == NULL &&
          strchr (poll_result->balance_header, 'X') == NULL &&
          (g_ascii_isdigit (*poll_result->balance_header) ||
           *poll_result->balance_header == '+' || *poll_result->balance_header == '-' ||
           *poll_result->balance_header == '.'))
        {
          result->balance = balance;
          result->formatted = g_strdup_printf ("%.2f %s", balance, LABEL_EURO);
        }
      else
        {
          /* non-numeric header: display the raw string, keep the last
             numeric balance for the low-balance logic */
          result->raw_balance = TRUE;
          result->formatted = g_strdup (poll_result->balance_header);
        }
    }
  else
    {
      result->error = g_strdup_printf (ERROR_HTTP_NO_HEADER, poll_result->http_status);
    }

  /* poll_result is owned by the idle's GDestroyNotify (the g_idle_add_full in
     greenpt_poll_thread), which runs now
     that we return G_SOURCE_REMOVE; do not free it here */

  if (result->formatted != NULL)
    {
      g_free (greenpt->last_update_time);
      now = g_date_time_new_now_local ();
      greenpt->last_update_time = g_date_time_format (now, "%H:%M:%S");
      g_date_time_unref (now);

      gtk_label_set_text (GTK_LABEL (greenpt->label), result->formatted);
      if (result->raw_balance)
        tooltip = g_strdup_printf ("GreenPT %s: %s remaining%s%s",
                               REGION_NAME[result->region], result->formatted,
                               TOOLTIP_LAST_UPDATE, greenpt->last_update_time);
      else
        {
          greenpt->have_balance = TRUE;
          greenpt->balance = result->balance;
          tooltip = g_strdup_printf ("GreenPT %s: %.2f %s remaining%s%s",
                                 REGION_NAME[result->region], greenpt->balance,
                                 LABEL_EURO, TOOLTIP_LAST_UPDATE,
                                 greenpt->last_update_time);
        }
      gtk_widget_set_tooltip_text (greenpt->box, tooltip);
      g_free (tooltip);
    }
  else if (result->invalid_key)
    {
      gtk_label_set_text (GTK_LABEL (greenpt->label), LABEL_DASH);
      gtk_widget_set_tooltip_text (greenpt->box, TOOLTIP_INVALID_KEY);
    }
  else if (result->no_credits)
    {
      gchar *zero_label;
      greenpt->have_balance = TRUE;
      greenpt->balance = 0.0;
      zero_label = g_strdup_printf ("0.00 %s", LABEL_EURO);
      gtk_label_set_text (GTK_LABEL (greenpt->label), zero_label);
      g_free (zero_label);
      gtk_widget_set_tooltip_text (greenpt->box, TOOLTIP_NO_CREDITS);
    }
  else
    {
      if (greenpt->have_balance)
        {
          label_text = g_strdup_printf ("%.2f %s", greenpt->balance, LABEL_EURO);
          gtk_label_set_text (GTK_LABEL (greenpt->label), label_text);
          g_free (label_text);
        }
      else
        gtk_label_set_text (GTK_LABEL (greenpt->label), LABEL_DASH);

      tooltip = g_strdup_printf (TOOLTIP_UPDATE_FAILED,
                             result->error != NULL ? result->error : ERROR_UNKNOWN,
                             greenpt->last_update_time != NULL ? TOOLTIP_LAST_UPDATE : "",
                             greenpt->last_update_time != NULL ? greenpt->last_update_time : "");
      gtk_widget_set_tooltip_text (greenpt->box, tooltip);
      g_free (tooltip);
    }

  /* low balance highlighting */
  style_ctx = gtk_widget_get_style_context (greenpt->label);
  if (greenpt->have_balance && greenpt->balance < greenpt->low_threshold)
    gtk_style_context_add_class (style_ctx, CSS_CLASS_LOW);
  else
    gtk_style_context_remove_class (style_ctx, CSS_CLASS_LOW);

  g_free (result->formatted);
  g_free (result->error);
  g_free (result);
  return G_SOURCE_REMOVE;
}

static gboolean
greenpt_refresh_callback (gpointer user_data)
{
  GreenptPlugin *greenpt = user_data;
  const gchar *resolved_key;
  PollRequest *request;

  if (greenpt->worker_running)
    return G_SOURCE_CONTINUE;

  resolved_key = greenpt_resolve_key (greenpt);
  if (resolved_key == NULL || *resolved_key == '\0')
    {
      greenpt_show_missing_key (greenpt);
      return G_SOURCE_CONTINUE;
    }

  /* same invariant as dialog-entered keys: the key goes verbatim into an
     HTTP request header, so never send one containing CRLF or non-ASCII */
  if (!greenpt_key_is_safe (resolved_key))
    {
      gtk_label_set_text (GTK_LABEL (greenpt->label), _("Invalid API Key"));
      gtk_widget_set_tooltip_text (greenpt->box, TOOLTIP_UNSAFE_KEY);
      g_warning ("greenpt: API key contains control or non-ASCII characters; not sending it");
      return G_SOURCE_CONTINUE;
    }

  /* the worker reads only this snapshot, never the live plugin fields */
  request = g_new (PollRequest, 1);
  request->owner = greenpt;
  request->region = greenpt->region;
  request->api_key = g_strdup (resolved_key);

  g_atomic_int_set (&greenpt->shutting_down, FALSE);
  greenpt->worker_running = TRUE;
  greenpt->worker_thread = g_thread_try_new ("greenpt-poll", greenpt_poll_thread, request, NULL);
  if (greenpt->worker_thread == NULL)
    {
      greenpt->worker_running = FALSE;
      g_free (request->api_key);
      g_free (request);
      g_warning ("greenpt: failed to create poll thread");
    }

  return G_SOURCE_CONTINUE;
}

static void
greenpt_schedule_refresh (GreenptPlugin *greenpt)
{
  if (greenpt->refresh_timeout_id != 0)
    g_source_remove (greenpt->refresh_timeout_id);
  greenpt->refresh_timeout_id = g_timeout_add_seconds (greenpt->refresh_seconds,
                                           greenpt_refresh_callback, greenpt);
  /* fetch right away as well */
  greenpt_refresh_callback (greenpt);
}



/* ------------------------------------------------------------------ */
/* Properties dialog                                                   */
/* ------------------------------------------------------------------ */

/* the key goes verbatim into an HTTP request header, so restrict it to
   printable ASCII: anything else could break the header line, and the
   masked display assumes single-byte characters */
static gboolean
greenpt_key_is_safe (const gchar *key)
{
  gsize i;

  for (i = 0; key[i] != '\0'; i++)
    {
      guchar c = (guchar) key[i];
      if (c < 0x21 || c > 0x7e)   /* controls, space, DEL, non-ASCII */
        return FALSE;
    }
  return TRUE;
}

static void
greenpt_entry_sensitive_toggled (GtkToggleButton *toggle, gpointer user_data)
{
  gtk_widget_set_sensitive (GTK_WIDGET (user_data),
                            !gtk_toggle_button_get_active (toggle));
}

static void
greenpt_key_entry_changed (GtkEditable *editable, gpointer user_data G_GNUC_UNUSED)
{
  g_object_set_data (G_OBJECT (editable), DATA_KEY_EDITED,
                     GINT_TO_POINTER (TRUE));
}

/* hide everything but the first and last 4 characters */
static gchar *
greenpt_mask_key (const gchar *key)
{
  const gchar *DOTS = "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2";
  gsize length;

  if (key == NULL || *key == '\0')
    return g_strdup ("");

  length = strlen (key);
  if (length <= 16)   /* too short to reveal 4+4 without leaking most of it */
    return g_strdup_printf ("%s%s", DOTS, DOTS);

  return g_strdup_printf ("%.4s%s%.4s", key, DOTS, key + length - 4);
}

/* free-data can fire inside the nested gtk_dialog_run loop (plugin removed
   or panel exits while the dialog is open); record it so the code after the
   run returns does not touch the freed plugin struct */
static void
greenpt_configure_dialog_destroyed (GtkWidget *widget G_GNUC_UNUSED,
                                    gpointer user_data)
{
  GreenptPlugin *greenpt = user_data;
  greenpt->configure_destroyed = TRUE;
  greenpt->configure_dialog = NULL;
}

static void
greenpt_configure (XfcePanelPlugin *plugin, GreenptPlugin *greenpt)
{
  GtkWidget *dialog;
  GtkWidget *vbox;
  GtkWidget *grid;
  GtkWidget *key_entry;
  GtkWidget *region_combo;
  GtkWidget *interval_spin;
  GtkWidget *threshold_spin;
  GtkWidget *env_check;
  GtkWidget *label;
  gint response;

  dialog = gtk_dialog_new_with_buttons (
    _("GreenPT Credits"),
    GTK_WINDOW (gtk_widget_get_toplevel (GTK_WIDGET (plugin))),
    GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL,
    _("_Close"), GTK_RESPONSE_OK,
    NULL);
  gtk_window_set_resizable (GTK_WINDOW (dialog), FALSE);

  vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  gtk_container_set_border_width (GTK_CONTAINER (vbox), 12);
  gtk_box_pack_start (GTK_BOX (gtk_dialog_get_content_area (GTK_DIALOG (dialog))),
                      vbox, TRUE, TRUE, 0);

  grid = gtk_grid_new ();
  gtk_grid_set_row_spacing (GTK_GRID (grid), 6);
  gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
  gtk_box_pack_start (GTK_BOX (vbox), grid, TRUE, TRUE, 0);

  label = gtk_label_new (_("API key:"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 0, 1, 1);

  key_entry = gtk_entry_new ();
  gtk_entry_set_width_chars (GTK_ENTRY (key_entry), 40);
  gtk_entry_set_text (GTK_ENTRY (key_entry), greenpt_mask_key (greenpt->api_key));
  gtk_widget_set_tooltip_text (key_entry,
    _("Stored in plaintext in the panel configuration file. "
      "Prefer $GREENPT_API_TOKEN if the key must not touch disk."));
  g_object_set_data (G_OBJECT (key_entry), DATA_KEY_EDITED,
                     GINT_TO_POINTER (FALSE));
  g_signal_connect (G_OBJECT (key_entry), "changed",
                    G_CALLBACK (greenpt_key_entry_changed), NULL);
  gtk_grid_attach (GTK_GRID (grid), key_entry, 1, 0, 1, 1);

  env_check = gtk_check_button_new_with_label (
    _("Use $GREENPT_API_TOKEN environment variable instead"));
  gtk_grid_attach (GTK_GRID (grid), env_check, 1, 1, 1, 1);
  gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (env_check), greenpt->use_env_token);
  gtk_widget_set_sensitive (key_entry, !greenpt->use_env_token);
  g_signal_connect (G_OBJECT (env_check), "toggled",
                    G_CALLBACK (greenpt_entry_sensitive_toggled), key_entry);

  label = gtk_label_new (_("Region:"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 2, 1, 1);

  region_combo = gtk_combo_box_text_new ();
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (region_combo), REGION_NAME[REGION_EU]);
  gtk_combo_box_text_append_text (GTK_COMBO_BOX_TEXT (region_combo), REGION_NAME[REGION_US]);
  gtk_combo_box_set_active (GTK_COMBO_BOX (region_combo), greenpt->region);
  gtk_grid_attach (GTK_GRID (grid), region_combo, 1, 2, 1, 1);

  label = gtk_label_new (_("Refresh interval (seconds):"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 3, 1, 1);

  interval_spin = gtk_spin_button_new_with_range (MIN_REFRESH_SECONDS, MAX_REFRESH_SECONDS, 10);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (interval_spin), greenpt->refresh_seconds);
  gtk_spin_button_set_digits (GTK_SPIN_BUTTON (interval_spin), 0);
  gtk_grid_attach (GTK_GRID (grid), interval_spin, 1, 3, 1, 1);

  label = gtk_label_new (_("Low balance threshold:"));
  gtk_widget_set_halign (label, GTK_ALIGN_START);
  gtk_grid_attach (GTK_GRID (grid), label, 0, 4, 1, 1);

  threshold_spin = gtk_spin_button_new_with_range (0, 100000, 0.01);
  gtk_spin_button_set_value (GTK_SPIN_BUTTON (threshold_spin), greenpt->low_threshold);
  gtk_grid_attach (GTK_GRID (grid), threshold_spin, 1, 4, 1, 1);

  gtk_widget_show_all (vbox);

  greenpt->configure_dialog = dialog;
  greenpt->configure_destroyed = FALSE;
  g_signal_connect (G_OBJECT (dialog), "destroy",
                    G_CALLBACK (greenpt_configure_dialog_destroyed), greenpt);

  response = greenpt->configure_destroyed ? GTK_RESPONSE_NONE
                                          : gtk_dialog_run (GTK_DIALOG (dialog));

  if (!greenpt->configure_destroyed && response == GTK_RESPONSE_OK)
    {
      /* only accept the entry contents when the user actually edited it;
         untouched means the masked placeholder is still showing */
      if (GPOINTER_TO_INT (g_object_get_data (G_OBJECT (key_entry),
                                              DATA_KEY_EDITED)))
        {
          const gchar *entered = gtk_entry_get_text (GTK_ENTRY (key_entry));
          if (greenpt_key_is_safe (entered))
            {
              g_free (greenpt->api_key);
              greenpt->api_key = g_strdup (entered);
            }
          else
            {
              g_warning ("greenpt: rejected API key containing control characters");
              gtk_widget_error_bell (key_entry);
            }
        }
      greenpt->use_env_token = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (env_check));
      greenpt->region = gtk_combo_box_get_active (GTK_COMBO_BOX (region_combo));
      greenpt->refresh_seconds = gtk_spin_button_get_value_as_int (GTK_SPIN_BUTTON (interval_spin));
      greenpt->low_threshold = gtk_spin_button_get_value (GTK_SPIN_BUTTON (threshold_spin));

      greenpt_config_save (greenpt);
      greenpt_schedule_refresh (greenpt);
    }

  /* skip if the dialog was destroyed elsewhere (free-data) */
  if (greenpt->configure_dialog == dialog)
    gtk_widget_destroy (dialog);
}



/* ------------------------------------------------------------------ */
/* Panel plugin glue                                                   */
/* ------------------------------------------------------------------ */

static void
greenpt_free (XfcePanelPlugin *plugin G_GNUC_UNUSED, GreenptPlugin *greenpt)
{
  /* if the properties dialog is open (nested main loop), close it first; its
     destroy handler clears the dialog fields on the still-valid struct */
  if (greenpt->configure_dialog != NULL)
    gtk_widget_destroy (greenpt->configure_dialog);

  if (greenpt->refresh_timeout_id != 0)
    {
      g_source_remove (greenpt->refresh_timeout_id);
      greenpt->refresh_timeout_id = 0;
    }

  /* wait for an in-flight poll; the worker_thread may already have scheduled the
     result callback, so reap the thread before touching greenpt->result_idle_id.
     shutting_down makes the transfer's progress callback abort it, so this join
     returns promptly instead of waiting out the request timeout. */
  g_atomic_int_set (&greenpt->shutting_down, TRUE);
  if (greenpt->worker_thread != NULL)
    g_thread_join (greenpt->worker_thread);
  greenpt->worker_thread = NULL;
  greenpt->worker_running = FALSE;

  /* the joined worker may have queued the result idle; destroy it if it has
      not run yet (the destroy notify frees the queued PollResult) */
  if (greenpt->result_idle_id != 0)
    {
      GSource *pending = g_main_context_find_source_by_id (NULL,
                                                           greenpt->result_idle_id);
      if (pending != NULL)
        g_source_destroy (pending);
      greenpt->result_idle_id = 0;
    }

  /* safe: no poll thread is running past the joins above */
  if (greenpt->curl != NULL)
    curl_easy_cleanup (greenpt->curl);

  gtk_style_context_remove_provider_for_screen (
    gdk_screen_get_default (), GTK_STYLE_PROVIDER (greenpt->css_provider));

  if (g_atomic_int_dec_and_test (&greenpt_curl_ref_count))
    curl_global_cleanup ();

  g_free (greenpt->api_key);
  g_free (greenpt->last_update_time);
  gtk_widget_destroy (greenpt->box);
  g_slice_free (GreenptPlugin, greenpt);
}

static void
greenpt_save (XfcePanelPlugin *plugin G_GNUC_UNUSED, GreenptPlugin *greenpt)
{
  greenpt_config_save (greenpt);
}

static void
greenpt_construct (XfcePanelPlugin *plugin)
{
  GreenptPlugin *greenpt;

  /* refcounted per plugin instance: several instances may live in the same
     panel process, so cleanup only happens for the last one */
  if (g_atomic_int_add (&greenpt_curl_ref_count, 1) == 0)
    curl_global_init (CURL_GLOBAL_DEFAULT);

  greenpt = g_slice_new0 (GreenptPlugin);
  greenpt->plugin = plugin;

  greenpt->css_provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_data (greenpt->css_provider,
                                   CSS_LOW_VALUE_RULE,
                                   -1, NULL);
  gtk_style_context_add_provider_for_screen (
    gdk_screen_get_default (), GTK_STYLE_PROVIDER (greenpt->css_provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (greenpt->css_provider);

  greenpt->box = gtk_event_box_new ();
  gtk_container_set_border_width (GTK_CONTAINER (greenpt->box), 0);

  greenpt->hbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_container_add (GTK_CONTAINER (greenpt->box), greenpt->hbox);

  greenpt->image = gtk_image_new_from_icon_name ("greenpt", GTK_ICON_SIZE_MENU);
  greenpt->label = gtk_label_new (LABEL_DASH);

  gtk_box_pack_start (GTK_BOX (greenpt->hbox), greenpt->image, FALSE, FALSE, 2);
  gtk_box_pack_start (GTK_BOX (greenpt->hbox), greenpt->label, FALSE, FALSE, 2);

  gtk_container_add (GTK_CONTAINER (plugin), greenpt->box);
  gtk_widget_show_all (greenpt->box);

  xfce_panel_plugin_set_small (plugin, TRUE);

  greenpt_config_load (greenpt);

  g_signal_connect (G_OBJECT (plugin), "save", G_CALLBACK (greenpt_save), greenpt);
  g_signal_connect (G_OBJECT (plugin), "free-data", G_CALLBACK (greenpt_free), greenpt);

  xfce_panel_plugin_menu_show_configure (plugin);
  g_signal_connect (G_OBJECT (plugin), "configure-plugin",
                    G_CALLBACK (greenpt_configure), greenpt);

  greenpt_schedule_refresh (greenpt);
}

XFCE_PANEL_PLUGIN_REGISTER (greenpt_construct)
