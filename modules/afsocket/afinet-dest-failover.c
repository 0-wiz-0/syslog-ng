
#include "afinet-dest-failover.h"
#include "messages.h"
#include "timeutils.h"
#include "afinet.h"

#include <iv.h>

struct _AFInetDestDriverFailover
{
  GList *server_candidates;
  GList *current_server_candidate;

  LogExprNode *owner_expression;
  TransportMapper *transport_mapper;
  SocketOptions *socket_options;
  GSockAddr *bind_addr;
  gchar *dest_port;

  gpointer on_primary_available_cookie;
  AFInetOnPrimaryAvailable on_primary_available_func;

  struct iv_timer timer;
  struct iv_fd fd;
  guint tcp_probe_interval;
  guint successful_probes_required;
  guint successful_probes_received;


  GSockAddr *primary_addr;
  const gchar *primary_hostname;
};

static const int TCP_PROBE_INTERVAL_DEFAULT = 60;
static const int SUCCESSFUL_PROBES_REQUIRED_DEFAULT = 3;

void
afinet_dd_failover_set_tcp_probe_interval(AFInetDestDriverFailover *self, gint tcp_probe_interval)
{
  self->tcp_probe_interval = tcp_probe_interval;
}

void
afinet_dd_failover_set_successful_probes_required(AFInetDestDriverFailover *self, gint successful_probes_required)
{
  self->successful_probes_required = successful_probes_required;
}

static GList *
_primary(AFInetDestDriverFailover *self)
{
  return g_list_first(self->server_candidates);
}

static void
_afinet_dd_hand_over_connection_to_afsocket(AFInetDestDriverFailover *self)
{
  self->successful_probes_received = 0;
  self->current_server_candidate = _primary(self);
  self->on_primary_available_func(self->on_primary_available_cookie, self->fd.fd, self->primary_addr);
  self->primary_addr = NULL;
  self->fd.fd = -1;
}

static void
_afinet_dd_start_failback_timer(AFInetDestDriverFailover *self)
{
  glong elapsed_time;

  iv_validate_now();
  elapsed_time = timespec_diff_msec(&iv_now, &(self->timer.expires));
  self->timer.expires = iv_now;

  if (elapsed_time < (self->tcp_probe_interval*1000))
    {
      timespec_add_msec(&self->timer.expires, (self->tcp_probe_interval*1000 - elapsed_time));
    }
  iv_timer_register(&self->timer);
}

static void
_afinet_dd_tcp_probe_succeded(AFInetDestDriverFailover *self)
{
  self->successful_probes_received++;
  msg_notice("Probing primary server successful",
             evt_tag_int("successful-probes-received", self->successful_probes_received),
             evt_tag_int("successful-probes-required", self->successful_probes_required));

  if (self->successful_probes_received >= self->successful_probes_required)
    {
      msg_notice("Primary server seems to be stable, reconnecting to primary server");
      _afinet_dd_hand_over_connection_to_afsocket(self);
    }
  else
    {
      close(self->fd.fd);
      g_sockaddr_unref(self->primary_addr);
      _afinet_dd_start_failback_timer(self);
    }
}

static void
_afinet_dd_tcp_probe_failed(AFInetDestDriverFailover *self)
{
  self->successful_probes_received = 0;
  g_sockaddr_unref(self->primary_addr);
  _afinet_dd_start_failback_timer(self);
}

static gboolean
_socket_succeeded(AFInetDestDriverFailover *self)
{
  int error = 0;
  socklen_t errorlen = sizeof(error);
  gchar buf[MAX_SOCKADDR_STRING];

  if (getsockopt(self->fd.fd, SOL_SOCKET, SO_ERROR, &error, &errorlen) == -1)
    {
      msg_error("getsockopt(SOL_SOCKET, SO_ERROR) failed for connecting socket",
                evt_tag_int("fd", self->fd.fd),
                evt_tag_str("server", g_sockaddr_format(self->primary_addr, buf, sizeof(buf), GSA_FULL)),
                evt_tag_error(EVT_TAG_OSERROR));
      return FALSE;
    }

  if (error)
    {
      msg_error("Connection towards primary server failed",
                evt_tag_int("fd", self->fd.fd),
                evt_tag_str("server", g_sockaddr_format(self->primary_addr, buf, sizeof(buf), GSA_FULL)),
                evt_tag_errno(EVT_TAG_OSERROR, error));
      close(self->fd.fd);
      return FALSE;
    }
  return TRUE;
}

