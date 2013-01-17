/*
 * Copyright (c) 2002-2010 BalaBit IT Ltd, Budapest, Hungary
 * Copyright (c) 1998-2010 Balázs Scheidler
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * As an additional exemption you are allowed to compile & link against the
 * OpenSSL libraries as published by the OpenSSL project. See the file
 * COPYING for details.
 *
 */

#include "control.h"
#include "control_server.h"
#include "gsocket.h"
#include "messages.h"
#include "stats.h"
#include "misc.h"
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <iv.h>

static ControlServer *control_server;

static GString *
control_connection_send_stats(GString *command)
{
  gchar *stats = stats_generate_csv();
  GString *result = g_string_new(stats);
  g_free(stats);
  return result;
}

static GString *
control_connection_message_log(GString *command)
{
  gchar **cmds = g_strsplit(command->str, " ", 3);
  gboolean on;
  int *type = NULL;
  GString *result = g_string_sized_new(128);

  if (!cmds[1])
    {
      g_string_assign(result,"Invalid arguments received, expected at least one argument");
      goto exit;
    }

  if (g_str_equal(cmds[1], "DEBUG"))
    type = &debug_flag;
  else if (g_str_equal(cmds[1], "VERBOSE")) 
    type = &verbose_flag;
  else if (g_str_equal(cmds[1], "TRACE")) 
    type = &trace_flag;

  if (type)
    {
      if (cmds[2])
        {
          on = g_str_equal(cmds[2], "ON");
          if (*type != on)
            {
              msg_info("Verbosity changed", evt_tag_str("type", cmds[1]), evt_tag_int("on", on), NULL);
              *type = on;
            }

          g_string_assign(result,"OK");
        }
      else
        {
          g_string_printf(result,"%s=%d", cmds[1], *type);
        }
    }
  else
    return g_string_new("Invalid arguments received");
exit:
  g_strfreev(cmds);
  return result;
}

void 
control_init(const gchar *control_name)
{
  control_server = control_server_new(control_name);
  control_server_register_command_handler(control_server, "LOG", control_connection_message_log);
  control_server_register_command_handler(control_server, "STATS", control_connection_send_stats);
  control_server_start(control_server);
}

void
control_destroy(void)
{
  control_server_free(control_server);
}
