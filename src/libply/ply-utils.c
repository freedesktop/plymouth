/* ply-utils.c -  random useful functions and macros
 *
 * Copyright (C) 2007 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include <config.h>

#include "ply-utils.h"

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <linux/fs.h>
#include <linux/vt.h>

#include <dlfcn.h>

#include "ply-logger.h"

#ifndef PLY_OPEN_FILE_DESCRIPTORS_DIR
#define PLY_OPEN_FILE_DESCRIPTORS_DIR "/proc/self/fd"
#endif

#ifndef PLY_ERRNO_STACK_SIZE
#define PLY_ERRNO_STACK_SIZE 256
#endif

#ifndef PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG
#define PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG 2
#endif

#ifndef PLY_DISABLE_CONSOLE_PRINTK
#define PLY_DISABLE_CONSOLE_PRINTK 6
#endif

#ifndef PLY_ENABLE_CONSOLE_PRINTK
#define PLY_ENABLE_CONSOLE_PRINTK 7
#endif

#ifndef PLY_MAX_COMMAND_LINE_SIZE
#define PLY_MAX_COMMAND_LINE_SIZE 4096
#endif

static int errno_stack[PLY_ERRNO_STACK_SIZE];
static int errno_stack_position = 0;

static int overridden_device_scale = 0;
static int overridden_device_orientation = 0;

static char kernel_command_line[PLY_MAX_COMMAND_LINE_SIZE];
static bool kernel_command_line_is_set;

bool
ply_open_unidirectional_pipe (int *sender_fd,
                              int *receiver_fd)
{
        int pipe_fds[2];

        assert (sender_fd != NULL);
        assert (receiver_fd != NULL);

        if (pipe2 (pipe_fds, O_CLOEXEC) < 0)
                return false;

        *sender_fd = pipe_fds[1];
        *receiver_fd = pipe_fds[0];

        return true;
}

static int
ply_open_unix_socket (void)
{
        int fd;
        const int should_pass_credentials = true;

        fd = socket (PF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

        if (fd < 0)
                return -1;

        if (setsockopt (fd, SOL_SOCKET, SO_PASSCRED,
                        &should_pass_credentials, sizeof(should_pass_credentials)) < 0) {
                ply_save_errno ();
                close (fd);
                ply_restore_errno ();
                return -1;
        }

        return fd;
}

static struct sockaddr *
create_unix_address_from_path (const char            *path,
                               ply_unix_socket_type_t type,
                               size_t                *address_size)
{
        struct sockaddr_un *address;

        assert (path != NULL && path[0] != '\0');
        assert (strlen (path) < sizeof(address->sun_path));

        address = calloc (1, sizeof(struct sockaddr_un));
        address->sun_family = AF_UNIX;

        /* a socket is marked as abstract if its path has the
         * NUL byte at the beginning of the buffer.
         *
         * Note, we depend on the memory being zeroed by the calloc
         * call above.
         */
        if (type == PLY_UNIX_SOCKET_TYPE_CONCRETE)
                strncpy (address->sun_path, path, sizeof(address->sun_path) - 1);
        else
                strncpy (address->sun_path + 1, path, sizeof(address->sun_path) - 1);

        assert (address_size != NULL);

        /* it's very popular to trim the trailing zeros off the end of the path these
         * days for abstract sockets.  Unfortunately, the 0s are part of the name, so
         * both client and server have to agree.
         */
        if (type == PLY_UNIX_SOCKET_TYPE_TRIMMED_ABSTRACT) {
                *address_size = offsetof (struct sockaddr_un, sun_path)
                                + 1 /* NUL */ +
                                strlen (address->sun_path + 1) /* path */;
        } else {
                *address_size = sizeof(struct sockaddr_un);
        }

        return (struct sockaddr *) address;
}

