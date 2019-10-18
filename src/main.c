/* main.c - boot messages monitor
 *
 * Copyright (C) 2007 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sysexits.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <paths.h>
#include <assert.h>
#include <values.h>
#include <locale.h>

#include <linux/kd.h>
#include <linux/vt.h>

#include "ply-buffer.h"
#include "ply-command-parser.h"
#include "ply-boot-server.h"
#include "ply-boot-splash.h"
#include "ply-device-manager.h"
#include "ply-event-loop.h"
#include "ply-hashtable.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-renderer.h"
#include "ply-terminal-session.h"
#include "ply-trigger.h"
#include "ply-utils.h"
#include "ply-progress.h"
#include "ply-pixel-buffer.h"

#define BOOT_DURATION_FILE     PLYMOUTH_TIME_DIRECTORY "/boot-duration"
#define SHUTDOWN_DURATION_FILE PLYMOUTH_TIME_DIRECTORY "/shutdown-duration"

typedef struct
{
        const char    *keys;
        ply_trigger_t *trigger;
} ply_keystroke_watch_t;

typedef struct
{
        enum { PLY_ENTRY_TRIGGER_TYPE_PASSWORD,
               PLY_ENTRY_TRIGGER_TYPE_QUESTION }
        type;
        const char    *prompt;
        ply_trigger_t *trigger;
} ply_entry_trigger_t;

typedef struct
{
        ply_event_loop_t       *loop;
        ply_boot_server_t      *boot_server;
        ply_boot_splash_t      *boot_splash;
        ply_terminal_session_t *session;
        ply_buffer_t           *boot_buffer;
        ply_progress_t         *progress;
        ply_list_t             *keystroke_triggers;
        ply_list_t             *entry_triggers;
        ply_buffer_t           *entry_buffer;
        ply_list_t             *messages;
        ply_command_parser_t   *command_parser;
        ply_boot_splash_mode_t  mode;
        ply_terminal_t         *local_console_terminal;
        ply_device_manager_t   *device_manager;

        ply_trigger_t          *deactivate_trigger;
        ply_trigger_t          *quit_trigger;

        double                  start_time;
        double                  splash_delay;
        double                  device_timeout;

        uint32_t                no_boot_log : 1;
        uint32_t                showing_details : 1;
        uint32_t                system_initialized : 1;
        uint32_t                is_redirected : 1;
        uint32_t                is_attached : 1;
        uint32_t                should_be_attached : 1;
        uint32_t                should_retain_splash : 1;
        uint32_t                is_inactive : 1;
        uint32_t                is_shown : 1;
        uint32_t                should_force_details : 1;

        char                   *override_splash_path;
        char                   *system_default_splash_path;
        char                   *distribution_default_splash_path;
        const char             *default_tty;

        int                     number_of_errors;
} state_t;

static void show_splash (state_t *state);
static ply_boot_splash_t *load_built_in_theme (state_t *state);
static ply_boot_splash_t *load_theme (state_t    *state,
                                      const char *theme_path);
static ply_boot_splash_t *show_theme (state_t    *state,
                                      const char *theme_path);

static void attach_splash_to_devices (state_t           *state,
                                      ply_boot_splash_t *splash);
static bool attach_to_running_session (state_t *state);
static void detach_from_running_session (state_t *state);
static void on_escape_pressed (state_t *state);
static void dump_details_and_quit_splash (state_t *state);
static void update_display (state_t *state);

static void on_error_message (ply_buffer_t *debug_buffer,
                              const void   *bytes,
                              size_t        number_of_bytes);
static ply_buffer_t *debug_buffer;
static char *debug_buffer_path = NULL;
static char *pid_file = NULL;
static void toggle_between_splash_and_details (state_t *state);
#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
static void tell_systemd_to_print_details (state_t *state);
static void tell_systemd_to_stop_printing_details (state_t *state);
#endif
static const char *get_cache_file_for_mode (ply_boot_splash_mode_t mode);
static void on_escape_pressed (state_t *state);
static void on_enter (state_t    *state,
                      const char *line);
static void on_keyboard_input (state_t    *state,
                               const char *keyboard_input,
                               size_t      character_size);
static void on_backspace (state_t *state);
static void on_quit (state_t       *state,
                     bool           retain_splash,
                     ply_trigger_t *quit_trigger);
static bool sh_is_init (state_t *state);
static void cancel_pending_delayed_show (state_t *state);

static void
on_session_output (state_t    *state,
                   const char *output,
                   size_t      size)
{
        ply_buffer_append_bytes (state->boot_buffer, output, size);
        if (state->boot_splash != NULL)
                ply_boot_splash_update_output (state->boot_splash,
                                               output, size);
}

static void
on_session_hangup (state_t *state)
{
        ply_trace ("got hang up on terminal session fd");
}

static void
on_update (state_t    *state,
           const char *status)
{
        ply_trace ("updating status to '%s'", status);
        ply_progress_status_update (state->progress,
                                    status);
        if (state->boot_splash != NULL)
                ply_boot_splash_update_status (state->boot_splash,
                                               status);
}

static void
on_change_mode (state_t    *state,
                const char *mode)
{
        if (state->boot_splash == NULL) {
                ply_trace ("no splash set");
                return;
        }

        ply_trace ("updating mode to '%s'", mode);
        if (strcmp (mode, "boot-up") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_BOOT_UP;
        else if (strcmp (mode, "shutdown") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_SHUTDOWN;
        else if (strcmp (mode, "reboot") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_REBOOT;
        else if (strcmp (mode, "updates") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_UPDATES;
        else if (strcmp (mode, "system-upgrade") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE;
        else if (strcmp (mode, "firmware-upgrade") == 0)
                state->mode = PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE;
        else
                return;

        if (!ply_boot_splash_show (state->boot_splash, state->mode)) {
                ply_trace ("failed to update splash");
                return;
        }
}

static void
on_system_update (state_t *state,
                  int      progress)
{
        if (state->boot_splash == NULL) {
                ply_trace ("no splash set");
                return;
        }

        ply_trace ("setting system update to '%i'", progress);
        if (!ply_boot_splash_system_update (state->boot_splash, progress)) {
                ply_trace ("failed to update splash");
                return;
        }
}

static void
show_messages (state_t *state)
{
        if (state->boot_splash == NULL) {
                ply_trace ("not displaying messages, since no boot splash");
                return;
        }

        ply_list_node_t *node = ply_list_get_first_node (state->messages);
        while (node != NULL) {
                ply_list_node_t *next_node;
                char *message = ply_list_node_get_data (node);

                ply_trace ("displaying messages");

                ply_boot_splash_display_message (state->boot_splash, message);
                next_node = ply_list_get_next_node (state->messages, node);
                node = next_node;
        }
}

static bool
load_settings (state_t    *state,
               const char *path,
               char      **theme_path)
{
        ply_key_file_t *key_file = NULL;
        bool settings_loaded = false;
        char *scale_string = NULL;
        char *splash_string = NULL;
        char *orientation_string = NULL;

        ply_trace ("Trying to load %s", path);
        key_file = ply_key_file_new (path);

        if (!ply_key_file_load (key_file))
                goto out;

        splash_string = ply_key_file_get_value (key_file, "Daemon", "Theme");

        if (splash_string == NULL)
                goto out;

        asprintf (theme_path,
                  PLYMOUTH_RUNTIME_THEME_PATH "%s/%s.plymouth",
                  splash_string, splash_string);
        ply_trace ("Checking if %s exists", *theme_path);
        if (!ply_file_exists (*theme_path)) {
                ply_trace ("%s not found, fallbacking to " PLYMOUTH_THEME_PATH,
                           *theme_path);
                asprintf (theme_path,
                          PLYMOUTH_THEME_PATH "%s/%s.plymouth",
                          splash_string, splash_string);
        }

        if (isnan (state->splash_delay)) {
                char *delay_string;

                delay_string = ply_key_file_get_value (key_file, "Daemon", "ShowDelay");

                if (delay_string != NULL) {
                        state->splash_delay = atof (delay_string);
                        ply_trace ("Splash delay is set to %lf", state->splash_delay);
                        free (delay_string);
                }
        }

        if (isnan (state->device_timeout)) {
                char *timeout_string;

                timeout_string = ply_key_file_get_value (key_file, "Daemon", "DeviceTimeout");

                if (timeout_string != NULL) {
                        state->device_timeout = atof (timeout_string);
                        ply_trace ("Device timeout is set to %lf", state->device_timeout);

                        free (timeout_string);
                }
        }

        orientation_string = ply_key_file_get_value (key_file, "Daemon", "DeviceOrientation");

        if (orientation_string != NULL) {
                int orientation = PLY_PIXEL_BUFFER_ROTATE_UPRIGHT;

                if (strcmp (orientation_string, "UpsideDown") == 0)
                        orientation = PLY_PIXEL_BUFFER_ROTATE_UPSIDE_DOWN;

                if (strcmp (orientation_string, "LeftSideUp") == 0) {
                        /* Left side up, rotate counter clockwise to correct */
                        orientation = PLY_PIXEL_BUFFER_ROTATE_COUNTER_CLOCKWISE;
                }

                if (strcmp (orientation_string, "RightSideUp") == 0) {
                        /* Left side up, rotate clockwise to correct */
                        orientation = PLY_PIXEL_BUFFER_ROTATE_CLOCKWISE;
                }

                ply_set_device_orientation (orientation);
                free (orientation_string);
        }

        scale_string = ply_key_file_get_value (key_file, "Daemon", "DeviceScale");

        if (scale_string != NULL) {
                ply_set_device_scale (strtoul (scale_string, NULL, 0));
                free (scale_string);
        }

        settings_loaded = true;
