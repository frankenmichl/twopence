/*
Test executor, virtio and serial plugins
(the ones that use a custom protocol to communicate with the remote host).


Copyright (C) 2014 SUSE

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 2.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include <sys/stat.h>
#include <sys/poll.h>
#include <stdio.h>                     // For snprintf() parsing facility. Most I/O is low-level and unbuffered.
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "twopence.h"
#include "protocol.h"

#define BUFFER_SIZE 32768              // Size in bytes of the work buffer for receiving data from the remote
#define COMMAND_BUFFER_SIZE 8192       // Size in bytes of the work buffer for sending data to the remote

/*
 * Class initialization
 */
void
twopence_pipe_target_init(struct twopence_pipe_target *target, int plugin_type, const struct twopence_plugin *plugin_ops,
			const struct twopence_pipe_ops *link_ops)
{
  memset(target, 0, sizeof(*target));

  target->base.plugin_type = plugin_type;
  target->base.ops = plugin_ops;
  target->link_timeout = 60000; /* 1 minute */
  target->link_ops = link_ops;

  twopence_sink_init_none(&target->base.current.sink);
}

///////////////////////////// Lower layer ///////////////////////////////////////

// Store length of data chunk to send
static void
store_length(unsigned int length, char *buffer)
{
  buffer[2] = (length & 0xFF00) >> 8;
  buffer[3] = length & 0xFF;
}

// Compute length of data chunk received
static inline unsigned int
compute_length(const void *data)
{
  const unsigned char *cp = (const unsigned char *) data;

  return (cp[2] << 8) | cp[3];
}

// Output a "stdout" character through one of the available methods
//
// Returns 0 if everything went fine, -1 otherwise
static int
__twopence_pipe_output(struct twopence_pipe_target *handle, char c)
{
  return __twopence_sink_write_stdout(&handle->base.current.sink, c);
}

// Output a "stderr" character through one of the available methods
//
// Returns 0 if everything went fine, -1 otherwise
static int
__twopence_pipe_error(struct twopence_pipe_target *handle, char c)
{
  return __twopence_sink_write_stderr(&handle->base.current.sink, c);
}

// Check for invalid usernames
static bool
_twopence_invalid_username(const char *username)
{
  const char *p;

  for (p = username; *p; p++)
  {
    if ('0' <= *p && *p <= '9') continue;
    if ('A' <= *p && *p <= 'Z') continue;
    if ('a' <= *p && *p <= 'z') continue;
    if (*p == '_') continue;
    return true;
  }

  return false;
}

/*
 * Wrap the link functions
 */
static int
__twopence_pipe_open_link(struct twopence_pipe_target *handle)
{
  return handle->link_ops->open(handle);
}

static inline int
__twopence_pipe_poll(int link_fd, int events, unsigned long timeout)
{
  struct pollfd pfd;
  int n;

  /* It's not quite clear why we're not just using blocking input here. */
  pfd.fd = link_fd;
  pfd.events = events;

  n = poll(&pfd, 1, timeout);
  if ((n == 1) && !(pfd.revents & events))
    n = 0;

  return n;
}