int
ply_connect_to_unix_socket (const char            *path,
                            ply_unix_socket_type_t type)
{
        struct sockaddr *address;
        size_t address_size;
        int fd;

        assert (path != NULL);
        assert (path[0] != '\0');

        fd = ply_open_unix_socket ();

        if (fd < 0)
                return -1;

        address = create_unix_address_from_path (path, type, &address_size);

        if (connect (fd, address, address_size) < 0) {
                ply_save_errno ();
                free (address);
                close (fd);
                ply_restore_errno ();

                return -1;
        }
        free (address);

        return fd;
}

int
ply_listen_to_unix_socket (const char            *path,
                           ply_unix_socket_type_t type)
{
        struct sockaddr *address;
        size_t address_size;
        int fd;

        assert (path != NULL);
        assert (path[0] != '\0');

        fd = ply_open_unix_socket ();

        if (fd < 0)
                return -1;

        address = create_unix_address_from_path (path, type, &address_size);

        if (bind (fd, address, address_size) < 0) {
                ply_save_errno ();
                free (address);
                close (fd);
                ply_restore_errno ();

                return -1;
        }

        free (address);

        if (listen (fd, SOMAXCONN) < 0) {
                ply_save_errno ();
                close (fd);
                ply_restore_errno ();
                return -1;
        }

        if (type == PLY_UNIX_SOCKET_TYPE_CONCRETE) {
                if (fchmod (fd, 0600) < 0) {
                        ply_save_errno ();
                        close (fd);
                        ply_restore_errno ();
                        return -1;
                }
        }

        return fd;
}

bool
ply_get_credentials_from_fd (int    fd,
                             pid_t *pid,
                             uid_t *uid,
                             gid_t *gid)
{
        struct ucred credentials;
        socklen_t credential_size;

        credential_size = sizeof(credentials);
        if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                        &credential_size) < 0)
                return false;

        if (credential_size < sizeof(credentials))
                return false;

        if (pid != NULL)
                *pid = credentials.pid;

        if (uid != NULL)
                *uid = credentials.uid;

        if (gid != NULL)
                *gid = credentials.gid;

        return true;
}

bool
ply_write (int         fd,
           const void *buffer,
           size_t      number_of_bytes)
{
        size_t bytes_left_to_write;
        size_t total_bytes_written = 0;

        assert (fd >= 0);

        bytes_left_to_write = number_of_bytes;

        do {
                ssize_t bytes_written = 0;

                bytes_written = write (fd,
                                       ((uint8_t *) buffer) + total_bytes_written,
                                       bytes_left_to_write);

                if (bytes_written > 0) {
                        total_bytes_written += bytes_written;
                        bytes_left_to_write -= bytes_written;
                } else if ((errno != EINTR)) {
                        break;
                }
        } while (bytes_left_to_write > 0);

        return bytes_left_to_write == 0;
}

bool
ply_write_uint32 (int      fd,
                  uint32_t value)
{
        uint8_t buffer[4];

        buffer[0] = (value >> 0) & 0xFF;
        buffer[1] = (value >> 8) & 0xFF;
        buffer[2] = (value >> 16) & 0xFF;
        buffer[3] = (value >> 24) & 0xFF;

        return ply_write (fd, buffer, 4 * sizeof(uint8_t));
}

static ssize_t
ply_read_some_bytes (int    fd,
                     void  *buffer,
                     size_t max_bytes)
{
        size_t bytes_left_to_read;
        size_t total_bytes_read = 0;

        assert (fd >= 0);

        bytes_left_to_read = max_bytes;

        do {
                ssize_t bytes_read = 0;

                bytes_read = read (fd,
                                   ((uint8_t *) buffer) + total_bytes_read,
                                   bytes_left_to_read);

                if (bytes_read > 0) {
                        total_bytes_read += bytes_read;
                        bytes_left_to_read -= bytes_read;
                } else if ((errno != EINTR)) {
                        break;
                }
        } while (bytes_left_to_read > 0);

        if ((bytes_left_to_read > 0) && (errno != EAGAIN))
                total_bytes_read = -1;

        return total_bytes_read;
}

