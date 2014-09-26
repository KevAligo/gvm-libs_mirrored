/* openvas-libraries/base
 * $Id$
 * Description: API to handle OSP implementation.
 *
 * Authors:
 * Hani Benhabiles <hani.benhabiles@greenbone.net>
 *
 * Copyright:
 * Copyright (C) 2014 Greenbone Networks GmbH
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <glib.h>

#include "openvas_hosts.h"
#include "../misc/openvas_server.h"
#include "../misc/openvas_uuid.h"
#include "../omp/xml.h"
#include "osp.h"


#undef  G_LOG_DOMAIN
#define G_LOG_DOMAIN "lib  osp"

typedef struct osp_connection {
  gnutls_session_t session;
  int socket;
  char *host;
  int port;
} osp_connection_t;

struct osp_param {
  char *id;
  char *name;
  char *desc;
  char *def;
  osp_param_type_t type;
};


static int
osp_send_command (osp_connection_t *, entity_t *, const char *, ...)
    __attribute__((__format__(__printf__, 3, 4)));


/* @brief Open a new connection to an OSP server.
 *
 * @param[in]   host    Host of OSP server.
 * @param[in]   port    Port of OSP server.
 * @param[in]   cacert  CA public key.
 * @param[in]   cert    Client public key.
 * @param[in]   key     Client private key.
 *
 * @return New osp connection, NULL if error.
 */
osp_connection_t *
osp_connection_new (const char *host, int port, const char *cacert,
                    const char *cert, const char *key)
{
  osp_connection_t *connection;

  if (port <= 0 || port > 65535)
    return NULL;
  if (!host || openvas_get_host_type (host) == -1)
    return NULL;
  if (!cert || !key || !cacert)
    return NULL;

  connection = g_malloc0(sizeof (*connection));
  connection->socket = openvas_server_open_with_cert
                        (&connection->session, host, port, cacert, cert, key);
  if (connection->socket == -1)
    {
      g_free (connection);
      return NULL;
    }

  connection->host = g_strdup (host);
  connection->port = port;
  return connection;
}

/* @brief Send a command to an OSP server.
 *
 * @param[in]   connection  Connection to OSP server.
 * @param[in]   command     OSP Command to send.
 * @param[out]  response    Response from OSP server.
 *
 * @return 0 and response, 1 if error.
 */
int
osp_send_command (osp_connection_t *connection, entity_t *response,
                  const char *fmt, ...)
{
  va_list ap;
  int rc = 1;

  va_start (ap, fmt);

  if (!connection || !fmt || !response)
    goto out;

  if (openvas_server_vsendf (&connection->session, fmt, ap) == -1)
    goto out;

  if (read_entity (&connection->session, response))
    goto out;

  rc = 0;

out:
  va_end(ap);

  return rc;
}

/* @brief Close a connection to an OSP server.
 *
 * @param[in]   connection  Connection to OSP server to close.
 */
void
osp_connection_close (osp_connection_t *connection)
{
  if (!connection)
    return;

  openvas_server_close (connection->socket, connection->session);
  g_free (connection->host);
  g_free (connection);
}

/* @brief Get the scanner version from an OSP server.
 *
 * @param[in]   connection  Connection to an OSP server.
 * @param[out]  version     Scanner version received.
 *
 * @return 0 and version value, 1 if error.
 */
int
osp_get_scanner_version (osp_connection_t *connection, char **version)
{
  entity_t entity, child;

  if (!connection)
    return 1;

  if (osp_send_command (connection, &entity, "<get_version/>"))
    return 1;

  /* Extract version. */
  child = entity_child (entity, "scanner");
  if (!child)
    {
      free_entity (entity);
      return 1;
    }
  child = entity_child (child, "version");
  if (!child)
    {
      free_entity (entity);
      return 1;
    }
  if (version)
    *version = g_strdup (entity_text (child));

  free_entity (entity);
  return 0;
}

