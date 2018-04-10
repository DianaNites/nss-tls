/* This file is part of nss-tls.
 *
 * Copyright (C) 2018  Dima Krasner
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <stdlib.h>
#include <arpa/inet.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

#include "nss-tls.h"

struct nss_tls_query {
    struct nss_tls_req req;
    struct nss_tls_res res;
    gint64 type;
    GSocketConnection *connection;
    gboolean ok;
};

static
void
on_done (GObject         *source_object,
         GAsyncResult    *res,
         gpointer        user_data)
{
    struct nss_tls_query *query = (struct nss_tls_query *)user_data;

    if (g_output_stream_write_all_finish (G_OUTPUT_STREAM (source_object),
                                          res,
                                          NULL,
                                          NULL)) {
        g_debug ("Done querying %s", query->req.name);
    } else {
        g_debug ("Failed to query %s", query->req.name);
    }

    g_object_unref (query->connection);
    g_free (query);
}

static
void
on_answer (JsonArray    *array,
           guint        index_,
           JsonNode     *element_node,
           gpointer     user_data)
{
    struct nss_tls_query *query = (struct nss_tls_query *)user_data;
    JsonObject *answero;
    JsonNode *type, *data;
    const gchar *s;
    gint64 i;

    answero = json_node_get_object (element_node);

    type = json_object_get_member (answero, "type");
    if (!type) {
        g_warning ("No type member for %s", query->req.name);
        return;
    }

    /* if the type doesn't match, it's OK - continue to the next answer */
    i = json_node_get_int (type);
    if (i != query->type) {
        return;
    }

    data = json_object_get_member (answero, "data");
    if (!data) {
        g_debug ("No data for answer %u of %s", index_, query->req.name);
        return;
    }

    s = json_node_get_string (data);
    if (!s) {
        g_debug ("Invalid data for answer %u of %s", index_, query->req.name);
        return;
    }

    if (inet_pton (query->req.af, s, &query->res.addr)) {
        g_debug ("%s = %s", query->req.name, s);
        query->ok = TRUE;
    }
}

static
void
on_res (GObject         *source_object,
        GAsyncResult    *res,
        gpointer        user_data)
{
    GError *err = NULL;
    struct nss_tls_query *query = (struct nss_tls_query *)user_data;
    GInputStream *in;
    JsonParser *j = NULL;
    JsonNode *root;
    JsonObject *rooto;
    JsonArray *answers;
    GOutputStream *out;

    query->ok = FALSE;

    in = soup_session_send_finish (SOUP_SESSION (source_object),
                                   res,
                                   &err);
    if (!in) {
        if (err) {
            g_warning ("Failed to query %s: %s",
                       query->req.name,
                       err->message);
        } else {
            g_warning ("Failed to query %s", query->req.name);
        }
        goto fail;
    }

    j = json_parser_new ();
    if (!json_parser_load_from_stream (j, in, NULL, &err)) {
        if (err) {
            g_warning ("Failed to parse the result for %s: %s",
                       query->req.name,
                       err->message);
        } else {
            g_warning ("Failed to parse the result for %s", query->req.name);
        }
        goto fail;
    }

    root = json_parser_get_root (j);
    rooto = json_node_get_object (root);
    if (!rooto) {
        g_warning ("No root object for %s", query->req.name);
        goto fail;
    }

    answers = json_object_get_array_member (rooto, "Answer");
    if (!answers) {
        g_warning ("No Answer member for %s", query->req.name);
        goto fail;
    }

    json_array_foreach_element (answers, on_answer, query);

    if (query->ok) {
        out = g_io_stream_get_output_stream (G_IO_STREAM (query->connection));
        g_output_stream_write_all_async (out,
                                         &query->res,
                                         sizeof(query->res),
                                         G_PRIORITY_DEFAULT,
                                         NULL,
                                         on_done,
                                         query);
    }

fail:
    if (j)
        g_object_unref (j);

    if (query->ok)
        return;

    if (err) {
        g_error_free (err);
    }

    g_object_unref (query->connection);
    g_free (query);
}

static
void
on_req (GObject         *source_object,
        GAsyncResult    *res,
        gpointer        user_data)
{
    GError *err = NULL;
    SoupSession *sess;
    SoupMessage *msg;
    gchar *url;
    struct nss_tls_query *query = user_data;
    gsize len;

    if (!g_input_stream_read_all_finish (G_INPUT_STREAM (source_object),
                                         res,
                                         &len,
                                         &err)) {
        if (err) {
            g_warning ("Failed to receive a request: %s", err->message);
            g_error_free (err);
        }
        else
            g_warning ("Failed to receive a request");
        goto fail;
    }

    if (len != sizeof(query->req)) {
        g_debug ("Bad request");
        goto fail;
    }

    g_debug ("Querying %s", query->req.name);

    switch (query->req.af) {
    case AF_INET:
        /* A */
        query->type = 1;
        url = g_strdup_printf ("https://"NSS_TLS_RESOLVER"/dns-query?ct=application/dns-json&name=%s&type=A", query->req.name);
        break;

    case AF_INET6:
        /* AAAA */
        query->type = 28;
        url = g_strdup_printf ("https://"NSS_TLS_RESOLVER"/dns-query?ct=application/dns-json&name=%s&type=AAAA", query->req.name);
        break;

    default:
        goto fail;
    }

    g_debug ("Fetching %s", url);

    sess = soup_session_new ();
    msg = soup_message_new ("GET", url);

    soup_session_send_async (sess, msg, NULL, on_res, query);
    g_free (url);

    return;

fail:
    g_object_unref (query->connection);
    g_free (query);
}

static void
on_incoming (GSocketService     *service,
             GSocketConnection  *connection,
             GObject            *source_object,
             gpointer           user_data)
{
    GSocket *s;
    struct nss_tls_query *query;
    GInputStream *in;

    s = g_socket_connection_get_socket (connection);
    g_socket_set_timeout (s, NSS_TLS_TIMEOUT);

    query = g_new0 (struct nss_tls_query, 1);
    query->connection = g_object_ref (connection);

    in = g_io_stream_get_input_stream (G_IO_STREAM (connection));
    g_input_stream_read_all_async (in,
                                   &query->req,
                                   sizeof(query->req),
                                   G_PRIORITY_DEFAULT,
                                   NULL,
                                   on_req,
                                   query);
}

static
gboolean
on_term (gpointer user_data)
{
    g_main_loop_quit ((GMainLoop *)user_data);
    return FALSE;
}

int main(int argc, char **argv)
{
    GMainLoop *loop;
    GSocketService *s;
    GSocketAddress *sa;

    g_unlink (NSS_TLS_SOCKET);
    sa = g_unix_socket_address_new (NSS_TLS_SOCKET);
    s = g_socket_service_new ();
    loop = g_main_loop_new (NULL, FALSE);

    g_socket_listener_add_address (G_SOCKET_LISTENER (s),
                                   sa,
                                   G_SOCKET_TYPE_STREAM,
                                   0,
                                   NULL,
                                   NULL,
                                   NULL);

    g_signal_connect (s,
                      "incoming",
                      G_CALLBACK (on_incoming),
                      NULL);
    g_socket_service_start (s);
    g_chmod (NSS_TLS_SOCKET , 0666);

    g_unix_signal_add (SIGINT, on_term, loop);
    g_unix_signal_add (SIGTERM, on_term, loop);

    g_main_loop_run(loop);

    g_main_loop_unref(loop);
    g_object_unref(s);
    g_unlink (NSS_TLS_SOCKET);
    g_object_unref(sa);

    return EXIT_SUCCESS;
}