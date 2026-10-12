/**************************************************************************/
/*  remote_debugger_peer.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "remote_debugger_peer.h"

#include "core/config/project_settings.h"
#include "core/io/marshalls.h"
#include "core/io/stream_peer_socket.h"
#include "core/io/stream_peer_tcp.h"
#include "core/io/stream_peer_uds.h"
#include "core/os/os.h"

bool RemoteDebuggerPeerTCP::is_peer_connected() {
	return connected.is_set();
}

bool RemoteDebuggerPeerTCP::has_message() {
	MutexLock lock(mutex);
	return in_queue.size() > 0;
}

Array RemoteDebuggerPeerTCP::get_message() {
	MutexLock lock(mutex);
	List<Array>::Element *E = in_queue.front();
	ERR_FAIL_NULL_V_MSG(E, Array(), "No remote debugger messages in queue.");

	Array out = E->get();
	in_queue.pop_front();
	return out;
}

Error RemoteDebuggerPeerTCP::put_message(const Array &p_arr) {
	MutexLock lock(mutex);
	if (!connected.is_set()) {
		return ERR_UNCONFIGURED;
	}
	if (out_queue.size() >= max_queued_messages || out_queued_bytes >= MAX_QUEUED_BYTES) {
		return ERR_OUT_OF_MEMORY;
	}

	// Array/Dictionary copies retain mutable backing storage. Encode while the
	// producer still owns the message, before it can resume and mutate its data.
	// Callers must not mutate the payload concurrently with this call.
	const int max_size = MIN(get_max_message_size(), MAX_QUEUED_BYTES - out_queued_bytes) - 4;
	int size = 0;
	Error err = encode_variant_bounded(p_arr, nullptr, size, max_size);
	if (err != OK) {
		return err;
	}
	Vector<uint8_t> packet;
	err = packet.resize(size + 4);
	if (err != OK) {
		return err;
	}
	int written = 0;
	err = encode_variant_bounded(p_arr, packet.ptrw() + 4, written, size);
	if (err != OK) {
		return err;
	}
	ERR_FAIL_COND_V(written != size, ERR_INVALID_DATA);
	encode_uint32(written, packet.ptrw());
	out_queued_bytes += packet.size();
	out_queue.push_back(packet);
	return OK;
}

int RemoteDebuggerPeerTCP::get_max_message_size() const {
	return 8 << 20; // 8 MiB
}

void RemoteDebuggerPeerTCP::close() {
	{
		MutexLock lock(mutex);
		connected.clear();
		running.clear();
	}
	if (thread.is_started()) {
		thread.wait_to_finish();
	}
	MutexLock lock(mutex);
	if (tcp_client.is_valid()) {
		tcp_client->disconnect_from_host();
	}
	out_queue.clear();
	out_queued_bytes = 0;
	out_left = 0;
	out_pos = 0;
	out_buf.clear();
	in_queue.clear();
	in_buf.clear();
}

RemoteDebuggerPeerTCP::RemoteDebuggerPeerTCP() {
	in_buf.resize((8 << 20) + 4); // Need 4 extra bytes for encoding packet size.
}

RemoteDebuggerPeerTCP::RemoteDebuggerPeerTCP(Ref<StreamPeerSocket> p_stream) :
		RemoteDebuggerPeerTCP() {
	DEV_ASSERT(p_stream.is_valid());
	tcp_client = p_stream;
	connected.set();
	running.set();
	thread.start(_thread_func, this);
}

RemoteDebuggerPeerTCP::~RemoteDebuggerPeerTCP() {
	close();
}

void RemoteDebuggerPeerTCP::_write_out() {
	while (running.is_set() && tcp_client->get_status() == StreamPeerTCP::STATUS_CONNECTED && tcp_client->wait(NetSocket::POLL_TYPE_OUT) == OK) {
		if (out_left <= 0) {
			MutexLock lock(mutex);
			List<Vector<uint8_t>>::Element *E = out_queue.front();
			if (!E) {
				break;
			}
			out_buf = E->get();
			out_queue.pop_front();
			out_left = out_buf.size();
			out_pos = 0;
		}
		int sent = 0;
		Error err = tcp_client->put_partial_data(out_buf.ptr() + out_pos, out_left, sent);
		if (err != OK) {
			_disconnect_with_error("Remote debugger: Connection lost while writing packet.");
			return;
		}
		if (sent == 0) {
			break; // Wait until the socket can make progress, instead of spinning.
		}
		out_left -= sent;
		out_pos += sent;
		if (out_left == 0) {
			MutexLock lock(mutex);
			out_queued_bytes -= out_buf.size();
			out_buf.clear();
		}
	}
}

void RemoteDebuggerPeerTCP::_read_in() {
	while (running.is_set() && tcp_client->get_status() == StreamPeerTCP::STATUS_CONNECTED && tcp_client->wait(NetSocket::POLL_TYPE_IN) == OK) {
		uint8_t *buf = in_buf.ptrw();
		Error err = OK;
		if (in_left <= 0) {
			{
				MutexLock lock(mutex);
				if (in_queue.size() > max_queued_messages) {
					break; // Too many messages already in queue.
				}
			}
			if (tcp_client->get_available_bytes() < 4) {
				break; // Need 4 more bytes.
			}
			uint32_t size = 0;
			int read = 0;
			err = tcp_client->get_partial_data((uint8_t *)&size, 4, read);
			if (err != OK || read != 4) {
				_disconnect_with_error("Remote debugger: Connection lost while reading packet size.");
				return;
			}
			if (size > (uint32_t)in_buf.size()) {
				_disconnect_with_error(vformat("Remote debugger: Packet too large (%s > %s bytes). Disconnecting.", String::num_uint64(size), String::num_int64(in_buf.size())));
				return;
			}
			in_left = size;
			in_pos = 0;
		}
		int read = 0;
		err = tcp_client->get_partial_data(buf + in_pos, in_left, read);
		if (err != OK || read == 0) {
			_disconnect_with_error("Remote debugger: Connection lost while reading packet payload.");
			return;
		}
		in_left -= read;
		in_pos += read;
		if (in_left == 0) {
			Variant var;
			err = decode_variant(var, buf, in_pos, &read);
			ERR_CONTINUE(read != in_pos || err != OK);
			ERR_CONTINUE_MSG(var.get_type() != Variant::ARRAY, "Malformed packet received, not an Array.");
			MutexLock lock(mutex);
			in_queue.push_back(var);
		}
	}
}

Error RemoteDebuggerPeerTCP::_try_connect(Ref<StreamPeerSocket> tcp_client) {
	const int tries = 6;
	const int waits[tries] = { 1, 10, 100, 1000, 1000, 1000 };

	for (int i = 0; i < tries; i++) {
		tcp_client->poll();
		if (tcp_client->get_status() == StreamPeerTCP::STATUS_CONNECTED) {
			print_verbose("Remote Debugger: Connected!");
			break;
		} else {
			const int ms = waits[i];
			OS::get_singleton()->delay_usec(ms * 1000);
			print_verbose("Remote Debugger: Connection failed with status: '" + String::num_int64(tcp_client->get_status()) + "', retrying in " + String::num_int64(ms) + " msec.");
		}
	}

	if (tcp_client->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
		ERR_PRINT(vformat("Remote Debugger: Unable to connect. Status: %s.", String::num_int64(tcp_client->get_status())));
		return FAILED;
	}
	return OK;
}

void RemoteDebuggerPeerTCP::_thread_func(void *p_ud) {
	// Update in time for 144hz monitors
	const uint64_t min_tick = 6900;
	RemoteDebuggerPeerTCP *peer = static_cast<RemoteDebuggerPeerTCP *>(p_ud);
	while (peer->running.is_set() && peer->is_peer_connected()) {
		uint64_t ticks_usec = OS::get_singleton()->get_ticks_usec();
		peer->_poll();
		if (!peer->is_peer_connected()) {
			break;
		}
		ticks_usec = OS::get_singleton()->get_ticks_usec() - ticks_usec;
		if (ticks_usec < min_tick) {
			OS::get_singleton()->delay_usec(min_tick - ticks_usec);
		}
	}
}

void RemoteDebuggerPeerTCP::poll() {
	// Nothing to do, polling is done in thread.
}

void RemoteDebuggerPeerTCP::_poll() {
	tcp_client->poll();
	if (connected.is_set()) {
		_write_out();
		_read_in();
		if (tcp_client->get_status() != StreamPeerTCP::STATUS_CONNECTED) {
			MutexLock lock(mutex);
			connected.clear();
			out_queue.clear();
			out_queued_bytes = 0;
			out_left = 0;
			out_buf.clear();
		}
	}
}

Ref<RemoteDebuggerPeer> RemoteDebuggerPeerTCP::create_tcp(const String &p_uri) {
	ERR_FAIL_COND_V(!p_uri.begins_with("tcp://"), nullptr);

	String debug_host = p_uri.replace("tcp://", "");
	uint16_t debug_port = 6007;

	if (debug_host.contains_char(':')) {
		int sep_pos = debug_host.rfind_char(':');
		debug_port = debug_host.substr(sep_pos + 1).to_int();
		debug_host = debug_host.substr(0, sep_pos);
	}

	IPAddress ip;
	if (debug_host.is_valid_ip_address()) {
		ip = debug_host;
	} else {
		ip = IP::get_singleton()->resolve_hostname(debug_host);
	}

	Ref<StreamPeerTCP> stream;
	stream.instantiate();
	ERR_FAIL_COND_V_MSG(stream->connect_to_host(ip, debug_port) != OK, nullptr, vformat("Remote Debugger: Unable to connect to host '%s:%d'.", debug_host, debug_port));
	ERR_FAIL_COND_V(_try_connect(stream), nullptr);
	return memnew(RemoteDebuggerPeerTCP(stream));
}

Ref<RemoteDebuggerPeer> RemoteDebuggerPeerTCP::create_unix(const String &p_uri) {
	ERR_FAIL_COND_V(!p_uri.begins_with("unix://"), nullptr);

	String debug_path = p_uri.replace("unix://", "");
	Ref<StreamPeerUDS> stream;
	stream.instantiate();
	Error err = stream->connect_to_host(debug_path);
	ERR_FAIL_COND_V_MSG(err != OK && err != ERR_BUSY, nullptr, vformat("Remote Debugger: Unable to connect to socket path '%s'.", debug_path));
	ERR_FAIL_COND_V(_try_connect(stream), nullptr);
	return memnew(RemoteDebuggerPeerTCP(stream));
}

RemoteDebuggerPeer::RemoteDebuggerPeer() {
	max_queued_messages = (int)GLOBAL_GET("network/limits/debugger/max_queued_messages");
}

void RemoteDebuggerPeerTCP::_disconnect_with_error(const String &p_reason) {
	ERR_PRINT(p_reason);
	tcp_client->disconnect_from_host();
}