/* @brief Delete a scan from an OSP server.
 *
 * @param[in]   connection  Connection to an OSP server.
 * @param[in]   scan_id     ID of scan to delete.
 *
 * @return 0 if success, 1 if error.
 */
int
osp_delete_scan (osp_connection_t *connection, const char *scan_id)
{
  entity_t entity;
  int ret = 0;
  const char *status;

  if (!connection)
    return 1;

  ret = osp_send_command (connection, &entity, "<delete_scan scan_id='%s'/>",
                          scan_id);
  if (ret)
    return 1;

  /* Check response status. */
  status = entity_attribute (entity, "status");
  assert (status);
  if (strcmp (status, "200"))
    ret = 1;

  free_entity (entity);
  return ret;
}

int
osp_get_scan (osp_connection_t *connection, const char *scan_id,
              char **report_xml)
{
  entity_t entity, child;
  GString *string;
  int progress;
  int rc;

  if (!connection)
    return 1;

  rc = osp_send_command (connection, &entity, "<get_scans scan_id='%s'/>",
                         scan_id);
  if (rc)
    return 1;

  child = entity_child (entity, "scan");
  if (!child)
    {
      free_entity (entity);
      return -1;
    }
  progress = atoi (entity_attribute (child, "progress"));
  string = g_string_new ("");
  print_entity_to_string (child, string);
  free_entity (entity);
  *report_xml = g_string_free (string, FALSE);
  return progress;
}

static void
option_concat_as_xml (gpointer key, gpointer value, gpointer pstr)
{
  char *options_str, *tmp, *key_escaped, *value_escaped;

  options_str = *(char **) pstr;

  key_escaped = g_markup_escape_text ((char *) key, -1);
  value_escaped = g_markup_escape_text ((char *) value, -1);
  tmp = g_strdup_printf ("%s<%s>%s</%s>", options_str ?: "", key_escaped,
                         value_escaped, key_escaped);

  g_free (options_str);
  g_free (key_escaped);
  g_free (value_escaped);
  *(char **) pstr = tmp;
}

/* @brief Start an OSP scan against a target.
 *
 * @param[in]   connection  Connection to an OSP server.
 * @param[in]   target      Target host to scan.
 * @param[in]   options     Table of scan options.
 *
 * @return scan_id, null if otherwise. Free with g_free().
 */
char *
osp_start_scan (osp_connection_t *connection, const char *target,
                GHashTable *options)
{
  entity_t entity;
  char *options_str = NULL, *scan_id = NULL;
  int status;
  int rc;

  if (!target)
    return NULL;

  /* Construct options string. */
  if (options)
    g_hash_table_foreach (options, option_concat_as_xml, &options_str);

  rc = osp_send_command (connection, &entity,
                         "<start_scan target='%s'><scanner_params>"
                         "%s</scanner_params></start_scan>",
                         target, options_str ? options_str : "");
  g_free (options_str);
  if (rc)
    return NULL;

  status = atoi (entity_attribute (entity, "status"));
  if (status == 200)
    {
      entity_t child = entity_child (entity, "id");
      assert (child);
      assert (entity_text (child));
      scan_id = g_strdup (entity_text (child));
    }
  else
    {
      const char *text = entity_attribute (entity, "status_text");
      assert (text);
      g_log (G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "start_scan failure: %s\n",
             text);
    }
  free_entity (entity);
  return scan_id;
}

/* @brief Get an OSP parameter's type from its string format.
 *
 * @param[in]   str     OSP parameter in string format.
 *
 * @return OSP parameter type.
 */