out:
        free (splash_string);
        ply_key_file_free (key_file);

        return settings_loaded;
}

static void
show_detailed_splash (state_t *state)
{
        ply_boot_splash_t *splash;

        cancel_pending_delayed_show (state);

        if (state->boot_splash != NULL)
                return;

        ply_trace ("Showing detailed splash screen");
        splash = show_theme (state, NULL);

        if (splash == NULL) {
                ply_trace ("Could not start detailed splash screen, this could be a problem.");
                return;
        }

        state->boot_splash = splash;

        show_messages (state);
        update_display (state);
}

static void
find_override_splash (state_t *state)
{
        char *splash_string;

        if (state->override_splash_path != NULL)
                return;

        splash_string = ply_kernel_command_line_get_key_value ("plymouth.splash=");

        if (splash_string != NULL) {
                ply_trace ("Splash is configured to be '%s'", splash_string);
                asprintf (&state->override_splash_path,
                          PLYMOUTH_RUNTIME_THEME_PATH "%s/%s.plymouth",
                          splash_string, splash_string);

                ply_trace ("Checking if %s exists", state->override_splash_path);
                if (!ply_file_exists (state->override_splash_path)) {
                        ply_trace ("%s not found, fallbacking to " PLYMOUTH_THEME_PATH,
                                   state->override_splash_path);
                        free (state->override_splash_path);
                        asprintf (&state->override_splash_path,
                                  PLYMOUTH_THEME_PATH "%s/%s.plymouth",
                                  splash_string, splash_string);
                }
                free (splash_string);
        }

        if (isnan (state->splash_delay)) {
                const char *delay_string;

                delay_string = ply_kernel_command_line_get_string_after_prefix ("plymouth.splash-delay=");

                if (delay_string != NULL)
                        state->splash_delay = atof (delay_string);
        }
}

static void
find_force_scale (state_t *state)
{
        const char *scale_string;

        scale_string = ply_kernel_command_line_get_string_after_prefix ("plymouth.force-scale=");

        if (scale_string != NULL)
                ply_set_device_scale (strtoul (scale_string, NULL, 0));
}

static void
find_system_default_splash (state_t *state)
{
        if (state->system_default_splash_path != NULL)
                return;

        if (!load_settings (state, PLYMOUTH_CONF_DIR "plymouthd.conf", &state->system_default_splash_path)) {
                ply_trace ("failed to load " PLYMOUTH_CONF_DIR "plymouthd.conf");
                return;
        }

        ply_trace ("System configured theme file is '%s'", state->system_default_splash_path);
}

static void
find_distribution_default_splash (state_t *state)
{
        if (state->distribution_default_splash_path != NULL)
                return;

        if (!load_settings (state, PLYMOUTH_RUNTIME_DIR "/plymouthd.defaults", &state->distribution_default_splash_path)) {
                ply_trace ("failed to load " PLYMOUTH_RUNTIME_DIR "/plymouthd.defaults, trying " PLYMOUTH_POLICY_DIR);
                if (!load_settings (state, PLYMOUTH_POLICY_DIR "plymouthd.defaults", &state->distribution_default_splash_path)) {
                        ply_trace ("failed to load " PLYMOUTH_POLICY_DIR "plymouthd.defaults");
                        return;
                }
        }

        ply_trace ("Distribution default theme file is '%s'", state->distribution_default_splash_path);
}

static void
show_default_splash (state_t *state)
{
        if (state->boot_splash != NULL)
                return;

        ply_trace ("Showing splash screen");
        if (state->override_splash_path != NULL) {
                ply_trace ("Trying override splash at '%s'", state->override_splash_path);
                state->boot_splash = show_theme (state, state->override_splash_path);
        }

        if (state->boot_splash == NULL &&
            state->system_default_splash_path != NULL) {
                ply_trace ("Trying system default splash");
                state->boot_splash = show_theme (state, state->system_default_splash_path);
        }

        if (state->boot_splash == NULL &&
            state->distribution_default_splash_path != NULL) {
                ply_trace ("Trying distribution default splash");
                state->boot_splash = show_theme (state, state->distribution_default_splash_path);
        }

        if (state->boot_splash == NULL) {
                ply_trace ("Trying old scheme for default splash");
                state->boot_splash = show_theme (state, PLYMOUTH_THEME_PATH "default.plymouth");
        }

        if (state->boot_splash == NULL) {
                ply_trace ("Could not start default splash screen,"
                           "showing text splash screen");
                state->boot_splash = show_theme (state, PLYMOUTH_THEME_PATH "text/text.plymouth");
        }

        if (state->boot_splash == NULL) {
                ply_trace ("Could not start text splash screen,"
                           "showing built-in splash screen");
                state->boot_splash = show_theme (state, NULL);
        }

        if (state->boot_splash == NULL) {
                ply_error ("plymouthd: could not start boot splash: %m");
                return;
        }

        show_messages (state);
        update_display (state);
}

static void
cancel_pending_delayed_show (state_t *state)
{
        if (isnan (state->splash_delay))
                return;

        ply_event_loop_stop_watching_for_timeout (state->loop,
                                                  (ply_event_loop_timeout_handler_t)
                                                  show_splash,
                                                  state);
        state->splash_delay = NAN;
}

static void
on_ask_for_password (state_t       *state,
                     const char    *prompt,
                     ply_trigger_t *answer)
{
        ply_entry_trigger_t *entry_trigger;

        if (state->boot_splash == NULL) {
                /* Waiting to be shown, boot splash will
                 * arrive shortly so just sit tight
                 */
                if (state->is_shown) {
                        bool has_displays;

                        cancel_pending_delayed_show (state);

                        has_displays = ply_device_manager_has_displays (state->device_manager);

                        if (has_displays) {
                                ply_trace ("displays available now, showing splash immediately");
                                show_splash (state);
                        } else {
                                ply_trace ("splash still coming up, waiting a bit");
                        }
                } else {
                        /* No splash, client will have to get password */
                        ply_trace ("no splash loaded, replying immediately with no password");
                        ply_trigger_pull (answer, NULL);
                        return;
                }
        }

        entry_trigger = calloc (1, sizeof(ply_entry_trigger_t));
        entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_PASSWORD;
        entry_trigger->prompt = prompt;
        entry_trigger->trigger = answer;
        ply_trace ("queuing password request with boot splash");
        ply_list_append_data (state->entry_triggers, entry_trigger);
        update_display (state);
}

static void
on_ask_question (state_t       *state,
                 const char    *prompt,
                 ply_trigger_t *answer)
{
        ply_entry_trigger_t *entry_trigger;

        entry_trigger = calloc (1, sizeof(ply_entry_trigger_t));
        entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_QUESTION;
        entry_trigger->prompt = prompt;
        entry_trigger->trigger = answer;
        ply_trace ("queuing question with boot splash");
        ply_list_append_data (state->entry_triggers, entry_trigger);
        update_display (state);
}

static void
on_display_message (state_t    *state,
                    const char *message)
{
        if (state->boot_splash != NULL) {
                ply_trace ("displaying message %s", message);
                ply_boot_splash_display_message (state->boot_splash, message);
        } else {
                ply_trace ("not displaying message %s as no splash", message);
        }
        ply_list_append_data (state->messages, strdup (message));
}