static void
_afinet_dd_handle_tcp_probe_socket(gpointer s)
{
  AFInetDestDriverFailover *self = (AFInetDestDriverFailover *) s;

  if (iv_fd_registered(&self->fd))
    iv_fd_unregister(&self->fd);

  if (_socket_succeeded(self))
    _afinet_dd_tcp_probe_succeded(self);
  else
    _afinet_dd_tcp_probe_failed(self);
}

static gboolean
_connect_normal(GIOStatus iostatus)
{
  return G_IO_STATUS_NORMAL == iostatus;
}

static gboolean
_connect_in_progress(GIOStatus iostatus)
{
  return G_IO_STATUS_ERROR == iostatus && EINPROGRESS == errno;
}

static void
_afinet_dd_tcp_probe_primary_server(AFInetDestDriverFailover *self)
{
  GIOStatus iostatus = g_connect(self->fd.fd, self->primary_addr);
  if (_connect_normal(iostatus))
    {
      msg_notice("Successfully connected to primary");
      _afinet_dd_tcp_probe_succeded(self);
      return;
    }

  if (_connect_in_progress(iostatus))
    {
      iv_fd_register(&self->fd);
      return;
    }

  gchar buf[MAX_SOCKADDR_STRING];
  msg_error("Connection towards primary server failed",
            evt_tag_int("fd", self->fd.fd),
            evt_tag_str("server", g_sockaddr_format(self->primary_addr, buf, sizeof(buf), GSA_FULL)),
            evt_tag_error(EVT_TAG_OSERROR));
  close(self->fd.fd);
  _afinet_dd_tcp_probe_failed(self);
}

static const gchar *
_failover_get_hostname(GList *l)
{
  return (const gchar *)(l->data);
}

static gboolean
_resolve_primary_address(AFInetDestDriverFailover *self)
{
  if (!resolve_hostname_to_sockaddr(&self->primary_addr, self->transport_mapper->address_family,
                                    _failover_get_hostname(_primary(self))))
    {
      msg_warning("Unable to resolve the address of the primary server",
                  evt_tag_str("address", _failover_get_hostname(_primary(self))));
      return FALSE;
    }
  g_sockaddr_set_port(self->primary_addr, afinet_determine_port(self->transport_mapper, self->dest_port));
  return TRUE;
}

static gboolean
_setup_failback_fd(AFInetDestDriverFailover *self)
{
  if (!transport_mapper_open_socket(self->transport_mapper, self->socket_options, self->bind_addr,
                                    AFSOCKET_DIR_SEND, &self->fd.fd))
    {
      msg_error("Error creating socket for tcp-probe the primary server",
                evt_tag_error(EVT_TAG_OSERROR));
      return FALSE;
    }
  return TRUE;
}

static void
_afinet_dd_failback_timer_elapsed(void *cookie)
{
  AFInetDestDriverFailover *self = (AFInetDestDriverFailover *) cookie;

  msg_notice("Probing the primary server.",
             evt_tag_int("tcp-probe-interval", self->tcp_probe_interval));

  iv_validate_now();
  self->timer.expires = iv_now; // register starting time, required for "elapsed_time"

  if (!_resolve_primary_address(self))
    {
      _afinet_dd_tcp_probe_failed(self);
      return;
    }

  if (!_setup_failback_fd(self))
    {
      _afinet_dd_tcp_probe_failed(self);
      return;
    }

  _afinet_dd_tcp_probe_primary_server(self);
}

static gboolean
_is_failback_enabled(AFInetDestDriverFailover *self)
{
  return self->on_primary_available_func != NULL;
}

