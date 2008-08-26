/* GStreamer
 * Copyright (C) <2007> Leandro Melo de Sales <leandroal@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include "gstdccp.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <string.h>

/* Prototypes and definitions for private functions and not exported via gstdccp.h */
gint gst_dccp_socket_write (int socket, const void *buf, size_t count,
    int packet_size);
gboolean gst_dccp_socket_connected (GstElement * element, int server_sock_fd);
struct sockaddr_in gst_dccp_create_sockaddr (GstElement * element, gchar * ip,
    int port);

/* Resolves host to IP address
 * @return a gchar pointer containing the ip address or NULL
 */
gchar *
gst_dccp_host_to_ip (GstElement * element, const gchar * host)
{
  struct hostent *hostinfo;
  char **addrs;
  gchar *ip;
  struct in_addr addr;

  GST_DEBUG_OBJECT (element, "resolving host %s", host);

  /* first check if it already is an IP address */
  if (inet_aton (host, &addr)) {
    ip = g_strdup (host);
    GST_DEBUG_OBJECT (element, "resolved to IP %s", ip);
    return ip;
  }

  /* perform a name lookup */
  if (!(hostinfo = gethostbyname (host))) {
    GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND, (NULL),
        ("Could not find IP address for host \"%s\".", host));
    return NULL;
  }

  if (hostinfo->h_addrtype != AF_INET) {
    GST_ELEMENT_ERROR (element, RESOURCE, NOT_FOUND, (NULL),
        ("host \"%s\" is not an IP host", host));
    return NULL;
  }

  addrs = hostinfo->h_addr_list;

  /* There could be more than one IP address, but we just return the first */
  ip = g_strdup (inet_ntoa (*(struct in_addr *) *addrs));

  return ip;
}

/* Read a buffer from the given socket
 * @returns:
 *   a GstBuffer from which data should be read
 *   or NULL, indicating a connection close or an error. Handle it with EOS.
 */
GstFlowReturn
gst_dccp_read_buffer (GstElement * this, int socket, GstBuffer ** buf)
{
  fd_set testfds;
  int maxfdp1;
  int ret;
  ssize_t bytes_read;
  int readsize;

  *buf = NULL;

  /* do a blocking select on the socket */
  FD_ZERO (&testfds);
  FD_SET (socket, &testfds);
  maxfdp1 = socket + 1;

  /* no action (0) is also an error in our case */
  if ((ret = select (maxfdp1, &testfds, NULL, NULL, 0)) <= 0) {
    GST_ELEMENT_ERROR (this, RESOURCE, READ, (NULL),
        ("select failed: %s", g_strerror (errno)));
    return GST_FLOW_ERROR;
  }

  /* ask how much is available for reading on the socket */
  if ((ret = ioctl (socket, FIONREAD, &readsize)) < 0) {
    GST_ELEMENT_ERROR (this, RESOURCE, READ, (NULL),
        ("read FIONREAD value failed: %s", g_strerror (errno)));
    return GST_FLOW_ERROR;
  }

  if (readsize == 0) {
    GST_DEBUG_OBJECT (this, "Got EOS on socket stream");
    return GST_FLOW_UNEXPECTED;
  }

  *buf = gst_buffer_new_and_alloc (readsize);
  bytes_read = read (socket, GST_BUFFER_DATA (*buf), readsize);

  GST_LOG_OBJECT (this, "bytes read %" G_GSIZE_FORMAT, bytes_read);
  GST_LOG_OBJECT (this, "returning buffer of size %d", GST_BUFFER_SIZE (*buf));

  return GST_FLOW_OK;
}

/* Create a new socket
 * @return the socket file descriptor
 */
gint
gst_dccp_create_new_socket (GstElement * element)
{
  int sock_fd;
  if ((sock_fd = socket (AF_INET, SOCK_DCCP, IPPROTO_DCCP)) < 0) {
    GST_ELEMENT_ERROR (element, RESOURCE, OPEN_READ, (NULL), GST_ERROR_SYSTEM);
  }

  return sock_fd;
}

/* Connect to a server
 * @return true in case of successfull connection, false otherwise
 */
gboolean
gst_dccp_connect_to_server (GstElement * element, struct sockaddr_in server_sin,
    int sock_fd)
{
  GST_DEBUG_OBJECT (element, "connecting to server");

  if (connect (sock_fd, (struct sockaddr *) &server_sin, sizeof (server_sin))) {
    switch (errno) {
      case ECONNREFUSED:
        GST_ERROR_OBJECT (element, "Connection refused.");
        return FALSE;
        break;
      default:
        GST_ERROR_OBJECT (element, "Connection failed.");
        return FALSE;
        break;
    }
  }
  return TRUE;
}