bool
ply_read (int    fd,
          void  *buffer,
          size_t number_of_bytes)
{
        size_t total_bytes_read;
        bool read_was_successful;

        assert (fd >= 0);
        assert (buffer != NULL);
        assert (number_of_bytes != 0);

        total_bytes_read = ply_read_some_bytes (fd, buffer, number_of_bytes);

        read_was_successful = total_bytes_read == number_of_bytes;

        return read_was_successful;
}

bool
ply_read_uint32 (int       fd,
                 uint32_t *value)
{
        uint8_t buffer[4];

        if (!ply_read (fd, buffer, 4 * sizeof(uint8_t)))
                return false;

        *value = (buffer[0] << 0) |
                 (buffer[1] << 8) |
                 (buffer[2] << 16) |
                 (buffer[3] << 24);
        return true;
}

bool
ply_fd_has_data (int fd)
{
        struct pollfd poll_data;
        int result;

        poll_data.fd = fd;
        poll_data.events = POLLIN | POLLPRI;
        poll_data.revents = 0;
        result = poll (&poll_data, 1, 10);

        return result == 1
               && ((poll_data.revents & POLLIN)
                   || (poll_data.revents & POLLPRI));
}

bool
ply_set_fd_as_blocking (int fd)
{
        int flags;
        int ret = 0;

        assert (fd >= 0);

        flags = fcntl (fd, F_GETFL);

        if (flags == -1) {
                return false;
        }

        if (flags & O_NONBLOCK) {
                flags &= ~O_NONBLOCK;

                ret = fcntl (fd, F_SETFL, flags);
        }

        return ret == 0;
}

char **
ply_copy_string_array (const char *const *array)
{
        char **copy;
        int i;

        for (i = 0; array[i] != NULL; i++) {
        }

        copy = calloc (i + 1, sizeof(char *));

        for (i = 0; array[i] != NULL; i++) {
                copy[i] = strdup (array[i]);
        }

        return copy;
}

void
ply_free_string_array (char **array)
{
        int i;

        if (array == NULL)
                return;

        for (i = 0; array[i] != NULL; i++) {
                free (array[i]);
                array[i] = NULL;
        }

        free (array);
}

double
ply_get_timestamp (void)
{
        const double nanoseconds_per_second = 1000000000.0;
        double timestamp;
        struct timespec now = { 0L, /* zero-filled */ };

        clock_gettime (CLOCK_MONOTONIC, &now);
        timestamp = ((nanoseconds_per_second * now.tv_sec) + now.tv_nsec) /
                    nanoseconds_per_second;

        return timestamp;
}

void
ply_save_errno (void)
{
        assert (errno_stack_position < PLY_ERRNO_STACK_SIZE);
        errno_stack[errno_stack_position] = errno;
        errno_stack_position++;
}

void
ply_restore_errno (void)
{
        assert (errno_stack_position > 0);
        errno_stack_position--;
        errno = errno_stack[errno_stack_position];
}

bool
ply_directory_exists (const char *dir)
{
        struct stat file_info;

        if (stat (dir, &file_info) < 0)
                return false;

        return S_ISDIR (file_info.st_mode);
}

bool
ply_file_exists (const char *file)
{
        struct stat file_info;

        if (stat (file, &file_info) < 0)
                return false;

        return S_ISREG (file_info.st_mode);
}

bool
ply_character_device_exists (const char *device)
{
        struct stat file_info;

        if (stat (device, &file_info) < 0)
                return false;

        return S_ISCHR (file_info.st_mode);
}

ply_module_handle_t *
ply_open_module (const char *module_path)
{
        ply_module_handle_t *handle;

        assert (module_path != NULL);

        handle = (ply_module_handle_t *) dlopen (module_path,
                                                 RTLD_NODELETE | RTLD_NOW | RTLD_LOCAL);

        if (handle == NULL) {
                ply_trace ("Could not load module \"%s\": %s", module_path, dlerror ());
                if (errno == 0)
                        errno = ELIBACC;
        }

        return handle;
}