static void
_init_current_server_candidate(AFInetDestDriverFailover *self )
{
  self->current_server_candidate = _is_failback_enabled(self) ? g_list_next(_primary(self)) : _primary(self);

  if (_primary(self) == self->current_server_candidate)
    {
      msg_warning("Last failover server reached, trying the original host again",
                  evt_tag_str("host", _failover_get_hostname(self->current_server_candidate)),
                  log_expr_node_location_tag(self->owner_expression));
    }
  else
    {
      msg_warning("Last failover server reached, trying the first failover again",
                  evt_tag_str("next_failover_server", _failover_get_hostname(self->current_server_candidate)),
                  log_expr_node_location_tag(self->owner_expression));
    }
}

static void
_step_current_server_iterator(AFInetDestDriverFailover *self)
{
  if (!self->current_server_candidate)
    {
      self->current_server_candidate = _primary(self);
      return;
    }

  GList *current = self->current_server_candidate;
  self->current_server_candidate = g_list_next(self->current_server_candidate);

  if (!self->current_server_candidate)
    {
      _init_current_server_candidate(self);
      return;
    }

  if (_is_failback_enabled(self) && _primary(self) == current)
    {
      _afinet_dd_start_failback_timer(self);
      msg_warning("Current primary server is inaccessible, sending the messages to the next failover server",
                  evt_tag_str("next_failover_server", _failover_get_hostname(self->current_server_candidate)),
                  log_expr_node_location_tag(self->owner_expression));
      return;
    }

  msg_warning("Current failover server is inaccessible, sending the messages to the next failover server",
              evt_tag_str("next_failover_server", _failover_get_hostname(self->current_server_candidate)),
              log_expr_node_location_tag(self->owner_expression));
}


const gchar *
afinet_dd_failover_get_hostname(AFInetDestDriverFailover *self)
{
  return (const gchar *)(self->current_server_candidate->data);
}

void
afinet_dd_failover_next(AFInetDestDriverFailover *self)
{
  _step_current_server_iterator(self);
}

void
afinet_dd_failover_add_servers(AFInetDestDriverFailover *self, GList *failovers)
{
  self->server_candidates = g_list_concat(self->server_candidates, failovers);
}

void
afinet_dd_failover_enable_failback(AFInetDestDriverFailover *self, gpointer cookie,
                                   AFInetOnPrimaryAvailable callback_function)
{
  self->on_primary_available_cookie = cookie;
  self->on_primary_available_func = callback_function;
}

static void
_afinet_dd_init_failback_handlers(AFInetDestDriverFailover *self)
{
  IV_TIMER_INIT(&self->timer);
  self->timer.cookie = self;
  self->timer.handler = _afinet_dd_failback_timer_elapsed;

  IV_FD_INIT(&self->fd);
  self->fd.cookie = self;
  self->fd.handler_out = (void (*)(void *)) _afinet_dd_handle_tcp_probe_socket;
}

void
afinet_dd_failover_init(AFInetDestDriverFailover *self,
                        LogExprNode *owner_expr, TransportMapper *transport_mapper,
                        const gchar *primary, gchar *dest_port,
                        SocketOptions *socket_options, GSockAddr *bind_addr)
{
  self->server_candidates = g_list_prepend(self->server_candidates, g_strdup(primary));
  self->owner_expression = owner_expr;
  self->transport_mapper = transport_mapper;
  self->dest_port = dest_port;
  self->socket_options = socket_options;
  self->bind_addr = bind_addr;
  _afinet_dd_init_failback_handlers(self);
}

AFInetDestDriverFailover *
afinet_dd_failover_new(void)
{
  AFInetDestDriverFailover *self = g_new0(AFInetDestDriverFailover, 1);
  self->tcp_probe_interval = TCP_PROBE_INTERVAL_DEFAULT;
  self->successful_probes_required = SUCCESSFUL_PROBES_REQUIRED_DEFAULT;
  return self;
}

static void
_afinet_dd_stop_failback_handlers(AFInetDestDriverFailover *self)
{
  if (iv_timer_registered(&self->timer))
    iv_timer_unregister(&self->timer);

  if (iv_fd_registered(&self->fd))
    {
      iv_fd_unregister(&self->fd);
      close(self->fd.fd);
    }
}

void
afinet_dd_failover_free(AFInetDestDriverFailover *self)
{
  if (!self)
    return;

  _afinet_dd_stop_failback_handlers(self);
  g_list_free_full(self->server_candidates, g_free);
  g_sockaddr_unref(self->primary_addr);
}