static int
__twopence_pipe_recvbuf(struct twopence_pipe_target *handle, int link_fd, char *buffer, size_t size)
{
  size_t received = 0;

  while (received < size) {
    int n, rc;

    /* It's not quite clear why we're not just using blocking input here. */
    n = __twopence_pipe_poll(link_fd, POLLIN, handle->link_timeout);
    if (n < 0) {
      perror("poll error");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    if (n == 0) {
      fprintf(stderr, "timeout on link");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    /* Read some data from the link */
    rc = handle->link_ops->recv(handle, link_fd, buffer + received, size - received);
    if (rc < 0)
      return rc;

    if (rc == 0) {
      fprintf(stderr, "unexpected EOF on link");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    received += rc;
  }

  return received;
}

static int
__twopence_pipe_sendbuf(struct twopence_pipe_target *handle, int link_fd, const char *buffer, size_t count)
{
  size_t sent = 0;

  while (sent < count) {
    int n, rc;

    /* It's not quite clear why we're not just using blocking input here. */
    n = __twopence_pipe_poll(link_fd, POLLOUT, handle->link_timeout);
    if (n < 0) {
      perror("poll error");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    if (n == 0) {
      fprintf(stderr, "timeout on link");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    rc = handle->link_ops->send(handle, link_fd, buffer + sent, count - sent);
    if (rc < 0)
      return rc;

    sent += rc;
  }

  return sent;
}

/*
 * Read a chunk (normally called a packet or frame) from the link
 */
static int
__twopence_pipe_read_frame(struct twopence_pipe_target *handle, int link_fd, char *buffer, size_t size)
{
  int rc, length;

  /* First try to read the header */
  rc = __twopence_pipe_recvbuf(handle, link_fd, buffer, 4);
  if (rc < 0)
    return rc;

  length = compute_length(buffer);     // Decode the announced amount of data
  if (length > size)
    return TWOPENCE_PROTOCOL_ERROR;

  /* SECURITY: prevent buffer overflow */
  if (length < 4)
    return TWOPENCE_PROTOCOL_ERROR;

  /* Read the announced amount of data */
  rc = __twopence_pipe_recvbuf(handle, link_fd, buffer + 4, length - 4);
  if (rc < 0)
    return rc;

  return 0;
}


/*
 * Helper function to read from either link or stdin
 */
static int
__twopence_pipe_recvbuf_both(struct twopence_pipe_target *handle, int link_fd, int stdin_fd, char *buffer, size_t size)
{
  unsigned long timeout = 60000; /* 1 minute */

  while (true) {
    struct pollfd pfd[2];
    int nfds = 0, n;

    pfd[nfds].fd = link_fd;
    pfd[nfds].events = POLLIN;
    nfds++;

    if (stdin_fd >= 0) {
      pfd[nfds].fd = stdin_fd;
      pfd[nfds].events = POLLIN;
      nfds++;
    }

    n = poll(pfd, nfds, timeout);
    if (n < 0) {
      if (errno == EINTR)
	continue;
      perror("poll");
      return TWOPENCE_PROTOCOL_ERROR;
    }
    if (n == 0) {
      fprintf(stderr, "recv timeout on link\n");
      return TWOPENCE_PROTOCOL_ERROR;
    }

    if (pfd[0].revents & POLLIN) {
      /* Incoming data on the link. Read the complete frame right away (blocking until we have it) */
      return __twopence_pipe_read_frame(handle, link_fd, buffer, size);
    }

    if (nfds > 1 && (pfd[0].revents & POLLIN)) {
      int count;

      count = read(stdin_fd, buffer + 4, size - 4);
      if (count < 0) {
	if (errno == EINTR)
	  continue;
	return count;
      }

      if (count == 0) {
        buffer[0] = 'E'; /* EOF on standard input */
      } else {
	buffer[0] = '0'; /* Data on standard input */
      }

      store_length(count + 4, buffer);
      return count + 4;
    }

    /* Can we get here? */
  }

  return 0;
}

///////////////////////////// Middle layer //////////////////////////////////////
// Read stdin, stdout, stderr, and both error codes
//
// Returns 0 if everything went fine, or a negative error code if failed
int
_twopence_read_results(struct twopence_pipe_target *handle, int link_fd, int *major, int *minor)
{
  int state;                           // 0 = processing results, 1 = major received, 2 = minor received
  int stdin_fd;
  char buffer[BUFFER_SIZE];
  int rc, received, sent;
  const char *p;

  stdin_fd = 0; /* Initially, we will try to read from stdin */
  state = 0;

  while (state != 2)
  {
    rc = __twopence_pipe_recvbuf_both(handle, link_fd, stdin_fd, buffer, sizeof(buffer));
    if (rc != 0)
      return TWOPENCE_RECEIVE_RESULTS_ERROR;

    received = compute_length(buffer);
    switch (buffer[0]) {
      case 'E':                        // End of file on stdin
	stdin_fd = -1;
	/* fallthru */
      case '0':                        // Data on stdin
        if (state != 0)
          return TWOPENCE_FORWARD_INPUT_ERROR;
	// Forward it to the system under test
        sent = __twopence_pipe_sendbuf(handle, link_fd, buffer, received);
        if (sent < 0)
          return TWOPENCE_FORWARD_INPUT_ERROR;
        break;

      case '1':                        // stdout
        if (state != 0)
          return TWOPENCE_RECEIVE_RESULTS_ERROR;
        for (p = buffer + 4; received > 4; received--)
        {                              // Output it
          if (__twopence_pipe_output(handle, *p++) < 0)
            return TWOPENCE_RECEIVE_RESULTS_ERROR;
        }
        break;

      case '2':                        // stderr
        if (state != 0)
          return TWOPENCE_RECEIVE_RESULTS_ERROR;
        for (p = buffer + 4; received > 4; received--)
        {                              // Output it
          if (__twopence_pipe_error(handle, *p++) < 0)
            return TWOPENCE_RECEIVE_RESULTS_ERROR;
        }
        break;

      case 'M':                        // Major error code
        if (state != 0)
          return TWOPENCE_RECEIVE_RESULTS_ERROR;
        state = 1;
        sscanf(buffer + 4, "%d", major);
        break;

      case 'm':                        // Minor error code
        if (state != 1)
          return TWOPENCE_RECEIVE_RESULTS_ERROR;
        state = 2;
        sscanf(buffer + 4, "%d", minor);
        break;

      default:
        return TWOPENCE_RECEIVE_RESULTS_ERROR;
    }
  }

  return 0;
}

// Read major error code
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
_twopence_read_major(struct twopence_pipe_target *handle, int link_fd, int *major)
{
  char buffer[BUFFER_SIZE];
  int rc;

  // Receive a chunk of data
  rc = __twopence_pipe_read_frame(handle, link_fd, buffer, sizeof(buffer));
  if (rc != 0)
    return TWOPENCE_RECEIVE_FILE_ERROR;

  if (buffer[0] != 'M')                // Analyze the header
    return TWOPENCE_RECEIVE_FILE_ERROR;
  sscanf(buffer + 4, "%d", major);

  return 0;
}

// Read minor error code
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
_twopence_read_minor(struct twopence_pipe_target *handle, int link_fd, int *minor)
{
  char buffer[BUFFER_SIZE];
  int rc;

  // Receive a chunk of data
  rc = __twopence_pipe_read_frame(handle, link_fd, buffer, sizeof(buffer));
  if (rc != 0)
    return TWOPENCE_RECEIVE_FILE_ERROR;

  if (buffer[0] != 'm')                // Analyze the header
    return TWOPENCE_RECEIVE_FILE_ERROR;
  sscanf(buffer + 4, "%d", minor);

  return 0;
}

// Read file size
// It can also get a remote error code if, for example, the remote file does not exist
//
// Returns 0 if everything went fine, or a negative error code if failed
static int
_twopence_read_size(struct twopence_pipe_target *handle, int link_fd, int *size, int *remote_rc)
{
  char buffer[BUFFER_SIZE];
  int rc;

  rc = __twopence_pipe_read_frame(handle, link_fd, buffer, sizeof(buffer));
  if (rc != 0)
    return TWOPENCE_RECEIVE_FILE_ERROR;

  switch (buffer[0])                   // Analyze the header
  {
    case 's':
      sscanf(buffer + 4, "%d", size);
      break;
    case 'M':
      sscanf(buffer + 4, "%d", remote_rc);
      break;
    default:
      return TWOPENCE_RECEIVE_FILE_ERROR;
  }

  return 0;
}

// Send a file in chunks to the link
//
// Returns 0 if everything went fine, or a negative error code if failed
int _twopence_send_file
  (struct twopence_pipe_target *handle, int file_fd, int link_fd, int remaining)
{
  char buffer[BUFFER_SIZE];
  int size, received;

  while (remaining > 0)
  {
    size =                             // Read at most BUFFER_SIZE - 4 bytes from the file
           remaining < BUFFER_SIZE - 4?
           remaining:
           BUFFER_SIZE - 4;
    received = read(file_fd, buffer + 4, size);
    if (received != size)
    {
      __twopence_pipe_output(handle, '\n');
      return TWOPENCE_LOCAL_FILE_ERROR;
    }

    buffer[0] = 'd';                   // Send them to the remote host, together with 4 bytes of header
    store_length(received + 4, buffer);
    if (!__twopence_pipe_sendbuf(handle, link_fd, buffer, received + 4))
    {
      __twopence_pipe_output(handle, '\n');
      return TWOPENCE_SEND_FILE_ERROR;
    }

    __twopence_pipe_output(handle, '.');     // Progression dots
    remaining -= received;             // One chunk less to send
  }
  __twopence_pipe_output(handle, '\n');
  return 0;
}

// Receive a file in chunks from the link and write it to a file
//
// Returns 0 if everything went fine, or a negative error code if failed
int _twopence_receive_file
  (struct twopence_pipe_target *handle, int file_fd, int link_fd, int remaining)
{
  char buffer[BUFFER_SIZE];
  int rc, received, written;

  while (remaining > 0)
  {
    rc = __twopence_pipe_read_frame(handle, link_fd, buffer, sizeof(buffer));
    if (rc != 0)
    {
      __twopence_pipe_output(handle, '\n');
      return TWOPENCE_RECEIVE_FILE_ERROR;
    }

    received = compute_length(buffer) - 4;
    if (buffer[0] != 'd' || received < 0 || received > remaining)
    {
      __twopence_pipe_output(handle, '\n');
      return TWOPENCE_RECEIVE_FILE_ERROR;
    }

    if (received > 0)
    {
      written = write                  // Write the data to the file
        (file_fd, buffer + 4, received);
      if (written != received)
      {
        __twopence_pipe_output(handle, '\n');
        return TWOPENCE_LOCAL_FILE_ERROR;
      }
      __twopence_pipe_output(handle, '.');   // Progression dots
      remaining -= received;           // One chunk less to write
    }
  }
  __twopence_pipe_output(handle, '\n');
  return 0;
}

///////////////////////////// Top layer /////////////////////////////////////////

// Send a Linux command to the remote host
//
// Returns 0 if everything went fine, or a negative error code if failed
int
__twopence_pipe_command(struct twopence_pipe_target *handle, const char *username, const char *linux_command, int *major, int *minor)
{
  char command[COMMAND_BUFFER_SIZE];
  int n;
  int link_fd;
  int sent, rc;

  // By default, no major and no minor
  *major = 0;
  *minor = 0;

  // Check that the username is valid
  if (_twopence_invalid_username(username))
    return TWOPENCE_PARAMETER_ERROR;

  // Refuse to execute empty commands
  if (*linux_command == '\0')
    return TWOPENCE_PARAMETER_ERROR;

  // Prepare command to send to the remote host
  n = snprintf(command, COMMAND_BUFFER_SIZE,
               "c...%s %s", username, linux_command);
  if (n < 0 || n >= COMMAND_BUFFER_SIZE)
    return TWOPENCE_PARAMETER_ERROR;
  store_length(n + 1, command);

  // Tune stdin so it is nonblocking
  if (twopence_tune_stdin(false) < 0)
    return TWOPENCE_OPEN_SESSION_ERROR;

  // Open communication link
  link_fd = __twopence_pipe_open_link(handle);
  if (link_fd < 0)
  {
    twopence_tune_stdin(true);
    return TWOPENCE_OPEN_SESSION_ERROR;
  }

  // Send command (including terminating NUL)
  sent = __twopence_pipe_sendbuf(handle, link_fd, command, n + 1);
  if (sent != n + 1)
  {
    twopence_tune_stdin(true);
    close(link_fd);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  // Read "standard output" and "standard error"
  rc = _twopence_read_results(handle, link_fd, major, minor);
  if (rc < 0)
  {
    twopence_tune_stdin(true);
    close(link_fd);
    return TWOPENCE_RECEIVE_RESULTS_ERROR;
  }

  twopence_tune_stdin(true);
  close(link_fd);
  return 0;
}

// Inject a file into the remote host
//
// Returns 0 if everything went fine
int _twopence_inject_virtio_serial
  (struct twopence_pipe_target *handle, const char *username, int file_fd, const char *remote_filename, int *remote_rc)
{
  char command[COMMAND_BUFFER_SIZE];
  int n;
  int link_fd;
  int sent, rc;
  struct stat filestats;

  // By default, no remote error
  *remote_rc = 0;

  // Check that the username is valid
  if (_twopence_invalid_username(username))
    return TWOPENCE_PARAMETER_ERROR;

  // Prepare command to send to the remote host
  fstat(file_fd, &filestats);
  n = snprintf(command, COMMAND_BUFFER_SIZE,
               "i...%s %ld %s", username, (long) filestats.st_size, remote_filename);
  if (n < 0 || n >= COMMAND_BUFFER_SIZE)
    return TWOPENCE_PARAMETER_ERROR;
  store_length(n + 1, command);

  // Open communication link
  link_fd = __twopence_pipe_open_link(handle);
  if (link_fd < 0)
    return TWOPENCE_OPEN_SESSION_ERROR;

  // Send command (including terminating NUL)
  sent = __twopence_pipe_sendbuf(handle, link_fd, command, n + 1);
  if (sent != n + 1)
  {
    close(link_fd);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  // Read first return code before we start transferring the file
  // This enables to detect a remote problem even before we start the transfer
  rc = _twopence_read_major(handle, link_fd, remote_rc);
  if (*remote_rc != 0)
  {
    close(link_fd);
    return TWOPENCE_SEND_FILE_ERROR;
  }

  // Send the file
  rc = _twopence_send_file(handle, file_fd, link_fd, filestats.st_size);
  if (rc < 0)
  {
    close(link_fd);
    return TWOPENCE_SEND_FILE_ERROR;
  }

  // Read second return code from remote
  rc = _twopence_read_minor(handle, link_fd, remote_rc);
  if (rc < 0)
  {
    close(link_fd);
    return TWOPENCE_SEND_FILE_ERROR;
  }

  close(link_fd);
  return 0;
}

// Extract a file from the remote host
//
// Returns 0 if everything went fine, or a negative error code if failed
int _twopence_extract_virtio_serial
  (struct twopence_pipe_target *handle, const char *username, int file_fd, const char *remote_filename, int *remote_rc)
{
  char command[COMMAND_BUFFER_SIZE];
  int n;
  int link_fd;
  int sent, rc;
  int size;

  // By default, no remote error
  *remote_rc = 0;

  // Check that the username is valid
  if (_twopence_invalid_username(username))
    return TWOPENCE_PARAMETER_ERROR;

  // Prepare command to send to the remote host
  n = snprintf(command, COMMAND_BUFFER_SIZE,
               "e...%s %s", username, remote_filename);
  if (n < 0 || n >= COMMAND_BUFFER_SIZE)
    return TWOPENCE_PARAMETER_ERROR;
  store_length(n + 1, command);

  // Open link for transmitting the command
  link_fd = __twopence_pipe_open_link(handle);
  if (link_fd < 0)
    return TWOPENCE_OPEN_SESSION_ERROR;

  // Send command (including terminating NUL)
  sent = __twopence_pipe_sendbuf(handle, link_fd, command, n + 1);
  if (sent != n + 1)
  {
    close(link_fd);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  // Read the size of the file to receive
  rc = _twopence_read_size(handle, link_fd, &size, remote_rc);
  if (rc < 0)
  {
    close(link_fd);
    return TWOPENCE_RECEIVE_FILE_ERROR;
  }

  // Receive the file
  if (size >= 0)
  {
    rc = _twopence_receive_file(handle, file_fd, link_fd, size);
    if (rc < 0)
      return TWOPENCE_RECEIVE_FILE_ERROR;
  }

  close(link_fd);
  return 0;
}

// Tell the remote test server to exit
//
// Returns 0 if everything went fine, or a negative error code if failed
int _twopence_exit_virtio_serial
  (struct twopence_pipe_target *handle)
{
  char command[COMMAND_BUFFER_SIZE];
  int n;
  int link_fd;
  int sent;

  // Prepare command to send to the remote host
  n = snprintf(command, COMMAND_BUFFER_SIZE,
               "q...");
  if (n < 0 || n >= COMMAND_BUFFER_SIZE)
    return TWOPENCE_PARAMETER_ERROR;
  store_length(n + 1, command);

  // Open link for sending exit command
  link_fd = __twopence_pipe_open_link(handle);
  if (link_fd < 0)
    return TWOPENCE_OPEN_SESSION_ERROR;

  // Send command (including terminating NUL)
  sent = __twopence_pipe_sendbuf(handle, link_fd, command, n + 1);
  if (sent != n + 1)
  {
    close(link_fd);
    return TWOPENCE_SEND_COMMAND_ERROR;
  }

  close(link_fd);
  return 0;
}

// Interrupt current command
//
// Returns 0 if everything went fine, or a negative error code if failed
int _twopence_interrupt_virtio_serial
  (struct twopence_pipe_target *handle)
{
  char command[COMMAND_BUFFER_SIZE];
  int n;
  int link_fd;
  int sent;

  // Prepare command to send to the remote host
  n = snprintf(command, COMMAND_BUFFER_SIZE,
               "I...");
  if (n < 0 || n >= COMMAND_BUFFER_SIZE)
    return TWOPENCE_PARAMETER_ERROR;
  store_length(n + 1, command);

  // Open link for sending interrupt command
  link_fd = __twopence_pipe_open_link(handle);
  if (link_fd < 0)
    return TWOPENCE_OPEN_SESSION_ERROR;

  // Send command (including terminating NUL)
  sent = __twopence_pipe_sendbuf(handle, link_fd, command, n + 1);
  if (sent != n + 1)
  {
    close(link_fd);
    return TWOPENCE_INTERRUPT_COMMAND_ERROR;
  }

  close(link_fd);
  return 0;
}

///////////////////////////// Public interface //////////////////////////////////

// Run a test command, and print output
//
// Returns 0 if everything went fine
// 'major' is the return code of the test server
// 'minor' is the return code of the command
int
twopence_pipe_test_and_print_results(struct twopence_target *opaque_handle,
		const char *username, const char *command,
		int *major, int *minor)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;

  twopence_sink_init(&handle->base.current.sink, TWOPENCE_OUTPUT_SCREEN, NULL, NULL, 0);
  return __twopence_pipe_command
           (handle, username, command, major, minor);
}

// Run a test command, and drop output
//
// Returns 0 if everything went fine
// 'major' is the return code of the test server
// 'minor' is the return code of the command
int
twopence_pipe_test_and_drop_results(struct twopence_target *opaque_handle,
		const char *username, const char *command,
		int *major, int *minor)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;

  twopence_sink_init_none(&handle->base.current.sink);
  return __twopence_pipe_command
           (handle, username, command, major, minor);
}

// Run a test command, and store the results in memory in a common buffer
//
// Returns 0 if everything went fine
// 'major' is the return code of the test server
// 'minor' is the return code of the command
int
twopence_pipe_test_and_store_results_together(struct twopence_target *opaque_handle,
		const char *username, const char *command,
		char *buffer_out, int size,
		int *major, int *minor)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;
  int rc;

  twopence_sink_init(&handle->base.current.sink, TWOPENCE_OUTPUT_BUFFER, buffer_out, NULL, size);
  rc = __twopence_pipe_command
         (handle, username, command, major, minor);

  // Store final NUL
  if (rc == 0) {
    if (__twopence_pipe_output(handle, '\0') < 0)
      rc = TWOPENCE_RECEIVE_RESULTS_ERROR;
  }
  return rc;
}

// Run a test command, and store the results in memory in two separate buffers
//
// Returns 0 if everything went fine
// 'major' is the return code of the test server
// 'minor' is the return code of the command
int
twopence_pipe_test_and_store_results_separately(struct twopence_target *opaque_handle,
		const char *username, const char *command,
		char *buffer_out, char *buffer_err, int size,
		int *major, int *minor)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;
  int rc;

  twopence_sink_init(&handle->base.current.sink, TWOPENCE_OUTPUT_BUFFER_SEPARATELY, buffer_out, buffer_err, size);
  rc = __twopence_pipe_command
         (handle, username, command, major, minor);

  // Store final NULs
  if (rc == 0) {
    if (__twopence_pipe_output(handle, '\0') < 0
     || __twopence_pipe_error(handle, '\0') < 0)
      rc = TWOPENCE_RECEIVE_RESULTS_ERROR;
  }
  return rc;
}

// Inject a file into the Virtual Machine
//
// Returns 0 if everything went fine
int
twopence_pipe_inject_file(struct twopence_target *opaque_handle,
		const char *username,
		const char *local_filename, const char *remote_filename,
		int *remote_rc, bool dots)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;
  int fd, rc;

  twopence_sink_init(&handle->base.current.sink, dots? TWOPENCE_OUTPUT_SCREEN : TWOPENCE_OUTPUT_NONE, NULL, NULL, 0);

  // Open the file
  fd = open(local_filename, O_RDONLY);
  if (fd == -1)
    return errno == ENAMETOOLONG?
           TWOPENCE_PARAMETER_ERROR:
           TWOPENCE_LOCAL_FILE_ERROR;

  // Inject it
  rc = _twopence_inject_virtio_serial
         (handle, username, fd, remote_filename, remote_rc);
  if (rc == 0 && *remote_rc != 0)
    rc = TWOPENCE_REMOTE_FILE_ERROR;

  // Close it
  close(fd);
  return rc;
}

// Extract a file from the Virtual Machine
//
// Returns 0 if everything went fine
int
twopence_pipe_extract_file(struct twopence_target *opaque_handle,
		const char *username,
		const char *remote_filename, const char *local_filename,
		int *remote_rc, bool dots)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;
  int fd, rc;

  twopence_sink_init(&handle->base.current.sink, dots? TWOPENCE_OUTPUT_SCREEN : TWOPENCE_OUTPUT_NONE, NULL, NULL, 0);

  // Open the file, creating it if it does not exist (u=rw,g=rw,o=)
  fd = creat(local_filename, 00660);
  if (fd == -1)
    return errno == ENAMETOOLONG?
           TWOPENCE_PARAMETER_ERROR:
           TWOPENCE_LOCAL_FILE_ERROR;

  // Extract it
  rc = _twopence_extract_virtio_serial
         (handle, username, fd, remote_filename, remote_rc);
  if (rc == 0 && *remote_rc != 0)
    rc = TWOPENCE_REMOTE_FILE_ERROR;

  // Close it
  close(fd);
  return rc;
}

// Interrupt current command
//
// Returns 0 if everything went fine
int
twopence_pipe_interrupt_command(struct twopence_target *opaque_handle)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;

  return _twopence_interrupt_virtio_serial(handle);
}

// Tell the remote test server to exit
//
// Returns 0 if everything went fine
int
twopence_pipe_exit_remote(struct twopence_target *opaque_handle)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;

  twopence_sink_init_none(&handle->base.current.sink);

  return _twopence_exit_virtio_serial(handle);
}

// Close the library
void
twopence_pipe_end(struct twopence_target *opaque_handle)
{
  struct twopence_pipe_target *handle = (struct twopence_pipe_target *) opaque_handle;

  free(handle);
}
