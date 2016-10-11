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
#include "proxy.h"
#include "util.h"

struct watcher_proxy_data
{
	GMainLoop   *loop;
	GIOChannel  *channel;
	gchar       *msg_to_send;
	GString     *msg_received;
};

/**
 * Free resources associated with \p proxy.
 *
 * \param proxy \ref cc_proxy.
 *
 */
void
cc_proxy_free (struct cc_proxy *proxy) {
	if (! proxy) {
		return;
	}

	g_free_if_set (proxy->agent_ctl_socket);
	g_free_if_set (proxy->agent_tty_socket);

	if (proxy->socket) {
		g_object_unref (proxy->socket);
	}

	g_free (proxy);
}

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

	len = g_utf8_strlen(proxy_data->msg_to_send, -1);

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
	(void)monitor;
	(void)file;
	(void)other_file;

	g_debug("CTL created event: %d", event_type);
	g_main_loop_quit(loop);
}

/**
 * Determine if the hyper command was run successfully.
 *
 * Accomplished by checking the proxy response message which is
 * of the form:
 *
 *     {"success": [true|false], "error": "an explanation" }
 *
 * \param data \ref watcher_proxy_data.
 * \param proxy_success \c true if the last proxy command was
 * successful, else \c false.
 *
 * \return \c true if the proxy response could be checked,
 * else \c false.
 */
static gboolean
cc_proxy_hyper_check_response (const GString *response,
		gboolean *proxy_success)
{
	JsonParser  *parser = NULL;
	JsonReader  *reader = NULL;
	GError      *error = NULL;
	gboolean     ret;

	if (! (response && proxy_success)) {
		return false;
	}

	parser = json_parser_new ();
	reader = json_reader_new (NULL);

	ret = json_parser_load_from_data (parser,
			response->str,
			(gssize)response->len,
			&error);

	if (! ret) {
		g_critical ("failed to parse proxy response: %s",
				error->message);
		g_error_free (error);
		goto out;
	}

	json_reader_set_root (reader, json_parser_get_root (parser));

	ret = json_reader_read_member (reader, "success");
	if (! ret) {
		g_critical ("failed to find proxy response");
		goto out;
	}

	*proxy_success = json_reader_get_boolean_value (reader);

	json_reader_end_member (reader);

	ret = true;

out:
	if (reader) g_object_unref (reader);
	if (parser) g_object_unref (parser);

	return ret;
}

/**
 * Run any command via the \ref CC_OCI_PROXY.
 *
 * \param proxy \ref cc_proxy.
 * \param msg_to_send gchar.
 * \param msg_received GString.
 *
 * \return \c true on success, else \c false.
 */