ply_module_handle_t *
ply_open_built_in_module (void)
{
        ply_module_handle_t *handle;

        handle = (ply_module_handle_t *) dlopen (NULL,
                                                 RTLD_NODELETE | RTLD_NOW | RTLD_LOCAL);

        if (handle == NULL) {
                ply_trace ("Could not load built-in module: %s\n", dlerror ());
                if (errno == 0)
                        errno = ELIBACC;
        }

        return handle;
}

ply_module_function_t
ply_module_look_up_function (ply_module_handle_t *handle,
                             const char          *function_name)
{
        ply_module_function_t function;

        assert (handle != NULL);
        assert (function_name != NULL);

        dlerror ();
        function = (ply_module_function_t) dlsym (handle, function_name);

        if (dlerror () != NULL) {
                if (errno == 0)
                        errno = ELIBACC;

                return NULL;
        }

        return function;
}

void
ply_close_module (ply_module_handle_t *handle)
{
        dlclose (handle);
}

bool
ply_create_directory (const char *directory)
{
        assert (directory != NULL);
        assert (directory[0] != '\0');

        if (ply_directory_exists (directory)) {
                ply_trace ("directory '%s' already exists", directory);
                return true;
        }

        if (ply_file_exists (directory)) {
                ply_trace ("file '%s' is in the way", directory);
                errno = EEXIST;
                return false;
        }

        if (mkdir (directory, 0755) < 0) {
                char *parent_directory;
                char *last_path_component;
                bool is_created;

                is_created = errno == EEXIST;
                if (errno == ENOENT) {
                        parent_directory = strdup (directory);
                        last_path_component = strrchr (parent_directory, '/');
                        *last_path_component = '\0';

                        ply_trace ("parent directory '%s' doesn't exist, creating it first", parent_directory);
                        if (ply_create_directory (parent_directory)
                            && ((mkdir (directory, 0755) == 0) || errno == EEXIST))
                                is_created = true;

                        ply_save_errno ();
                        free (parent_directory);
                        ply_restore_errno ();
                }

                return is_created;
        }


        return true;
}

bool
ply_create_file_link (const char *source,
                      const char *destination)
{
        if (link (source, destination) < 0)
                return false;

        return true;
}

void
ply_show_new_kernel_messages (bool should_show)
{
        int type;

        if (should_show)
                type = PLY_ENABLE_CONSOLE_PRINTK;
        else
                type = PLY_DISABLE_CONSOLE_PRINTK;

        if (klogctl (type, NULL, 0) < 0)
                ply_trace ("could not toggle printk visibility: %m");
}

ply_daemon_handle_t *
ply_create_daemon (void)
{
        pid_t pid;
        int sender_fd, receiver_fd;
        int *handle;

        if (!ply_open_unidirectional_pipe (&sender_fd, &receiver_fd))
                return NULL;


        pid = fork ();

        if (pid < 0)
                return NULL;

        if (pid != 0) {
                uint8_t byte;
                close (sender_fd);

                if (!ply_read (receiver_fd, &byte, sizeof(uint8_t))) {
                        int read_error = errno;
                        int status;

                        if (waitpid (pid, &status, WNOHANG) <= 0)
                                ply_error ("failed to read status from child immediately after starting to daemonize: %s", strerror (read_error));
                        else if (WIFEXITED (status))
                                ply_error ("unexpectedly exited with status %d immediately after starting to daemonize", (int) WEXITSTATUS (status));
                        else if (WIFSIGNALED (status))
                                ply_error ("unexpectedly died from signal %s immediately after starting to daemonize", strsignal (WTERMSIG (status)));
                        _exit (1);
                }

                _exit ((int) byte);
        }
        close (receiver_fd);

        handle = calloc (1, sizeof(int));
        *handle = sender_fd;

        return (ply_daemon_handle_t *) handle;
}

bool
ply_detach_daemon (ply_daemon_handle_t *handle,
                   int                  exit_code)
{
        int sender_fd;
        uint8_t byte;

        assert (handle != NULL);
        assert (exit_code >= 0 && exit_code < 256);

        sender_fd = *(int *) handle;

        byte = (uint8_t) exit_code;

        if (!ply_write (sender_fd, &byte, sizeof(uint8_t)))
                return false;

        close (sender_fd);
        free (handle);

        return true;
}


