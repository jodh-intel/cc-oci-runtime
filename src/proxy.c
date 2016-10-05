/*
 * This file is part of cc-oci-runtime.
 *
 * Copyrighth (C) 2016 Intel Corporation
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <string.h>
#include <gio/gunixsocketaddress.h>
#include "oci.h"
#include "json.h"
#include "common.h"

/**
 * Connect to CC_OCI_PROXY.
 *
 * \param proxy \ref cc_proxy.
 *
 * \return \c true on success, else \c false.
 */
private gboolean
cc_proxy_connect (struct cc_proxy *proxy)
{
	GSocketAddress *addr;
	GError *error = NULL;
	gboolean ret = false;
	const gchar *path = NULL;

	if (! proxy) {
		return false;
	}

	if (proxy->socket) {
		g_critical ("already connected to proxy");
		return false;
	}

	g_debug ("connecting to proxy");

	addr = g_unix_socket_address_new (CC_OCI_PROXY_SOCKET);
	if (! addr) {
		g_critical ("socket path does not exist: %s",
				CC_OCI_PROXY_SOCKET);
		goto out_addr;
	}

	path = g_unix_socket_address_get_path (G_UNIX_SOCKET_ADDRESS (addr));

	proxy->socket = g_socket_new (G_SOCKET_FAMILY_UNIX,
				      G_SOCKET_TYPE_STREAM,
				      G_SOCKET_PROTOCOL_DEFAULT,
				      &error);
	if (! proxy->socket) {
		g_critical ("failed to create socket for %s: %s",
				path,
				error->message);
		g_error_free (error);
		goto out_socket;
	}

	/* block on write and read */
	g_socket_set_blocking (proxy->socket, TRUE);

	ret = g_socket_connect (proxy->socket, addr, NULL, &error);
	if (! ret) {
		g_critical ("failed to connect to socket %s: %s",
				path,
				error->message);
		g_error_free (error);
		goto out_connect;
	}

	g_debug ("connected to proxy socket %s", path);

	ret = true;

	return ret;

out_connect:
	g_clear_object (&proxy->socket);
out_socket:
	g_object_unref (addr);
out_addr:
	return ret;
}

/**
 * Disconnect from CC_OCI_PROXY.
 *
 * \param proxy \ref cc_proxy.
 *
 * \return \c true on success, else \c false.
 */
private gboolean
cc_proxy_disconnect (struct cc_proxy *proxy)
{
	if (! proxy) {
		return false;
	}

	if (! proxy->socket) {
		g_critical ("not connected to proxy");
		return false;
	}

	g_debug ("disconnecting from proxy");

	g_socket_close (proxy->socket, NULL);
	g_clear_object (&proxy->socket);

	return true;
}

/**
 * Send the initial message to the proxy
 * which will block until it is ready. 
 *
 * \param proxy \ref cc_proxy.
 * \param container_id container id.
 *
 * \return \c true on success, else \c false.
 */
static gboolean
cc_proxy_hello (struct cc_proxy *proxy, const char *container_id)
{
	JsonObject        *obj = NULL;
	JsonObject        *data = NULL;
	JsonNode          *root = NULL;
	JsonGenerator     *generator = NULL;
	g_autofree gchar  *msg = NULL;
	GError            *error = NULL;
	GIOChannel        *channel = NULL;
	gboolean           ret = false;
	int                fd;
	GIOStatus          status;
	gsize              bytes_handled;
	gchar              buffer[CC_OCI_NET_BUF_SIZE] = { 0 };

	if (! (proxy && proxy->socket && container_id)) {
		return false;
	}

	obj = json_object_new ();
	data = json_object_new ();

	/* "hello" is the command used to initiate communicate with the
	 * proxy.
	 */
	json_object_set_string_member (obj, "id", "hello");

	json_object_set_string_member (data, "containerId",
			container_id);

	json_object_set_string_member (data, "ctlSerial",
			proxy->agent_ctl_socket);

	json_object_set_string_member (data, "ioSerial",
			proxy->agent_tty_socket);

	json_object_set_object_member (obj, "data", data);

	root = json_node_new (JSON_NODE_OBJECT);
	generator = json_generator_new ();
	json_node_take_object (root, obj);

	json_generator_set_root (generator, root);
	g_object_set (generator, "pretty", FALSE, NULL);
	msg = json_generator_to_data (generator, NULL);

	fd = g_socket_get_fd (proxy->socket);

	channel = g_io_channel_unix_new(fd);
	if (! channel) {
		g_critical("failed to create I/O channel");
		goto out;
	}

	g_io_channel_set_encoding (channel, NULL, NULL);

	g_debug ("sending initial message to proxy (fd %d)", fd);

	/* blocking write */
	status = g_io_channel_write_chars (channel,
			msg, -1,
			&bytes_handled, &error);

	if (status != G_IO_STATUS_NORMAL) {
		g_critical ("failed to prepare msg for proxy");
		if (error) {
			g_critical ("error: %s", error->message);
			g_error_free (error);
		}
		goto out;
	}

	status = g_io_channel_flush (channel, &error);

	if (status != G_IO_STATUS_NORMAL) {
		g_critical ("failed to send msg to proxy");
		if (error) {
			g_critical ("error: %s", error->message);
			g_error_free (error);
		}
		goto out;
	}

	g_debug ("waiting for proxy reply");

	status = g_io_channel_read_chars (channel,
			buffer,
			sizeof (buffer),
			&bytes_handled, &error);

	if (status != G_IO_STATUS_NORMAL) {
		g_critical ("failed to receive msg from proxy");
		if (error) {
			g_critical ("error: %s", error->message);
			g_error_free (error);
		}
		goto out;
	}

	ret = true;

	if (obj) {
		json_object_unref (obj);
	}

	if (channel) {
		g_io_channel_unref(channel);
	}

out:
	return ret;
}

/**
 * Connect to \ref CC_OCI_PROXY and wait until it is ready.
 *
 * \param config \ref cc_oci_config.
 *
 * \return \c true on success, else \c false.
 */
gboolean
cc_proxy_wait_until_ready (struct cc_oci_config *config)
{
	if (! config) {
		return false;
	}

	if (! cc_proxy_connect (&config->proxy)) {
		return false;
	}

	if (! cc_proxy_hello (&config->proxy,
		config->optarg_container_id)) {
		return false;
	}

	if (! cc_proxy_disconnect (&config->proxy)) {
		return false;
	}

	return true;
}