static void
on_hide_message (state_t    *state,
                 const char *message)
{
        ply_list_node_t *node;

        ply_trace ("hiding message %s", message);

        node = ply_list_get_first_node (state->messages);
        while (node != NULL) {
                ply_list_node_t *next_node;
                char *list_message;

                list_message = ply_list_node_get_data (node);
                next_node = ply_list_get_next_node (state->messages, node);

                if (strcmp (list_message, message) == 0) {
                        free (list_message);
                        ply_list_remove_node (state->messages, node);
                        ply_boot_splash_hide_message (state->boot_splash, message);
                }
                node = next_node;
        }
}

static void
on_watch_for_keystroke (state_t       *state,
                        const char    *keys,
                        ply_trigger_t *trigger)
{
        ply_keystroke_watch_t *keystroke_trigger =
                calloc (1, sizeof(ply_keystroke_watch_t));

        ply_trace ("watching for keystroke");
        keystroke_trigger->keys = keys;
        keystroke_trigger->trigger = trigger;
        ply_list_append_data (state->keystroke_triggers, keystroke_trigger);
}

static void
on_ignore_keystroke (state_t    *state,
                     const char *keys)
{
        ply_list_node_t *node;

        ply_trace ("ignoring for keystroke");

        for (node = ply_list_get_first_node (state->keystroke_triggers); node;
             node = ply_list_get_next_node (state->keystroke_triggers, node)) {
                ply_keystroke_watch_t *keystroke_trigger = ply_list_node_get_data (node);
                if ((!keystroke_trigger->keys && !keys) ||
                    (keystroke_trigger->keys && keys && strcmp (keystroke_trigger->keys, keys) == 0)) {
                        ply_trigger_pull (keystroke_trigger->trigger, NULL);
                        ply_list_remove_node (state->keystroke_triggers, node);
                        return;
                }
        }
}

static void
on_progress_pause (state_t *state)
{
        ply_trace ("pausing progress");
        ply_progress_pause (state->progress);
}

static void
on_progress_unpause (state_t *state)
{
        ply_trace ("unpausing progress");
        ply_progress_unpause (state->progress);
}

static void
on_newroot (state_t    *state,
            const char *root_dir)
{
        if (sh_is_init (state)) {
                ply_trace ("new root mounted at \"%s\", exiting since init= a shell", root_dir);
                on_quit (state, false, ply_trigger_new (NULL));
                return;
        }

        ply_trace ("new root mounted at \"%s\", switching to it", root_dir);
        chdir (root_dir);
        chroot (".");
        chdir ("/");
        /* Update local now that we have /usr/share/locale available */
        setlocale(LC_ALL, "");
        ply_progress_load_cache (state->progress, get_cache_file_for_mode (state->mode));
        if (state->boot_splash != NULL)
                ply_boot_splash_root_mounted (state->boot_splash);
}

static const char *
get_cache_file_for_mode (ply_boot_splash_mode_t mode)
{
        const char *filename;

        switch (mode) {
        case PLY_BOOT_SPLASH_MODE_BOOT_UP:
                filename = BOOT_DURATION_FILE;
                break;
        case PLY_BOOT_SPLASH_MODE_SHUTDOWN:
        case PLY_BOOT_SPLASH_MODE_REBOOT:
                filename = SHUTDOWN_DURATION_FILE;
                break;
        case PLY_BOOT_SPLASH_MODE_UPDATES:
        case PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE:
        case PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE:
                filename = NULL;
                break;
        case PLY_BOOT_SPLASH_MODE_INVALID:
        default:
                ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
                abort ();
                break;
        }

        ply_trace ("returning cache file '%s'", filename);
        return filename;
}

static const char *
get_log_file_for_state (state_t *state)
{
        const char *filename;

        switch (state->mode) {
        case PLY_BOOT_SPLASH_MODE_BOOT_UP:
                if (state->no_boot_log)
                        filename = NULL;
                else
                        filename = PLYMOUTH_LOG_DIRECTORY "/boot.log";
                break;
        case PLY_BOOT_SPLASH_MODE_SHUTDOWN:
        case PLY_BOOT_SPLASH_MODE_REBOOT:
        case PLY_BOOT_SPLASH_MODE_UPDATES:
        case PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE:
        case PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE:
                filename = _PATH_DEVNULL;
                break;
        case PLY_BOOT_SPLASH_MODE_INVALID:
        default:
                ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
                abort ();
                break;
        }

        ply_trace ("returning log file '%s'", filename);
        return filename;
}

static const char *
get_log_spool_file_for_mode (ply_boot_splash_mode_t mode)
{
        const char *filename;

        switch (mode) {
        case PLY_BOOT_SPLASH_MODE_BOOT_UP:
                filename = PLYMOUTH_SPOOL_DIRECTORY "/boot.log";
                break;
        case PLY_BOOT_SPLASH_MODE_SHUTDOWN:
        case PLY_BOOT_SPLASH_MODE_REBOOT:
        case PLY_BOOT_SPLASH_MODE_UPDATES:
        case PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE:
        case PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE:
                filename = NULL;
                break;
        case PLY_BOOT_SPLASH_MODE_INVALID:
        default:
                ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
                abort ();
                break;
        }

        ply_trace ("returning spool file '%s'", filename);
        return filename;
}

static void
spool_error (state_t *state)
{
        const char *logfile;
        const char *logspool;

        ply_trace ("spooling error for viewer");

        logfile = get_log_file_for_state (state);
        logspool = get_log_spool_file_for_mode (state->mode);

        if (logfile != NULL && logspool != NULL) {
                unlink (logspool);

                ply_create_file_link (logfile, logspool);
        }
}

static void
prepare_logging (state_t *state)
{
        const char *logfile;

        if (!state->system_initialized) {
                ply_trace ("not preparing logging yet, system not initialized");
                return;
        }

        if (state->session == NULL) {
                ply_trace ("not preparing logging, no session");
                return;
        }

        logfile = get_log_file_for_state (state);
        if (logfile != NULL) {
                bool log_opened;
                ply_trace ("opening log '%s'", logfile);

                log_opened = ply_terminal_session_open_log (state->session, logfile);

                if (!log_opened)
                        ply_trace ("failed to open log: %m");

                if (state->number_of_errors > 0)
                        spool_error (state);
        }
}

static void
on_system_initialized (state_t *state)
{
        ply_trace ("system now initialized, opening log");
        state->system_initialized = true;

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
        if (state->is_attached)
                tell_systemd_to_print_details (state);
#endif

        prepare_logging (state);
}

static void
on_error (state_t *state)
{
        ply_trace ("encountered error during boot up");

        if (state->system_initialized && state->number_of_errors == 0)
                spool_error (state);
        else
                ply_trace ("not spooling because number of errors %d", state->number_of_errors);

        state->number_of_errors++;
}

static bool
plymouth_should_ignore_show_splash_calls (state_t *state)
{
        ply_trace ("checking if plymouth should be running");
        if (state->mode != PLY_BOOT_SPLASH_MODE_BOOT_UP || ply_kernel_command_line_has_argument ("plymouth.force-splash"))
                return false;

        if (ply_kernel_command_line_has_argument ("plymouth.ignore-show-splash"))
                return true;

        return false;
}

static bool
sh_is_init (state_t *state)
{
        char *init_string = ply_kernel_command_line_get_key_value ("init=");
        bool result = false;
        size_t length;

        if (init_string) {
                length = strlen (init_string);
                if (length > 2 && init_string[length - 2] == 's' &&
                                  init_string[length - 1] == 'h')
                        result = true;

                free (init_string);
        }

        return result;
}

static bool
plymouth_should_show_default_splash (state_t *state)
{
        ply_trace ("checking if plymouth should show default splash");

        const char * const strings[] = {
                "single", "1", "s", "S", "-S", NULL
        };
        int i;

        if (state->should_force_details)
                return false;

        for (i = 0; strings[i] != NULL; i++) {
                if (ply_kernel_command_line_has_argument (strings[i])) {
                        ply_trace ("no default splash because kernel command line has option \"%s\"", strings[i]);
                        return false;
                }
        }

        if (ply_kernel_command_line_has_argument ("splash=verbose")) {
                ply_trace ("no default splash because kernel command line has option \"splash=verbose\"");
                return false;
        }

        if (ply_kernel_command_line_has_argument ("rhgb")) {
                ply_trace ("using default splash because kernel command line has option \"rhgb\"");
                return true;
        }

        if (ply_kernel_command_line_has_argument ("splash")) {
                ply_trace ("using default splash because kernel command line has option \"splash\"");
                return true;
        }

        if (ply_kernel_command_line_has_argument ("splash=silent")) {
                ply_trace ("using default splash because kernel command line has option \"splash=silent\"");
                return true;
        }

        ply_trace ("no default splash because kernel command line lacks \"splash\" or \"rhgb\"");
        return false;
}