/* FIXME support only one client */
/*
 * Accept connection on the server socket.
 * @return the socket of the client connected to the server.
 */
gint
gst_dccp_server_wait_connections (GstElement * element, int server_sock_fd)
{
  /* new client */
  int client_sock_fd;
  struct sockaddr_in client_address;
  unsigned int client_address_len;

  /* For some stupid reason, client_address and client_address_len has to be
   * zeroed */
  memset (&client_address, 0, sizeof (client_address));
  client_address_len = 0;

  if ((client_sock_fd =
          accept (server_sock_fd, (struct sockaddr *) &client_address,
              &client_address_len)) == -1) {
    return -1;
  }
  /* to support multiple connection, fork here a new thread passing the
   * client_sock_fd returned by accept function.
   */

  GST_DEBUG_OBJECT (element, "added new client ip %s with fd %d",
      inet_ntoa (client_address.sin_addr), client_sock_fd);

  /* return the thread object, instead of the fd */
  return client_sock_fd;
}

/*
 * Bind a server address.
 * @return true in success, false otherwise.
 */
gboolean
gst_dccp_bind_server_socket (GstElement * element, int server_sock_fd,
    struct sockaddr_in server_sin)
{
  int ret;

  GST_DEBUG_OBJECT (element, "binding server socket to address");
  ret = bind (server_sock_fd, (struct sockaddr *) &server_sin,
      sizeof (server_sin));
  if (ret) {
    switch (errno) {
      default:
        GST_ELEMENT_ERROR (element, RESOURCE, OPEN_READ, (NULL),
            ("bind on port %d failed: %s", server_sin.sin_port,
                g_strerror (errno)));
        return FALSE;
        break;
    }
  }
  return TRUE;
}

gboolean
gst_dccp_listen_server_socket (GstElement * element, int server_sock_fd)
{

  GST_DEBUG_OBJECT (element, "listening on server socket %d with queue of %d",
      server_sock_fd, DCCP_BACKLOG);
  if (listen (server_sock_fd, DCCP_BACKLOG) == -1) {
    GST_ELEMENT_ERROR (element, RESOURCE, OPEN_READ, (NULL),
        ("Could not listen on server socket: %s", g_strerror (errno)));
    return FALSE;
  }
  GST_DEBUG_OBJECT (element,
      "listened on server socket %d, returning from connection setup",
      server_sock_fd);

  return TRUE;
}

/* FIXME */
gboolean
gst_dccp_socket_connected (GstElement * element, int server_sock_fd)
{
  return FALSE;
}

/* Write buffer to given socket incrementally.
 * Returns number of bytes written.
 */
gint
gst_dccp_socket_write (int socket, const void *buf, size_t size,
    int packet_size)
{
  size_t bytes_written = 0;
  ssize_t wrote;

  while (bytes_written < size) {
    do {
      wrote = write (socket, (const char *) buf + bytes_written,
          MIN (packet_size, size - bytes_written));
    } while (wrote == -1 && errno == EAGAIN);

    /* TODO print the send error */
    bytes_written += wrote;
  }

  if (bytes_written < 0)
    GST_WARNING ("error while writing");
  else
    GST_LOG ("wrote %" G_GSIZE_FORMAT " bytes succesfully", bytes_written);
  return bytes_written;
}

GstFlowReturn
gst_dccp_send_buffer (GstElement * this, GstBuffer * buffer, int client_sock_fd,
    int packet_size)
{
  size_t wrote;
  gint size = 0;
  guint8 *data;

  size = GST_BUFFER_SIZE (buffer);
  data = GST_BUFFER_DATA (buffer);

  GST_LOG_OBJECT (this, "writing %d bytes", size);

  if (packet_size < 0) {
    GST_LOG_OBJECT (this, "error getting MTU");
    return GST_FLOW_ERROR;
  }

  wrote = gst_dccp_socket_write (client_sock_fd, data, size, packet_size);

  if (wrote != size) {
    GST_DEBUG_OBJECT (this, ("Error while sending data"));
    return GST_FLOW_ERROR;
  }
  return GST_FLOW_OK;
}

/*
 * Create socket address.
 * @return sockaddr_in.
 */
struct sockaddr_in
gst_dccp_create_sockaddr (GstElement * element, gchar * ip, int port)
{
  struct sockaddr_in sin;

  memset (&sin, 0, sizeof (sin));
  sin.sin_family = AF_INET;     /* network socket */
  sin.sin_port = htons (port);  /* on port */
  sin.sin_addr.s_addr = inet_addr (ip); /* on host ip */

  return sin;
}