static osp_param_type_t
osp_param_str_to_type (const char *str)
{
  assert (str);
  if (!strcmp (str, "integer"))
    return OSP_PARAM_TYPE_INT;
  else if (!strcmp (str, "string"))
    return OSP_PARAM_TYPE_STR;
  else if (!strcmp (str, "password"))
    return OSP_PARAM_TYPE_PASSWORD;
  else if (!strcmp (str, "file"))
    return OSP_PARAM_TYPE_FILE;
  else if (!strcmp (str, "boolean"))
    return OSP_PARAM_TYPE_BOOLEAN;
  assert (0);
}

const char *
osp_param_type_str (const osp_param_t *param)
{
  osp_param_type_t type;

  assert (param);
  type = param->type;
  if (type == OSP_PARAM_TYPE_INT)
    return "integer";
  else if (type == OSP_PARAM_TYPE_STR)
    return "string";
  else if (type == OSP_PARAM_TYPE_PASSWORD)
    return "password";
  else if (type == OSP_PARAM_TYPE_FILE)
    return "file";
  else if (type == OSP_PARAM_TYPE_BOOLEAN)
    return "boolean";
  assert (0);
}

/* @brief Get an OSP scanner's parameters.
 *
 * @param[in]   connection  Connection to an OSP server.
 *
 * @return List of osp_param_t, null if error. Free with g_free().
 */
GSList *
osp_get_scanner_params (osp_connection_t *connection)
{
  entity_t entity, child;
  entities_t entities;
  GSList *params = NULL;

  assert (connection);

  if (osp_send_command (connection, &entity, "<get_scanner_details/>"))
    return NULL;
  child = entity_child (entity, "scanner_params");
  if (!child)
    {
      free_entity (entity);
      return NULL;
    }
  entities = child->entities;
  while (entities)
    {
      osp_param_t *param;

      child = entities->data;
      param = osp_param_new ();
      param->id = g_strdup (entity_attribute (child, "id"));
      param->type = osp_param_str_to_type (entity_attribute (child, "type"));
      param->name = g_strdup (entity_text (entity_child (child, "name")));
      param->desc = g_strdup (entity_text (entity_child (child,
                                                         "description")));
      param->def = g_strdup (entity_text (entity_child (child,
                                                        "default")));
      params = g_slist_append (params, param);
      entities = next_entities (entities);
    }
  free_entity (entity);
  return params;
}

/* @brief Create a new OSP parameter.
 *
 * @return New OSP parameter.
 */
osp_param_t *
osp_param_new ()
{
  return g_malloc0 (sizeof (osp_param_t));
}

/* @brief Get an OSP parameter's id.
 *
 * @param[in]   param   OSP parameter.
 *
 * @return ID of OSP parameter.
 */
const char *
osp_param_id (osp_param_t *param)
{
  assert (param);

  return param->id;
}

/* @brief Get an OSP parameter's name.
 *
 * @param[in]   param   OSP parameter.
 *
 * @return Name of OSP parameter.
 */
const char *
osp_param_name (osp_param_t *param)
{
  assert (param);

  return param->name;
}

/* @brief Get an OSP parameter's description.
 *
 * @param[in]   param   OSP parameter.
 *
 * @return Description of OSP parameter.
 */
const char *
osp_param_desc (osp_param_t *param)
{
  assert (param);

  return param->desc;
}

/* @brief Get an OSP parameter's default value.
 *
 * @param[in]   param   OSP parameter.
 *
 * @return Default value of OSP parameter.
 */
const char *
osp_param_default (osp_param_t *param)
{
  assert (param);

  return param->def;
}

/* @brief Get an OSP parameter's type.
 *
 * @param[in]   param   OSP parameter.
 *
 * @return Type of OSP parameter.
 */
osp_param_type_t
osp_param_type (osp_param_t *param)
{
  assert (param);

  return param->type;
}

/* @brief Free an OSP parameter.
 *
 * @param[in] param OSP parameter to destroy.
 */
void
osp_param_free (osp_param_t *param)
{
  if (!param)
    return;
  g_free (param->id);
  g_free (param->name);
  g_free (param->desc);
  g_free (param->def);
  g_free (param);
}