static void
on_show_splash (state_t *state)
{
        bool has_displays;

        if (state->is_shown) {
                ply_trace ("show splash called while already shown");
                return;
        }

        if (state->is_inactive) {
                ply_trace ("show splash called while inactive");
                return;
        }

        if (plymouth_should_ignore_show_splash_calls (state)) {
                ply_trace ("show splash called while ignoring show splash calls");
                dump_details_and_quit_splash (state);
                return;
        }

        state->is_shown = true;
        has_displays = ply_device_manager_has_displays (state->device_manager);

        if (!state->is_attached && state->should_be_attached && has_displays)
                attach_to_running_session (state);

        if (has_displays) {
                ply_trace ("at least one display already available, so loading splash");
                show_splash (state);
        } else {
                ply_trace ("no displays available to show splash on, waiting...");
        }
}

static void
show_splash (state_t *state)
{
        if (state->boot_splash != NULL)
                return;

        if (!isnan (state->splash_delay)) {
                double now, running_time;

                now = ply_get_timestamp ();
                running_time = now - state->start_time;
                if (state->splash_delay > running_time) {
                        double time_left = state->splash_delay - running_time;

                        ply_trace ("delaying show splash for %lf seconds",
                                   time_left);
                        ply_event_loop_stop_watching_for_timeout (state->loop,
                                                                  (ply_event_loop_timeout_handler_t)
                                                                  show_splash,
                                                                  state);
                        ply_event_loop_watch_for_timeout (state->loop,
                                                          time_left,
                                                          (ply_event_loop_timeout_handler_t)
                                                          show_splash,
                                                          state);
                        /* Listen for ESC to show details */
                        ply_device_manager_activate_keyboards (state->device_manager);
                        return;
                }
        }

        if (plymouth_should_show_default_splash (state)) {
                show_default_splash (state);
                state->showing_details = false;
        } else {
                show_detailed_splash (state);
                state->showing_details = true;
        }
}

static void
on_keyboard_added (state_t        *state,
                   ply_keyboard_t *keyboard)
{
        ply_trace ("listening for keystrokes");
        ply_keyboard_add_input_handler (keyboard,
                                        (ply_keyboard_input_handler_t)
                                        on_keyboard_input, state);
        ply_trace ("listening for escape");
        ply_keyboard_add_escape_handler (keyboard,
                                         (ply_keyboard_escape_handler_t)
                                         on_escape_pressed, state);
        ply_trace ("listening for backspace");
        ply_keyboard_add_backspace_handler (keyboard,
                                            (ply_keyboard_backspace_handler_t)
                                            on_backspace, state);
        ply_trace ("listening for enter");
        ply_keyboard_add_enter_handler (keyboard,
                                        (ply_keyboard_enter_handler_t)
                                        on_enter, state);

        if (state->boot_splash != NULL) {
                ply_trace ("keyboard set after splash loaded, so attaching to splash");
                ply_boot_splash_set_keyboard (state->boot_splash, keyboard);
        }
}

static void
on_keyboard_removed (state_t        *state,
                     ply_keyboard_t *keyboard)
{
    ply_trace ("no longer listening for keystrokes");
    ply_keyboard_remove_input_handler (keyboard,
                                       (ply_keyboard_input_handler_t)
                                       on_keyboard_input);
    ply_trace ("no longer listening for escape");
    ply_keyboard_remove_escape_handler (keyboard,
                                        (ply_keyboard_escape_handler_t)
                                        on_escape_pressed);
    ply_trace ("no longer listening for backspace");
    ply_keyboard_remove_backspace_handler (keyboard,
                                           (ply_keyboard_backspace_handler_t)
                                           on_backspace);
    ply_trace ("no longer listening for enter");
    ply_keyboard_remove_enter_handler (keyboard,
                                       (ply_keyboard_enter_handler_t)
                                       on_enter);

    if (state->boot_splash != NULL)
            ply_boot_splash_unset_keyboard (state->boot_splash);
}

static void
on_pixel_display_added (state_t             *state,
                        ply_pixel_display_t *display)
{
        if (state->is_shown) {
                if (state->boot_splash == NULL) {
                        ply_trace ("pixel display added before splash loaded, so loading splash now");
                        show_splash (state);
                } else {
                        ply_trace ("pixel display added after splash loaded, so attaching to splash");
                        ply_boot_splash_add_pixel_display (state->boot_splash, display);

                        update_display (state);
                }
        }
}

static void
on_pixel_display_removed (state_t             *state,
                          ply_pixel_display_t *display)
{
        if (state->boot_splash == NULL)
                return;

        ply_boot_splash_remove_pixel_display (state->boot_splash, display);
}

static void
on_text_display_added (state_t            *state,
                       ply_text_display_t *display)
{
        if (state->is_shown) {
                if (state->boot_splash == NULL) {
                        ply_trace ("text display added before splash loaded, so loading splash now");
                        show_splash (state);
                } else {
                        ply_trace ("text display added after splash loaded, so attaching to splash");
                        ply_boot_splash_add_text_display (state->boot_splash, display);

                        update_display (state);
                }
        }
}

static void
on_text_display_removed (state_t            *state,
                         ply_text_display_t *display)
{
        if (state->boot_splash == NULL)
                return;

        ply_boot_splash_remove_text_display (state->boot_splash, display);
}

static void
load_devices (state_t                   *state,
              ply_device_manager_flags_t flags)
{
        state->device_manager = ply_device_manager_new (state->default_tty, flags);
        state->local_console_terminal = ply_device_manager_get_default_terminal (state->device_manager);

        ply_device_manager_watch_devices (state->device_manager,
                                          state->device_timeout,
                                          (ply_keyboard_added_handler_t)
                                          on_keyboard_added,
                                          (ply_keyboard_removed_handler_t)
                                          on_keyboard_removed,
                                          (ply_pixel_display_added_handler_t)
                                          on_pixel_display_added,
                                          (ply_pixel_display_removed_handler_t)
                                          on_pixel_display_removed,
                                          (ply_text_display_added_handler_t)
                                          on_text_display_added,
                                          (ply_text_display_removed_handler_t)
                                          on_text_display_removed,
                                          state);

        if (ply_device_manager_has_serial_consoles (state->device_manager)) {
                state->should_force_details = true;
        }
}

static void
quit_splash (state_t *state)
{
        ply_trace ("quitting splash");
        if (state->boot_splash != NULL) {
                ply_trace ("freeing splash");
                ply_boot_splash_free (state->boot_splash);
                state->boot_splash = NULL;
        }

        ply_device_manager_deactivate_keyboards (state->device_manager);

        if (state->local_console_terminal != NULL) {
                if (!state->should_retain_splash) {
                        ply_trace ("Not retaining splash, so deallocating VT");
                        ply_terminal_deactivate_vt (state->local_console_terminal);
                        ply_terminal_close (state->local_console_terminal);
                }
        }

        detach_from_running_session (state);
}

static void
hide_splash (state_t *state)
{
        if (state->boot_splash && ply_boot_splash_uses_pixel_displays (state->boot_splash))
                ply_device_manager_deactivate_renderers (state->device_manager);

        state->is_shown = false;

        cancel_pending_delayed_show (state);

        if (state->boot_splash == NULL)
                return;

        ply_boot_splash_hide (state->boot_splash);

        if (state->local_console_terminal != NULL)
                ply_terminal_set_mode (state->local_console_terminal, PLY_TERMINAL_MODE_TEXT);
}

static void
dump_details_and_quit_splash (state_t *state)
{
        state->showing_details = false;
        toggle_between_splash_and_details (state);

        hide_splash (state);
        quit_splash (state);
}

static void
on_hide_splash (state_t *state)
{
        if (state->is_inactive)
                return;

        if (state->boot_splash == NULL)
                return;

        ply_trace ("hiding boot splash");
        dump_details_and_quit_splash (state);
}

static void
quit_program (state_t *state)
{
        ply_trace ("cleaning up devices");
        ply_device_manager_free (state->device_manager);

        ply_trace ("exiting event loop");
        ply_event_loop_exit (state->loop, 0);

        if (pid_file != NULL) {
                unlink (pid_file);
                free (pid_file);
                pid_file = NULL;
        }

        if (state->deactivate_trigger != NULL) {
                ply_trigger_pull (state->deactivate_trigger, NULL);
                state->deactivate_trigger = NULL;
        }
        if (state->quit_trigger != NULL) {
                ply_trigger_pull (state->quit_trigger, NULL);
                state->quit_trigger = NULL;
        }
}