/*                    UTF-8 encoding
 * 00000000-01111111    00-7F   US-ASCII (single byte)
 * 10000000-10111111    80-BF   Second, third, or fourth byte of a multi-byte sequence
 * 11000000-11011111    C0-DF   Start of 2-byte sequence
 * 11100000-11101111    E0-EF   Start of 3-byte sequence
 * 11110000-11110100    F0-F4   Start of 4-byte sequence
 */
int
ply_utf8_character_get_size (const char *string,
                             size_t      n)
{
        int length;

        if (n < 1) return -1;
        if (string[0] == 0x00) length = 0;
        else if ((string[0] & 0x80) == 0x00) length = 1;
        else if ((string[0] & 0xE0) == 0xC0) length = 2;
        else if ((string[0] & 0xF0) == 0xE0) length = 3;
        else if ((string[0] & 0xF8) == 0xF0) length = 4;
        else return -2;
        if (length > (int) n) return -1;
        return length;
}

int
ply_utf8_string_get_length (const char *string,
                            size_t      n)
{
        size_t count = 0;

        while (true) {
                int charlen = ply_utf8_character_get_size (string, n);
                if (charlen <= 0) break;
                string += charlen;
                n -= charlen;
                count++;
        }
        return count;
}

char *
ply_get_process_command_line (pid_t pid)
{
        char *path;
        char *command_line;
        ssize_t bytes_read;
        int fd;
        ssize_t i;

        path = NULL;
        command_line = NULL;

        asprintf (&path, "/proc/%ld/cmdline", (long) pid);

        fd = open (path, O_RDONLY);

        if (fd < 0) {
                ply_trace ("Could not open %s: %m", path);
                goto error;
        }

        command_line = calloc (PLY_MAX_COMMAND_LINE_SIZE, sizeof(char));
        bytes_read = read (fd, command_line, PLY_MAX_COMMAND_LINE_SIZE - 1);
        if (bytes_read < 0) {
                ply_trace ("Could not read %s: %m", path);
                close (fd);
                goto error;
        }
        close (fd);
        free (path);

        for (i = 0; i < bytes_read - 1; i++) {
                if (command_line[i] == '\0')
                        command_line[i] = ' ';
        }
        command_line[i] = '\0';

        return command_line;

error:
        free (path);
        free (command_line);
        return NULL;
}

pid_t
ply_get_process_parent_pid (pid_t pid)
{
        char *path;
        FILE *fp;
        int ppid;

        asprintf (&path, "/proc/%ld/stat", (long) pid);

        ppid = 0;
        fp = fopen (path, "re");

        if (fp == NULL) {
                ply_trace ("Could not open %s: %m", path);
                goto out;
        }

        if (fscanf (fp, "%*d %*s %*c %d", &ppid) != 1) {
                ply_trace ("Could not parse %s: %m", path);
                goto out;
        }

        if (ppid <= 0) {
                ply_trace ("%s is returning invalid parent pid %d", path, ppid);
                ppid = 0;
                goto out;
        }

out:
        free (path);

        if (fp != NULL)
                fclose (fp);

        return (pid_t) ppid;
}

void
ply_set_device_scale (int device_scale)
{
    overridden_device_scale = device_scale;
    ply_trace ("Device scale is set to %d", device_scale);
}

void
ply_set_device_orientation (int device_orientation)
{
    overridden_device_orientation = device_orientation;
    ply_trace ("Device orientation is set to %d", device_orientation);
}

/* The minimum resolution at which we turn on a device-scale of 2 */
#define HIDPI_LIMIT 192
#define HIDPI_MIN_HEIGHT 1200