gboolean
gst_dccp_make_address_reusable (GstElement * element, int sock_fd)
{
  int ret = 1;
  /* make address reusable */
  if (setsockopt (sock_fd, SOL_SOCKET, SO_REUSEADDR,
          (void *) &ret, sizeof (ret)) < 0) {
    GST_ELEMENT_ERROR (element, RESOURCE, SETTINGS, (NULL),
        ("Could not setsockopt: %s", g_strerror (errno)));
    return FALSE;
  }
  return TRUE;
}

/* DCCP socket specific stuffs */
gboolean
gst_dccp_set_ccid (GstElement * element, int sock_fd, uint8_t ccid)
{
  uint8_t ccids[4];             /* for getting the available CCIDs, should be large enough */
  socklen_t len = sizeof (ccids);
  int i, ret;
  gboolean ccid_supported = FALSE;

  /*
   * Determine which CCIDs are available on the host
   */
  ret = getsockopt (sock_fd, SOL_DCCP, DCCP_SOCKOPT_AVAILABLE_CCIDS, &ccids,
      &len);
  if (ret < 0) {
    GST_ERROR_OBJECT (element, "Can not determine available CCIDs");
    return FALSE;
  }

  for (i = 0; i < sizeof (ccids); i++) {
    if (ccid == ccids[i]) {
      ccid_supported = TRUE;
    }
  }

  if (!ccid_supported) {
    GST_ERROR_OBJECT (element, "CCID specified is not supported");
    return FALSE;
  }

  if (setsockopt (sock_fd, SOL_DCCP, DCCP_SOCKOPT_CCID, &ccid,
          sizeof (ccid)) < 0) {
    GST_ERROR_OBJECT (element, "Can not set CCID");
    return FALSE;
  }

  return TRUE;
}

/*
 * Get the current ccid of TX or RX half-connection. tx_or_rx parameter must be
 * DCCP_SOCKOPT_TX_CCID or DCCP_SOCKOPT_RX_CCID.
 * @return ccid or -1 on error or tx_or_rx not the correct option
 */
uint8_t
gst_dccp_get_ccid (GstElement * element, int sock_fd, int tx_or_rx)
{
  uint8_t ccid;
  socklen_t ccidlen;
  int ret;

  switch (tx_or_rx) {
    case DCCP_SOCKOPT_TX_CCID:
    case DCCP_SOCKOPT_RX_CCID:
      break;
    default:
      return -1;
  }

  ccidlen = sizeof (ccid);
  ret = getsockopt (sock_fd, SOL_DCCP, tx_or_rx, &ccid, &ccidlen);
  if (ret < 0) {
    GST_ERROR_OBJECT (element, "Can not determine available CCIDs");
    return -1;
  }
  return ccid;
}

gint
gst_dccp_get_max_packet_size (GstElement * element, int sock)
{
  int size;
  socklen_t sizelen = sizeof (size);
  if (getsockopt (sock, SOL_DCCP, DCCP_SOCKOPT_GET_CUR_MPS,
          &size, &sizelen) < 0) {
    GST_ELEMENT_ERROR (element, RESOURCE, SETTINGS, (NULL),
        ("Could not get current MTU %d: %s", errno, g_strerror (errno)));
    return -1;
  }
  GST_DEBUG_OBJECT (element, "MTU: %d", size);
  return size;
}

/* Still not used and need to be FIXED */
gboolean
gst_dccp_set_sock_windowsize (GstElement * element, int sock, int winSize,
    gboolean inSend)
{
#ifdef SO_SNDBUF
  int rc;

  if (!inSend) {
    /* receive buffer -- set
     * note: results are verified after connect() or listen(),
     * since some OS's don't show the corrected value until then. */
    rc = setsockopt (sock, SOL_DCCP, SO_RCVBUF,
        (char *) &winSize, sizeof (winSize));
    GST_DEBUG_OBJECT (element, "set rcv sockbuf: %d", winSize);
  } else {
    /* send buffer -- set
     * note: results are verified after connect() or listen(),
     * since some OS's don't show the corrected value until then. */
    rc = setsockopt (sock, SOL_DCCP, SO_SNDBUF,
        (char *) &winSize, sizeof (winSize));
    GST_DEBUG_OBJECT (element, "set snd sockbuf: %d", winSize);
  }

  if (rc < 0) {
    GST_ELEMENT_ERROR (element, RESOURCE, SETTINGS, (NULL),
        ("Could not set window size %d: %s", errno, g_strerror (errno)));
    return FALSE;
  }
#endif /* SO_SNDBUF */

  return TRUE;
}