static void
deactivate_console (state_t *state)
{
        detach_from_running_session (state);

        if (state->local_console_terminal != NULL) {
                ply_trace ("deactivating terminal");
                ply_terminal_stop_watching_for_vt_changes (state->local_console_terminal);
                ply_terminal_set_buffered_input (state->local_console_terminal);
                ply_terminal_close (state->local_console_terminal);
        }

        /* do not let any tty opened where we could write after deactivate */
        if (ply_kernel_command_line_has_argument ("plymouth.debug"))
                ply_logger_close_file (ply_logger_get_error_default ());

}

static void
deactivate_splash (state_t *state)
{
        assert (!state->is_inactive);

        if (state->boot_splash && ply_boot_splash_uses_pixel_displays (state->boot_splash))
                ply_device_manager_deactivate_renderers (state->device_manager);

        deactivate_console (state);

        state->is_inactive = true;

        ply_trigger_pull (state->deactivate_trigger, NULL);
        state->deactivate_trigger = NULL;
}

static void
on_boot_splash_idle (state_t *state)
{
        ply_trace ("boot splash idle");

        /* In the case where we've received both a deactivate command and a
         * quit command, the quit command takes precedence.
         */
        if (state->quit_trigger != NULL) {
                if (!state->should_retain_splash) {
                        ply_trace ("hiding splash");
                        hide_splash (state);
                }

                ply_trace ("quitting splash");
                quit_splash (state);
                ply_trace ("quitting program");
                quit_program (state);
        } else if (state->deactivate_trigger != NULL) {
                ply_trace ("deactivating splash");
                deactivate_splash (state);
        }
}

static void
on_deactivate (state_t       *state,
               ply_trigger_t *deactivate_trigger)
{
        if (state->is_inactive) {
                deactivate_console (state);
                ply_trigger_pull (deactivate_trigger, NULL);
                return;
        }

        if (state->deactivate_trigger != NULL) {
                ply_trigger_add_handler (state->deactivate_trigger,
                                         (ply_trigger_handler_t)
                                         ply_trigger_pull,
                                         deactivate_trigger);
                return;
        }

        state->deactivate_trigger = deactivate_trigger;

        ply_trace ("deactivating");
        cancel_pending_delayed_show (state);

        ply_device_manager_pause (state->device_manager);
        ply_device_manager_deactivate_keyboards (state->device_manager);

        if (state->boot_splash != NULL) {
                ply_boot_splash_become_idle (state->boot_splash,
                                             (ply_boot_splash_on_idle_handler_t)
                                             on_boot_splash_idle,
                                             state);
        } else {
                ply_trace ("deactivating splash");
                deactivate_splash (state);
        }
}

static void
on_reactivate (state_t *state)
{
        if (!state->is_inactive)
                return;

        if (state->local_console_terminal != NULL) {
                ply_terminal_open (state->local_console_terminal);
                ply_terminal_watch_for_vt_changes (state->local_console_terminal);
                ply_terminal_set_unbuffered_input (state->local_console_terminal);
                ply_terminal_ignore_mode_changes (state->local_console_terminal, false);
        }

        if ((state->session != NULL) && state->should_be_attached) {
                ply_trace ("reactivating terminal session");
                attach_to_running_session (state);
        }

        ply_device_manager_activate_keyboards (state->device_manager);
        if (state->boot_splash && ply_boot_splash_uses_pixel_displays (state->boot_splash))
                ply_device_manager_activate_renderers (state->device_manager);

        ply_device_manager_unpause (state->device_manager);

        state->is_inactive = false;

        update_display (state);
}

static void
on_quit (state_t       *state,
         bool           retain_splash,
         ply_trigger_t *quit_trigger)
{
        ply_trace ("quitting (retain splash: %s)", retain_splash ? "true" : "false");

        if (state->quit_trigger != NULL) {
                ply_trace ("quit trigger already pending, so chaining to it");
                ply_trigger_add_handler (state->quit_trigger,
                                         (ply_trigger_handler_t)
                                         ply_trigger_pull,
                                         quit_trigger);
                return;
        }

        if (state->system_initialized) {
                ply_trace ("system initialized so saving boot-duration file");
                ply_create_directory (PLYMOUTH_TIME_DIRECTORY);
                ply_progress_save_cache (state->progress,
                                         get_cache_file_for_mode (state->mode));
        } else {
                ply_trace ("system not initialized so skipping saving boot-duration file");
        }
        state->quit_trigger = quit_trigger;
        state->should_retain_splash = retain_splash;

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
        tell_systemd_to_stop_printing_details (state);
#endif

        ply_trace ("closing log");
        if (state->session != NULL)
                ply_terminal_session_close_log (state->session);

        ply_device_manager_deactivate_keyboards (state->device_manager);

        ply_trace ("unloading splash");
        if (state->is_inactive && !retain_splash) {
                /* We've been deactivated and X failed to start
                 */
                dump_details_and_quit_splash (state);
                quit_program (state);
        } else if (state->boot_splash != NULL) {
                ply_boot_splash_become_idle (state->boot_splash,
                                             (ply_boot_splash_on_idle_handler_t)
                                             on_boot_splash_idle,
                                             state);
        } else {
                quit_program (state);
        }
}

static bool
on_has_active_vt (state_t *state)
{
        if (state->local_console_terminal != NULL)
                return ply_terminal_is_active (state->local_console_terminal);
        else
                return false;
}

static ply_boot_server_t *
start_boot_server (state_t *state)
{
        ply_boot_server_t *server;

        server = ply_boot_server_new ((ply_boot_server_update_handler_t) on_update,
                                      (ply_boot_server_change_mode_handler_t) on_change_mode,
                                      (ply_boot_server_system_update_handler_t) on_system_update,
                                      (ply_boot_server_ask_for_password_handler_t) on_ask_for_password,
                                      (ply_boot_server_ask_question_handler_t) on_ask_question,
                                      (ply_boot_server_display_message_handler_t) on_display_message,
                                      (ply_boot_server_hide_message_handler_t) on_hide_message,
                                      (ply_boot_server_watch_for_keystroke_handler_t) on_watch_for_keystroke,
                                      (ply_boot_server_ignore_keystroke_handler_t) on_ignore_keystroke,
                                      (ply_boot_server_progress_pause_handler_t) on_progress_pause,
                                      (ply_boot_server_progress_unpause_handler_t) on_progress_unpause,
                                      (ply_boot_server_show_splash_handler_t) on_show_splash,
                                      (ply_boot_server_hide_splash_handler_t) on_hide_splash,
                                      (ply_boot_server_newroot_handler_t) on_newroot,
                                      (ply_boot_server_system_initialized_handler_t) on_system_initialized,
                                      (ply_boot_server_error_handler_t) on_error,
                                      (ply_boot_server_deactivate_handler_t) on_deactivate,
                                      (ply_boot_server_reactivate_handler_t) on_reactivate,
                                      (ply_boot_server_quit_handler_t) on_quit,
                                      (ply_boot_server_has_active_vt_handler_t) on_has_active_vt,
                                      state);

        if (!ply_boot_server_listen (server)) {
                ply_save_errno ();
                ply_boot_server_free (server);
                ply_restore_errno ();
                return NULL;
        }

        ply_boot_server_attach_to_event_loop (server, state->loop);

        return server;
}


static void
update_display (state_t *state)
{
        if (!state->boot_splash) return;

        ply_list_node_t *node;
        node = ply_list_get_first_node (state->entry_triggers);
        if (node) {
                ply_entry_trigger_t *entry_trigger = ply_list_node_get_data (node);
                if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_PASSWORD) {
                        int bullets = ply_utf8_string_get_length (ply_buffer_get_bytes (state->entry_buffer),
                                                                  ply_buffer_get_size (state->entry_buffer));
                        bullets = MAX (0, bullets);
                        ply_boot_splash_display_password (state->boot_splash,
                                                          entry_trigger->prompt,
                                                          bullets);
                } else if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_QUESTION) {
                        ply_boot_splash_display_question (state->boot_splash,
                                                          entry_trigger->prompt,
                                                          ply_buffer_get_bytes (state->entry_buffer));
                } else {
                        ply_trace ("unkown entry type");
                }
        } else {
                ply_boot_splash_display_normal (state->boot_splash);
        }
}