int
ply_get_device_scale (uint32_t width,
                      uint32_t height,
                      uint32_t width_mm,
                      uint32_t height_mm)
{
        int device_scale;
        double dpi_x, dpi_y;
        const char *force_device_scale;

        device_scale = 1;

        if ((force_device_scale = getenv ("PLYMOUTH_FORCE_SCALE")))
                return strtoul (force_device_scale, NULL, 0);

        if (overridden_device_scale != 0)
                return overridden_device_scale;

        if (height < HIDPI_MIN_HEIGHT)
                return 1;

        /* Somebody encoded the aspect ratio (16/9 or 16/10)
         * instead of the physical size */
        if ((width_mm == 160 && height_mm == 90) ||
            (width_mm == 160 && height_mm == 100) ||
            (width_mm == 16 && height_mm == 9) ||
            (width_mm == 16 && height_mm == 10))
                return 1;

        if (width_mm > 0 && height_mm > 0) {
                dpi_x = (double)width / (width_mm / 25.4);
                dpi_y = (double)height / (height_mm / 25.4);
                /* We don't completely trust these values so both
                   must be high, and never pick higher ratio than
                   2 automatically */
                if (dpi_x > HIDPI_LIMIT && dpi_y > HIDPI_LIMIT)
                        device_scale = 2;
        }

        return device_scale;
}

int
ply_get_device_orientation (void)
{
        return overridden_device_orientation;
}

static const char *
ply_get_kernel_command_line (void)
{
        const char *remaining_command_line;
        char *key;
        int fd;

        if (kernel_command_line_is_set)
                return kernel_command_line;

        ply_trace ("opening /proc/cmdline");
        fd = open ("/proc/cmdline", O_RDONLY);

        if (fd < 0) {
                ply_trace ("couldn't open it: %m");
                return NULL;
        }

        ply_trace ("reading kernel command line");
        if (read (fd, kernel_command_line, sizeof(kernel_command_line) - 1) < 0) {
                ply_trace ("couldn't read it: %m");
                close (fd);
                return NULL;
        }

        /* we now use plymouth.argument for kernel commandline arguments.
         * It used to be plymouth:argument. This bit just rewrites all : to be .
         */
        remaining_command_line = kernel_command_line;
        while ((key = strstr (remaining_command_line, "plymouth:")) != NULL) {
                char *colon;

                colon = key + strlen ("plymouth");
                *colon = '.';

                remaining_command_line = colon + 1;
        }
        ply_trace ("Kernel command line is: '%s'", kernel_command_line);

        close (fd);

        kernel_command_line_is_set = true;
        return kernel_command_line;
}

const char *
ply_kernel_command_line_get_string_after_prefix (const char *prefix)
{
        const char *command_line = ply_get_kernel_command_line();
        char *argument;

        if (!command_line)
                return NULL;

        argument = strstr (command_line, prefix);

        if (argument == NULL)
                return NULL;

        if (argument == command_line ||
            argument[-1] == ' ')
                return argument + strlen (prefix);

        return NULL;
}

bool
ply_kernel_command_line_has_argument (const char *argument)
{
        const char *string;

        string = ply_kernel_command_line_get_string_after_prefix (argument);

        if (string == NULL)
                return false;

        if (!isspace ((int) string[0]) && string[0] != '\0')
                return false;

        return true;
}

char *
ply_kernel_command_line_get_key_value (const char *key)
{
        const char *value;

        value = ply_kernel_command_line_get_string_after_prefix (key);
        if (value == NULL || value[0] == '\0')
                return NULL;

        return strndup(value, strcspn (value, " \n"));
}

void
ply_kernel_command_line_override (const char *command_line)
{
        strncpy (kernel_command_line, command_line, sizeof(kernel_command_line));
        kernel_command_line[sizeof(kernel_command_line) - 1] = '\0';
        kernel_command_line_is_set = true;
}

double ply_strtod(const char *str)
{
        char *old_locale;
        double ret;

        /* Ensure strtod uses '.' as decimal separator, as we use this in our cfg files. */
        old_locale = setlocale(LC_NUMERIC, "C");
        ret = strtod(str, NULL);
        setlocale(LC_NUMERIC, old_locale);

        return ret;
}

/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