static gboolean
cc_proxy_run_cmd(struct cc_proxy *proxy,
		gchar *msg_to_send,
		GString* msg_received)
{
	GIOChannel        *channel = NULL;
	int                fd;
	struct watcher_proxy_data proxy_data;
	gboolean ret = false;
	gboolean hyper_result = false;

	if (! (proxy && msg_to_send && msg_received)) {
		return false;
	}

	if (! proxy->socket) {
		g_critical ("no proxy connection");
		return false;
	}

	proxy_data.loop = g_main_loop_new (NULL, false);

	proxy_data.msg_to_send = msg_to_send;

	fd = g_socket_get_fd (proxy->socket);

	channel = g_io_channel_unix_new(fd);
	if (! channel) {
		g_critical("failed to create I/O channel");
		goto out;
	}

	g_io_channel_set_encoding (channel, NULL, NULL);

	proxy_data.msg_received = msg_received;

	/* add a watcher for proxy's socket stdin */
	g_io_add_watch(channel, G_IO_OUT | G_IO_HUP,
	    (GIOFunc)cc_proxy_write_msg, &proxy_data);

	g_debug ("communicating with proxy");

	/* waiting for proxy response */
	g_main_loop_run(proxy_data.loop);

	if (! cc_proxy_hyper_check_response (msg_received,
			&hyper_result)) {
		g_critical ("failed to check proxy response");
		ret = false;
	} else {
		ret = hyper_result;
	}

out:
	g_main_loop_unref (proxy_data.loop);
	g_free (proxy_data.msg_to_send);
	if (channel) {
		g_io_channel_unref(channel);
	}
	return ret;
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
	gchar             *msg_to_send = NULL;
	GString           *msg_received = NULL;
	gboolean           ret = false;

	/* The name of the command used to initiate communicate
	 * with the proxy.
	 */
	const gchar       *proxy_cmd = "hello";

	if (! (proxy && proxy->socket && container_id)) {
		return false;
	}

	obj = json_object_new ();
	data = json_object_new ();

	json_object_set_string_member (obj, "id", proxy_cmd);

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

	msg_to_send = json_generator_to_data (generator, NULL);

	msg_received = g_string_new("");

	if (! cc_proxy_run_cmd(proxy, msg_to_send, msg_received)) {
		g_critical("failed to run proxy command %s: %s",
				proxy_cmd,
				msg_received->str);
		goto out;
	}

	ret = true;

	g_debug("msg received: %s", msg_received->str);

out:
	if (msg_received) {
		g_string_free(msg_received, true);
	}
	if (obj) {
		json_object_unref (obj);
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
static gboolean
cc_proxy_wait_until_ready (struct cc_oci_config *config)
{
	GFile             *ctl_file = NULL;
	GFileMonitor      *monitor = NULL;
	GMainLoop         *loop = NULL;
	struct stat        st;

	if (! (config && config->proxy)) {
		return false;
	}

	/* Unfortunately launching the hypervisor does not guarantee that
	 * CTL and TTY exist, for this reason we MUST wait for them before
	 * writing down any message into proxy's socket
	 */
	if (stat(config->proxy->agent_ctl_socket, &st)) {
		loop = g_main_loop_new (NULL, false);

		ctl_file = g_file_new_for_path( config->proxy->agent_ctl_socket);

		monitor = g_file_monitor(ctl_file, G_FILE_MONITOR_NONE, NULL, NULL);
		if (! monitor) {
			g_critical("failed to create a file monitor for %s",
				 config->proxy->agent_ctl_socket);
			goto out;
		}

		g_signal_connect(monitor, "changed",
			G_CALLBACK(cc_proxy_ctl_socket_created_callback), loop);

		/* last chance, if CTL socket does not exist we MUST wait for it */
		if (stat(config->proxy->agent_ctl_socket, &st)) {
			g_main_loop_run(loop);
		}
	}

	if (! cc_proxy_hello (config->proxy, config->optarg_container_id)) {
		return false;
	}
out:
	if (loop) {
		g_main_loop_unref(loop);
	}
	if (ctl_file) {
		g_object_unref(ctl_file);
	}
	if (monitor) {
		g_object_unref(monitor);
	}

	return true;
}

/**
 * Run a Hyper command via the \ref CC_OCI_PROXY.
 *
 * \note Must already be connected to the proxy.
 *
 * \param config \ref cc_oci_config.
 * \param cmd Name of hyper command to run.
 * \param data payload to pass to \p cmd (optional).
 *
 * \return \c true on success, else \c false.
 */
// FIXME: payload should probably be a JsonObject.
static gboolean
cc_proxy_run_hyper_cmd (struct cc_oci_config *config,
		const char *cmd, const char *payload)
{
	JsonObject        *obj = NULL;
	JsonObject        *data = NULL;
	JsonNode          *root = NULL;
	JsonGenerator     *generator = NULL;
	gboolean           ret = false;
	gchar             *msg_to_send = NULL;
	GString           *msg_received = NULL;

	/* data is optional */
	if (! (config && cmd)) {
		return false;
	}

	obj = json_object_new ();
	data = json_object_new ();

	/* tell the proxy to run in pass-through mode and forward
	 * the request on to hyperstart in the VM.
	 */
	json_object_set_string_member (obj, "id", "hyper");

	/* add the hyper command name and the data to pass to the
	 * command.
	 */
	json_object_set_string_member (data, cmd, payload);

	json_object_set_object_member (obj, "data", data);

	root = json_node_new (JSON_NODE_OBJECT);
	generator = json_generator_new ();
	json_node_take_object (root, obj);

	json_generator_set_root (generator, root);
	g_object_set (generator, "pretty", FALSE, NULL);

	msg_to_send = json_generator_to_data (generator, NULL);

	msg_received = g_string_new("");

	if (! cc_proxy_run_cmd(config->proxy, msg_to_send, msg_received)) {
		g_critical("failed to run hyper cmd %s: %s",
				cmd,
				msg_received->str);
		goto out;
	}

	g_debug("msg received: %s", msg_received->str);

	ret = true;

out:
	if (msg_received) {
		g_string_free(msg_received, true);
	}
	if (obj) {
		json_object_unref (obj);
	}

	return ret;
}

/**
 * Request \ref CC_OCI_PROXY create a new POD (container group).
 *
 * \note Must already be connected to the proxy.
 *
 * \param config \ref cc_oci_config.
 *
 * \return \c true on success, else \c false.
 */
gboolean
cc_proxy_hyper_pod_create (struct cc_oci_config *config)
{
	JsonObject        *data = NULL;
	JsonArray         *array = NULL;
	JsonNode          *root = NULL;
	JsonGenerator     *generator = NULL;
	gchar             *msg_to_send = NULL;
	gboolean           ret = false;

	if (! (config && config->proxy)) {
		return false;
	}

	if (! cc_proxy_connect (config->proxy)) {
		return false;
	}

	if (! cc_proxy_wait_until_ready (config)) {
		g_critical ("failed to wait for proxy %s", CC_OCI_PROXY);
		goto out;
	}

	/* json stanza for create pod (STARTPOD without containers)*/
	data = json_object_new ();

	json_object_set_string_member (data, "hostname",
		config->optarg_container_id);

	/* FIXME: missing interfaces, routes, dns,
	 * portmappingWhiteLists, externalNetworks ?
	 */

	array = json_array_new();

	json_object_set_array_member(data, "containers", array);

	json_object_set_string_member (data, "hostname",
		config->optarg_container_id);

	json_object_set_string_member (data, "shareDir", "rootfs");

	root = json_node_new (JSON_NODE_OBJECT);
	generator = json_generator_new ();
	json_node_take_object (root, data);

	json_generator_set_root (generator, root);
	g_object_set (generator, "pretty", FALSE, NULL);

	msg_to_send = json_generator_to_data (generator, NULL);

	if (! cc_proxy_run_hyper_cmd (config, "STARTPOD", msg_to_send)) {
		g_critical("failed to run pod create");
		goto out;
	}

	ret = true;

out:
	if (data) {
		json_object_unref (data);
	}

	cc_proxy_disconnect (config->proxy);

	return ret;
}