static void
toggle_between_splash_and_details (state_t *state)
{
        ply_trace ("toggling between splash and details");
        if (state->boot_splash != NULL) {
                ply_trace ("hiding and freeing current splash");
                hide_splash (state);
                ply_boot_splash_free (state->boot_splash);
                state->boot_splash = NULL;
        }

        if (!state->showing_details) {
                show_detailed_splash (state);
                state->showing_details = true;
        } else {
                show_default_splash (state);
                state->showing_details = false;
        }
}

static void
on_escape_pressed (state_t *state)
{
        ply_trace ("escape key pressed");
        toggle_between_splash_and_details (state);
}

static void
on_keyboard_input (state_t    *state,
                   const char *keyboard_input,
                   size_t      character_size)
{
        ply_list_node_t *node;

        node = ply_list_get_first_node (state->entry_triggers);
        if (node) { /* \x3 (ETX) is Ctrl+C and \x4 (EOT) is Ctrl+D */
                if (character_size == 1 && (keyboard_input[0] == '\x3' || keyboard_input[0] == '\x4')) {
                        ply_entry_trigger_t *entry_trigger = ply_list_node_get_data (node);
                        ply_trigger_pull (entry_trigger->trigger, "\x3");
                        ply_buffer_clear (state->entry_buffer);
                        ply_list_remove_node (state->entry_triggers, node);
                        free (entry_trigger);
                } else {
                        ply_buffer_append_bytes (state->entry_buffer, keyboard_input, character_size);
                }
                update_display (state);
        } else {
                for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                     node = ply_list_get_next_node (state->keystroke_triggers, node)) {
                        ply_keystroke_watch_t *keystroke_trigger = ply_list_node_get_data (node);
                        if (!keystroke_trigger->keys || strstr (keystroke_trigger->keys, keyboard_input)) { /* assume strstr works on utf8 arrays */
                                ply_trigger_pull (keystroke_trigger->trigger, keyboard_input);
                                ply_list_remove_node (state->keystroke_triggers, node);
                                free (keystroke_trigger);
                                return;
                        }
                }
                return;
        }
}

static void
on_backspace (state_t *state)
{
        ssize_t bytes_to_remove;
        ssize_t previous_character_size;
        const char *bytes;
        size_t size;
        ply_list_node_t *node = ply_list_get_first_node (state->entry_triggers);

        if (!node) return;

        bytes = ply_buffer_get_bytes (state->entry_buffer);
        size = ply_buffer_get_size (state->entry_buffer);
        if (size == 0)
                return;

        bytes_to_remove = MIN (size, PLY_UTF8_CHARACTER_SIZE_MAX);
        while ((previous_character_size = ply_utf8_character_get_size (bytes + size - bytes_to_remove, bytes_to_remove)) < bytes_to_remove) {
                if (previous_character_size > 0)
                        bytes_to_remove -= previous_character_size;
                else
                        bytes_to_remove--;
        }

        ply_buffer_remove_bytes_at_end (state->entry_buffer, bytes_to_remove);
        update_display (state);
}

static void
on_enter (state_t    *state,
          const char *line)
{
        ply_list_node_t *node;

        node = ply_list_get_first_node (state->entry_triggers);
        if (node) {
                ply_entry_trigger_t *entry_trigger = ply_list_node_get_data (node);
                const char *reply_text = ply_buffer_get_bytes (state->entry_buffer);
                ply_trigger_pull (entry_trigger->trigger, reply_text);
                ply_buffer_clear (state->entry_buffer);
                ply_list_remove_node (state->entry_triggers, node);
                free (entry_trigger);
                update_display (state);
        } else {
                for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                     node = ply_list_get_next_node (state->keystroke_triggers, node)) {
                        ply_keystroke_watch_t *keystroke_trigger = ply_list_node_get_data (node);
                        if (!keystroke_trigger->keys || strstr (keystroke_trigger->keys, "\n")) { /* assume strstr works on utf8 arrays */
                                ply_trigger_pull (keystroke_trigger->trigger, line);
                                ply_list_remove_node (state->keystroke_triggers, node);
                                free (keystroke_trigger);
                                return;
                        }
                }
                return;
        }
}

static void
attach_splash_to_devices (state_t           *state,
                          ply_boot_splash_t *splash)
{
        ply_list_t *keyboards;
        ply_list_t *pixel_displays;
        ply_list_t *text_displays;
        ply_list_node_t *node;

        keyboards = ply_device_manager_get_keyboards (state->device_manager);
        node = ply_list_get_first_node (keyboards);
        while (node != NULL) {
                ply_keyboard_t *keyboard;
                ply_list_node_t *next_node;

                keyboard = ply_list_node_get_data (node);
                next_node = ply_list_get_next_node (keyboards, node);

                ply_boot_splash_set_keyboard (splash, keyboard);

                node = next_node;
        }

        pixel_displays = ply_device_manager_get_pixel_displays (state->device_manager);
        node = ply_list_get_first_node (pixel_displays);
        while (node != NULL) {
                ply_pixel_display_t *pixel_display;
                ply_list_node_t *next_node;

                pixel_display = ply_list_node_get_data (node);
                next_node = ply_list_get_next_node (pixel_displays, node);

                ply_boot_splash_add_pixel_display (splash, pixel_display);

                node = next_node;
        }

        text_displays = ply_device_manager_get_text_displays (state->device_manager);
        node = ply_list_get_first_node (text_displays);
        while (node != NULL) {
                ply_text_display_t *text_display;
                ply_list_node_t *next_node;

                text_display = ply_list_node_get_data (node);
                next_node = ply_list_get_next_node (text_displays, node);

                ply_boot_splash_add_text_display (splash, text_display);

                node = next_node;
        }
}

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
static void
tell_systemd_to_print_details (state_t *state)
{
        ply_trace ("telling systemd to start printing details");
        if (kill (1, SIGRTMIN + 20) < 0)
                ply_trace ("could not tell systemd to print details: %m");
}

static void
tell_systemd_to_stop_printing_details (state_t *state)
{
        ply_trace ("telling systemd to stop printing details");
        if (kill (1, SIGRTMIN + 21) < 0)
                ply_trace ("could not tell systemd to stop printing details: %m");
}
#endif

static ply_boot_splash_t *
load_built_in_theme (state_t *state)
{
        ply_boot_splash_t *splash;
        bool is_loaded;

        ply_trace ("Loading built-in theme");

        splash = ply_boot_splash_new ("",
                                      PLYMOUTH_PLUGIN_PATH,
                                      state->boot_buffer);

        is_loaded = ply_boot_splash_load_built_in (splash);

        if (!is_loaded) {
                ply_save_errno ();
                ply_boot_splash_free (splash);
                ply_restore_errno ();
                return NULL;
        }

        ply_trace ("attaching plugin to event loop");
        ply_boot_splash_attach_to_event_loop (splash, state->loop);

        ply_trace ("attaching progress to plugin");
        ply_boot_splash_attach_progress (splash, state->progress);

        return splash;
}

static ply_boot_splash_t *
load_theme (state_t    *state,
            const char *theme_path)
{
        ply_boot_splash_t *splash;
        bool is_loaded;

        ply_trace ("Loading boot splash theme '%s'",
                   theme_path);

        splash = ply_boot_splash_new (theme_path,
                                      PLYMOUTH_PLUGIN_PATH,
                                      state->boot_buffer);

        is_loaded = ply_boot_splash_load (splash);

        if (!is_loaded) {
                ply_save_errno ();
                ply_boot_splash_free (splash);
                ply_restore_errno ();
                return NULL;
        }

        ply_trace ("attaching plugin to event loop");
        ply_boot_splash_attach_to_event_loop (splash, state->loop);

        ply_trace ("attaching progress to plugin");
        ply_boot_splash_attach_progress (splash, state->progress);

        return splash;
}

static ply_boot_splash_t *
show_theme (state_t    *state,
            const char *theme_path)
{
        ply_boot_splash_t *splash;

        if (theme_path != NULL)
                splash = load_theme (state, theme_path);
        else
                splash = load_built_in_theme (state);

        if (splash == NULL)
                return NULL;

        attach_splash_to_devices (state, splash);
        if (ply_boot_splash_uses_pixel_displays (splash))
                ply_device_manager_activate_renderers (state->device_manager);

        if (!ply_boot_splash_show (splash, state->mode)) {
                ply_save_errno ();
                ply_boot_splash_free (splash);
                ply_restore_errno ();
                return NULL;
        }

        ply_device_manager_activate_keyboards (state->device_manager);

        return splash;
}

