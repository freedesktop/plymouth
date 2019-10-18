/* ply-utils.h - random useful functions and macros
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
 * Written By: Ray Strode <rstrode@redhat.com>
 */
#ifndef PLY_UTILS_H
#define PLY_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>

#ifndef MIN
#define MIN(a, b) ((a) <= (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) >= (b) ? (a) : (b))
#endif

#ifndef CLAMP
#define CLAMP(a, b, c) (MIN (MAX ((a), (b)), (c)))
#endif

#define PLY_UTF8_CHARACTER_SIZE_MAX 4

typedef intptr_t ply_module_handle_t;
typedef void (*ply_module_function_t) (void);

typedef intptr_t ply_daemon_handle_t;

typedef enum
{
        PLY_UNIX_SOCKET_TYPE_CONCRETE = 0,
        PLY_UNIX_SOCKET_TYPE_ABSTRACT,
        PLY_UNIX_SOCKET_TYPE_TRIMMED_ABSTRACT
} ply_unix_socket_type_t;

#ifndef PLY_HIDE_FUNCTION_DECLARATIONS

#define ply_round_to_multiple(n, m) (((n) + (((m) - 1))) & ~((m) - 1))

bool ply_open_unidirectional_pipe (int *sender_fd,
                                   int *receiver_fd);
int ply_connect_to_unix_socket (const char            *path,
                                ply_unix_socket_type_t type);
int ply_listen_to_unix_socket (const char            *path,
                               ply_unix_socket_type_t type);
bool ply_get_credentials_from_fd (int    fd,
                                  pid_t *pid,
                                  uid_t *uid,
                                  gid_t *gid);

bool ply_write (int         fd,
                const void *buffer,
                size_t      number_of_bytes);
bool ply_write_uint32 (int      fd,
                       uint32_t value);
bool ply_read (int    fd,
               void  *buffer,
               size_t number_of_bytes);
bool ply_read_uint32 (int       fd,
                      uint32_t *value);

bool ply_fd_has_data (int fd);
bool ply_set_fd_as_blocking (int fd);
char **ply_copy_string_array (const char *const *array);
void ply_free_string_array (char **array);
double ply_get_timestamp (void);

void ply_save_errno (void);
void ply_restore_errno (void);

bool ply_directory_exists (const char *dir);
bool ply_file_exists (const char *file);
bool ply_character_device_exists (const char *device);

ply_module_handle_t *ply_open_module (const char *module_path);
ply_module_handle_t *ply_open_built_in_module (void);

ply_module_function_t ply_module_look_up_function (ply_module_handle_t *handle,
                                                   const char          *function_name);
void ply_close_module (ply_module_handle_t *handle);

bool ply_create_directory (const char *directory);
bool ply_create_file_link (const char *source,
                           const char *destination);
void ply_show_new_kernel_messages (bool should_show);

ply_daemon_handle_t *ply_create_daemon (void);
bool ply_detach_daemon (ply_daemon_handle_t *handle,
                        int                  exit_code);

int ply_utf8_character_get_size (const char *string,
                                 size_t      n);
int ply_utf8_string_get_length (const char *string,
                                size_t      n);

char *ply_get_process_command_line (pid_t pid);
pid_t ply_get_process_parent_pid (pid_t pid);

void ply_set_device_scale (int device_scale);
void ply_set_device_orientation (int device_orientation);

int ply_get_device_scale (uint32_t width,
                          uint32_t height,
                          uint32_t width_mm,
                          uint32_t height_mm);
int ply_get_device_orientation (void);

const char *ply_kernel_command_line_get_string_after_prefix (const char *prefix);
bool ply_kernel_command_line_has_argument (const char *argument);
void ply_kernel_command_line_override (const char *command_line);
char *ply_kernel_command_line_get_key_value (const char *key);

double ply_strtod(const char *str);

#endif

#endif /* PLY_UTILS_H */
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
