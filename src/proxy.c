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
#include <sys/stat.h>
#include <gio/gunixsocketaddress.h>
#include "oci.h"
#include "json.h"
#include "common.h"

struct watcher_proxy_data
{
	GMainLoop   *loop;
	GIOChannel  *channel;
	gchar       *msg_to_send;
	GString     *msg_received;
};

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
		g_critical ("failed to connect to proxy socket %s: %s",
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
 * Read a message from proxy's socket.
 *
 * \param source GIOChannel.
 * \param condition GIOCondition.
 * \param proxy_data struct watcher_proxy_data.
 *
 * \return \c false
 */
static gboolean
cc_proxy_read_msg(GIOChannel *source, GIOCondition condition,
	struct watcher_proxy_data *proxy_data)
{
	GIOStatus status;
	gchar buffer[LINE_MAX];
	gsize bytes_read;

	if (condition == G_IO_HUP) {
		g_io_channel_unref(source);
		goto out;
	}

	/* read and print all chars */
	while(true) {
		status = g_io_channel_read_chars(source, buffer, sizeof(buffer),
				&bytes_read, NULL);
		if (status == G_IO_STATUS_EOF) {
			goto out;
		}
		if (status != G_IO_STATUS_NORMAL) {
			break;
		}
		g_string_append_len(proxy_data->msg_received,
				buffer, (gssize)bytes_read);
	}

	g_debug("message read from proxy socket: %s",
			proxy_data->msg_received->str);

out:
	g_main_loop_quit (proxy_data->loop);

	/* unregister this watcher */
	return false;
}

/**
 * Write down a message into proxy's socket.
 *
 * \param source GIOChannel.
 * \param condition GIOCondition.
 * \param proxy_data struct watcher_proxy_data.
 *
 * \return \c false
 */
static gboolean
cc_proxy_write_msg(GIOChannel *source, GIOCondition condition,
	struct watcher_proxy_data *proxy_data)
{
	gsize bytes_written = 0;
	gsize len = 0;

	if (condition == G_IO_HUP) {
		g_io_channel_unref(source);
		goto out;
	}

	/* FIXME: Do not use strlen! */
	len = strlen(proxy_data->msg_to_send);

	g_debug("writing message to proxy socket: %s",
			proxy_data->msg_to_send);

	g_io_channel_write_chars(source,
			proxy_data->msg_to_send,
			(gssize)len, &bytes_written, NULL);

	g_io_channel_flush(source, NULL);

	/* Now we've sent the initial negotiation message,
	 * register a handler to wait for a reply.
	 */
	g_io_add_watch(source, G_IO_IN | G_IO_HUP,
	    (GIOFunc)cc_proxy_read_msg, proxy_data);

out:
	/* unregister this watcher */
	return false;
}

/**
 * Callback used to monitor CTL socket creation
 *
 * \param monitor GFileMonitor.
 * \param file GFile.
 * \param other_file GFile.
 * \param event_type GFileMonitorEvent.
 * \param loop GMainLoop.
 */
static void
cc_proxy_ctl_socket_created_callback(GFileMonitor *monitor, GFile *file,
	GFile *other_file, GFileMonitorEvent event_type, GMainLoop *loop)
{
	if (event_type != G_FILE_MONITOR_EVENT_CREATED) {
		g_critical("socket was not created %s",	g_file_get_path(file));
	}

	g_main_loop_quit(loop);
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
	GIOChannel        *channel = NULL;
	gboolean           ret = false;
	int                fd;
	struct watcher_proxy_data proxy_data;
	GFile             *ctl_file = NULL;
	GFileMonitor      *monitor = NULL;
	struct stat        st;

	if (! (proxy && proxy->socket && container_id)) {
		return false;
	}

	proxy_data.loop = g_main_loop_new (NULL, false);
	if (! proxy_data.loop) {
		g_critical("failed to create main loop");
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
	proxy_data.msg_to_send = json_generator_to_data (generator, NULL);

	fd = g_socket_get_fd (proxy->socket);

	channel = g_io_channel_unix_new(fd);
	if (! channel) {
		g_critical("failed to create I/O channel");
		goto out;
	}

	g_io_channel_set_encoding (channel, NULL, NULL);

	/* Unfortunately launching the hypervisor does not guarantee that
	 * CTL and TTY exist, for this reason we MUST wait for them before
	 * writing down any message into proxy's socket
	 */
	ctl_file = g_file_new_for_path(proxy->agent_ctl_socket);

	monitor = g_file_monitor(ctl_file, G_FILE_MONITOR_NONE, NULL, NULL);
	if (! monitor) {
		g_critical("failed to create a file monitor for %s",
			proxy->agent_ctl_socket);
		goto out;
	}

	g_signal_connect(monitor, "changed",
		G_CALLBACK(cc_proxy_ctl_socket_created_callback), proxy_data.loop);

	/* last chance, if CTL socket does not exist we MUST wait for it */
	if (stat(proxy->agent_ctl_socket, &st)) {
		g_main_loop_run(proxy_data.loop);
	}

	proxy_data.msg_received = g_string_new("");

	/* add a watcher for proxy's socket stdin */
	g_io_add_watch(channel, G_IO_OUT | G_IO_HUP,
	    (GIOFunc)cc_proxy_write_msg, &proxy_data);

	g_debug ("communicating with proxy");

	/* waiting for proxy response */
	g_main_loop_run(proxy_data.loop);

	ret = true;

	if (obj) {
		json_object_unref (obj);
	}

	if (channel) {
		g_io_channel_unref(channel);
	}

out:
	g_main_loop_unref (proxy_data.loop);
	g_free (proxy_data.msg_to_send);
	if (ctl_file) {
		g_object_unref(ctl_file);
	}
	if (monitor) {
		g_object_unref(monitor);
	}
	if (proxy_data.msg_received) {
		g_string_free (proxy_data.msg_received, true);
	}

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
	if (! (config && config->proxy)) {
		return false;
	}

	if (! cc_proxy_connect (config->proxy)) {
		return false;
	}

	if (! cc_proxy_hello (config->proxy,
		config->optarg_container_id)) {
		return false;
	}

	if (! cc_proxy_disconnect (config->proxy)) {
		return false;
	}

	return true;
}