static bool
attach_to_running_session (state_t *state)
{
        ply_terminal_session_t *session;
        ply_terminal_session_flags_t flags;
        bool should_be_redirected;

        flags = 0;

        should_be_redirected = !state->no_boot_log;

        if (should_be_redirected)
                flags |= PLY_TERMINAL_SESSION_FLAGS_REDIRECT_CONSOLE;

        if (state->session == NULL) {
                ply_trace ("creating new terminal session");
                session = ply_terminal_session_new (NULL);

                ply_terminal_session_attach_to_event_loop (session, state->loop);
        } else {
                session = state->session;
                ply_trace ("session already created");
        }

        if (!ply_terminal_session_attach (session, flags,
                                          (ply_terminal_session_output_handler_t)
                                          on_session_output,
                                          (ply_terminal_session_hangup_handler_t)
                                          (should_be_redirected ? on_session_hangup : NULL),
                                          -1, state)) {
                ply_save_errno ();
                ply_terminal_session_free (session);
                ply_buffer_free (state->boot_buffer);
                state->boot_buffer = NULL;
                ply_restore_errno ();

                state->is_redirected = false;
                state->is_attached = false;
                return false;
        }

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
        tell_systemd_to_print_details (state);
#endif

        state->is_redirected = should_be_redirected;
        state->is_attached = true;
        state->session = session;

        return true;
}

static void
detach_from_running_session (state_t *state)
{
        if (state->session == NULL)
                return;

        if (!state->is_attached)
                return;

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
        tell_systemd_to_stop_printing_details (state);
#endif

        ply_trace ("detaching from terminal session");
        ply_terminal_session_detach (state->session);
        state->is_redirected = false;
        state->is_attached = false;
}

static void
check_verbosity (state_t *state)
{
        char *stream;

        ply_trace ("checking if tracing should be enabled");

        if (!debug_buffer_path)
                debug_buffer_path = ply_kernel_command_line_get_key_value ("plymouth.debug=file:");

        stream = ply_kernel_command_line_get_key_value ("plymouth.debug=stream:");
        if (stream != NULL || debug_buffer_path != NULL ||
            ply_kernel_command_line_has_argument ("plymouth.debug")) {
                int fd;

                ply_trace ("tracing should be enabled!");
                if (!ply_is_tracing ())
                        ply_toggle_tracing ();

                if (debug_buffer == NULL)
                        debug_buffer = ply_buffer_new ();

                if (stream != NULL) {
                        ply_trace ("streaming debug output to %s instead of screen", stream);
                        fd = open (stream, O_RDWR | O_NOCTTY | O_CREAT, 0600);

                        if (fd < 0)
                                ply_trace ("could not stream output to %s: %m", stream);
                        else
                                ply_logger_set_output_fd (ply_logger_get_error_default (), fd);
                        free (stream);
                } else {
                        const char *device;
                        char *file;

                        device = state->default_tty;

                        ply_trace ("redirecting debug output to %s", device);

                        if (strncmp (device, "/dev/", strlen ("/dev/")) == 0)
                                file = strdup (device);
                        else
                                asprintf (&file, "/dev/%s", device);

                        fd = open (file, O_RDWR | O_APPEND);

                        if (fd < 0)
                                ply_trace ("could not redirected debug output to %s: %m", device);
                        else
                                ply_logger_set_output_fd (ply_logger_get_error_default (), fd);

                        free (file);
                }
        } else {
                ply_trace ("tracing shouldn't be enabled!");
        }

        if (debug_buffer != NULL) {
                if (debug_buffer_path == NULL)
                        debug_buffer_path = strdup (PLYMOUTH_LOG_DIRECTORY "/plymouth-debug.log");

                ply_logger_add_filter (ply_logger_get_error_default (),
                                       (ply_logger_filter_handler_t)
                                       on_error_message,
                                       debug_buffer);
        }
}

static void
check_logging (state_t *state)
{
        bool kernel_no_log;

        ply_trace ("checking if console messages should be redirected and logged");

        kernel_no_log = ply_kernel_command_line_has_argument ("plymouth.nolog");
        if (kernel_no_log)
                state->no_boot_log = true;

        if (state->no_boot_log)
                ply_trace ("logging won't be enabled!");
        else
                ply_trace ("logging will be enabled!");
}

static bool
redirect_standard_io_to_dev_null (void)
{
        int fd;

        fd = open ("/dev/null", O_RDWR | O_APPEND);

        if (fd < 0)
                return false;

        dup2 (fd, STDIN_FILENO);
        dup2 (fd, STDOUT_FILENO);
        dup2 (fd, STDERR_FILENO);

        close (fd);

        return true;
}
static const char *
find_fallback_tty (state_t *state)
{
        static const char *tty_list[] =
        {
                "/dev/ttyS0",
                "/dev/hvc0",
                "/dev/xvc0",
                "/dev/ttySG0",
                NULL
        };
        int i;

        for (i = 0; tty_list[i] != NULL; i++) {
                if (ply_character_device_exists (tty_list[i]))
                        return tty_list[i];
        }

        return state->default_tty;
}

static bool
initialize_environment (state_t *state)
{
        ply_trace ("initializing minimal work environment");

        if (!state->default_tty)
                if (getenv ("DISPLAY") != NULL && access (PLYMOUTH_PLUGIN_PATH "renderers/x11.so", F_OK) == 0)
                        state->default_tty = "/dev/tty";
        if (!state->default_tty) {
                if (state->mode == PLY_BOOT_SPLASH_MODE_SHUTDOWN ||
                    state->mode == PLY_BOOT_SPLASH_MODE_REBOOT)
                        state->default_tty = SHUTDOWN_TTY;
                else
                        state->default_tty = BOOT_TTY;

                ply_trace ("checking if '%s' exists", state->default_tty);
                if (!ply_character_device_exists (state->default_tty)) {
                        ply_trace ("nope, forcing details mode");
                        state->should_force_details = true;

                        state->default_tty = find_fallback_tty (state);
                        ply_trace ("going to go with '%s'", state->default_tty);
                }
        }

        check_verbosity (state);
        check_logging (state);

        ply_trace ("source built on %s", __DATE__);

        state->keystroke_triggers = ply_list_new ();
        state->entry_triggers = ply_list_new ();
        state->entry_buffer = ply_buffer_new ();
        state->messages = ply_list_new ();

        if (!ply_is_tracing ())
                redirect_standard_io_to_dev_null ();

        ply_trace ("Making sure " PLYMOUTH_RUNTIME_DIR " exists");
        if (!ply_create_directory (PLYMOUTH_RUNTIME_DIR))
                ply_trace ("could not create " PLYMOUTH_RUNTIME_DIR ": %m");

        ply_trace ("initialized minimal work environment");
        return true;
}

static void
on_error_message (ply_buffer_t *debug_buffer,
                  const void   *bytes,
                  size_t        number_of_bytes)
{
        ply_buffer_append_bytes (debug_buffer, bytes, number_of_bytes);
}

static void
dump_debug_buffer_to_file (void)
{
        int fd;
        const char *bytes;
        size_t size;

        fd = open (debug_buffer_path,
                   O_WRONLY | O_CREAT | O_TRUNC, 0600);

        if (fd < 0)
                return;

        size = ply_buffer_get_size (debug_buffer);
        bytes = ply_buffer_get_bytes (debug_buffer);
        ply_write (fd, bytes, size);
        close (fd);
}

#include <termios.h>
#include <unistd.h>
static void
on_crash (int signum)
{
        struct termios term_attributes;
        int fd;
        static const char *show_cursor_sequence = "\033[?25h";

        fd = open ("/dev/tty1", O_RDWR | O_NOCTTY);
        if (fd < 0) fd = open ("/dev/hvc0", O_RDWR | O_NOCTTY);

        ioctl (fd, KDSETMODE, KD_TEXT);

        write (fd, show_cursor_sequence, sizeof (show_cursor_sequence) - 1);

        tcgetattr (fd, &term_attributes);

        term_attributes.c_iflag |= BRKINT | IGNPAR | ICRNL | IXON;
        term_attributes.c_oflag |= OPOST;
        term_attributes.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;

        tcsetattr (fd, TCSAFLUSH, &term_attributes);

        close (fd);

        if (debug_buffer != NULL) {
                dump_debug_buffer_to_file ();
                sleep (30);
        }

        if (pid_file != NULL) {
                unlink (pid_file);
                free (pid_file);
                pid_file = NULL;
        }

        signal (signum, SIG_DFL);
        raise (signum);
}

static void
write_pid_file (const char *filename)
{
        FILE *fp;

        fp = fopen (filename, "w");
        if (fp == NULL) {
                ply_error ("could not write pid file %s: %m", filename);
        } else {
                fprintf (fp, "%d\n", (int) getpid ());
                fclose (fp);
        }
}

int
main (int    argc,
      char **argv)
{
        state_t state = { 0 };
        int exit_code;
        bool should_help = false;
        bool no_boot_log = false;
        bool no_daemon = false;
        bool debug = false;
        bool attach_to_session;
        ply_daemon_handle_t *daemon_handle = NULL;
        char *mode_string = NULL;
        char *kernel_command_line = NULL;
        char *tty = NULL;
        ply_device_manager_flags_t device_manager_flags = PLY_DEVICE_MANAGER_FLAGS_NONE;

        state.start_time = ply_get_timestamp ();
        state.command_parser = ply_command_parser_new ("plymouthd", "Splash server");

        state.loop = ply_event_loop_get_default ();

        ply_command_parser_add_options (state.command_parser,
                                        "help", "This help message", PLY_COMMAND_OPTION_TYPE_FLAG,
                                        "attach-to-session", "Redirect console messages from screen to log", PLY_COMMAND_OPTION_TYPE_FLAG,
                                        "no-daemon", "Do not daemonize", PLY_COMMAND_OPTION_TYPE_FLAG,
                                        "debug", "Output debugging information", PLY_COMMAND_OPTION_TYPE_FLAG,
                                        "debug-file", "File to output debugging information to", PLY_COMMAND_OPTION_TYPE_STRING,
                                        "mode", "Mode is one of: boot, shutdown", PLY_COMMAND_OPTION_TYPE_STRING,
                                        "pid-file", "Write the pid of the daemon to a file", PLY_COMMAND_OPTION_TYPE_STRING,
                                        "kernel-command-line", "Fake kernel command line to use", PLY_COMMAND_OPTION_TYPE_STRING,
                                        "tty", "TTY to use instead of default", PLY_COMMAND_OPTION_TYPE_STRING,
                                        "no-boot-log", "Do not write boot log file", PLY_COMMAND_OPTION_TYPE_FLAG,
                                        NULL);

        if (!ply_command_parser_parse_arguments (state.command_parser, state.loop, argv, argc)) {
                char *help_string;

                help_string = ply_command_parser_get_help_string (state.command_parser);

                ply_error_without_new_line ("%s", help_string);

                free (help_string);
                return EX_USAGE;
        }

        ply_command_parser_get_options (state.command_parser,
                                        "help", &should_help,
                                        "attach-to-session", &attach_to_session,
                                        "mode", &mode_string,
                                        "no-boot-log", &no_boot_log,
                                        "no-daemon", &no_daemon,
                                        "debug", &debug,
                                        "debug-file", &debug_buffer_path,
                                        "pid-file", &pid_file,
                                        "tty", &tty,
                                        "kernel-command-line", &kernel_command_line,
                                        NULL);

        if (should_help) {
                char *help_string;

                help_string = ply_command_parser_get_help_string (state.command_parser);

                if (argc < 2)
                        fprintf (stderr, "%s", help_string);
                else
                        printf ("%s", help_string);

                free (help_string);
                return 0;
        }

        if (debug && !ply_is_tracing ())
                ply_toggle_tracing ();

        if (mode_string != NULL) {
                if (strcmp (mode_string, "shutdown") == 0)
                        state.mode = PLY_BOOT_SPLASH_MODE_SHUTDOWN;
                else if (strcmp (mode_string, "reboot") == 0)
                        state.mode = PLY_BOOT_SPLASH_MODE_REBOOT;
                else if (strcmp (mode_string, "updates") == 0)
                        state.mode = PLY_BOOT_SPLASH_MODE_UPDATES;
                else if (strcmp (mode_string, "system-upgrade") == 0)
                        state.mode = PLY_BOOT_SPLASH_MODE_SYSTEM_UPGRADE;
                else if (strcmp (mode_string, "firmware-upgrade") == 0)
                        state.mode = PLY_BOOT_SPLASH_MODE_FIRMWARE_UPGRADE;
                else
                        state.mode = PLY_BOOT_SPLASH_MODE_BOOT_UP;

                free (mode_string);
        }

        if (tty != NULL)
                state.default_tty = tty;

        if (kernel_command_line != NULL)
                ply_kernel_command_line_override (kernel_command_line);

        if (geteuid () != 0) {
                ply_error ("plymouthd must be run as root user");
                return EX_OSERR;
        }

        state.no_boot_log = no_boot_log;

        chdir ("/");
        signal (SIGPIPE, SIG_IGN);

        if (!no_daemon) {
                daemon_handle = ply_create_daemon ();

                if (daemon_handle == NULL) {
                        ply_error ("plymouthd: cannot daemonize: %m");
                        return EX_UNAVAILABLE;
                }
        }

        if (debug)
                debug_buffer = ply_buffer_new ();

        signal (SIGABRT, on_crash);
        signal (SIGSEGV, on_crash);

        /* before do anything we need to make sure we have a working
         * environment.
         */
        if (!initialize_environment (&state)) {
                if (errno == 0) {
                        if (daemon_handle != NULL)
                                ply_detach_daemon (daemon_handle, 0);
                        return 0;
                }

                ply_error ("plymouthd: could not setup basic operating environment: %m");
                if (daemon_handle != NULL)
                        ply_detach_daemon (daemon_handle, EX_OSERR);
                return EX_OSERR;
        }

        /* Make the first byte in argv be '@' so that we can survive systemd's killing
         * spree when going from initrd to /, and so we stay alive all the way until
         * the power is killed at shutdown.
         * http://www.freedesktop.org/wiki/Software/systemd/RootStorageDaemons
         */
        argv[0][0] = '@';

        state.boot_server = start_boot_server (&state);

        if (state.boot_server == NULL) {
                ply_trace ("plymouthd is already running");

                if (daemon_handle != NULL)
                        ply_detach_daemon (daemon_handle, EX_OK);
                return EX_OK;
        }

        state.boot_buffer = ply_buffer_new ();

        if (attach_to_session) {
                state.should_be_attached = attach_to_session;
                if (!attach_to_running_session (&state)) {
                        ply_trace ("could not redirect console session: %m");
                        if (!no_daemon)
                                ply_detach_daemon (daemon_handle, EX_UNAVAILABLE);
                        return EX_UNAVAILABLE;
                }
        }

        state.progress = ply_progress_new ();
        state.splash_delay = NAN;
        state.device_timeout = NAN;

        ply_progress_load_cache (state.progress,
                                 get_cache_file_for_mode (state.mode));

        if (pid_file != NULL)
                write_pid_file (pid_file);

        if (daemon_handle != NULL
            && !ply_detach_daemon (daemon_handle, 0)) {
                ply_error ("plymouthd: could not tell parent to exit: %m");
                return EX_UNAVAILABLE;
        }

        find_override_splash (&state);
        find_system_default_splash (&state);
        find_distribution_default_splash (&state);

        if (ply_kernel_command_line_has_argument ("plymouth.ignore-serial-consoles"))
                device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_IGNORE_SERIAL_CONSOLES;

        if (ply_kernel_command_line_has_argument ("plymouth.ignore-udev") ||
            (getenv ("DISPLAY") != NULL))
                device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_IGNORE_UDEV;

        if (!plymouth_should_show_default_splash (&state)) {
                /* don't bother listening for udev events or setting up a graphical renderer
                 * if we're forcing details */
                device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_SKIP_RENDERERS;
                device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_IGNORE_UDEV;

                /* don't ever delay showing the detailed splash */
                state.splash_delay = NAN;
        }

        find_force_scale (&state);

        load_devices (&state, device_manager_flags);

        ply_trace ("entering event loop");
        exit_code = ply_event_loop_run (state.loop);
        ply_trace ("exited event loop");

        ply_boot_splash_free (state.boot_splash);
        state.boot_splash = NULL;

        ply_command_parser_free (state.command_parser);

        ply_boot_server_free (state.boot_server);
        state.boot_server = NULL;

        ply_trace ("freeing terminal session");
        ply_terminal_session_free (state.session);

        ply_buffer_free (state.boot_buffer);
        ply_progress_free (state.progress);

        ply_trace ("exiting with code %d", exit_code);

        if (debug_buffer != NULL) {
                dump_debug_buffer_to_file ();
                ply_buffer_free (debug_buffer);
        }

        ply_free_error_log ();

        return exit_code;
}
/* vim: set ts=4 ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
