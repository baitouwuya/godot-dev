/**************************************************************************/
/*  test_remote_debugger_peer.cpp                                         */
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

#include "core/config/project_settings.h"
#include "core/debugger/remote_debugger_peer.h"
#include "core/io/marshalls.h"
#include "core/io/stream_peer_socket.h"
#include "core/os/os.h"
#include "tests/test_macros.h"

TEST_FORCE_LINK(test_remote_debugger_peer);

#ifdef THREADS_ENABLED

namespace TestRemoteDebuggerPeer {

// Polling cannot reach the outgoing queue until writes are enabled. This makes
// producer mutation after put_message deterministic, without racing a Variant.
class GatedSocket : public NetSocket {
	GDSOFTCLASS(GatedSocket, NetSocket);

	SafeFlag opened = SafeFlag(true);
	SafeFlag writes_enabled;
	SafeFlag fail_writes;
	SafeNumeric<int> send_allowance = SafeNumeric<int>(INT_MAX);
	Mutex mutex;
	Vector<uint8_t> received;

public:
	void enable_writes() { writes_enabled.set(); }
	void fail_next_write() { fail_writes.set(); }
	void set_send_allowance(int p_bytes) { send_allowance.set(p_bytes); }
	Vector<uint8_t> get_received() {
		MutexLock lock(mutex);
		return received;
	}

	Error open(Family p_family, Type p_type, IP::Type &r_ip_type) override { return OK; }
	void close() override { opened.clear(); }
	Error bind(Address p_addr) override { return OK; }
	Error listen(int p_max_pending) override { return OK; }
	Error connect_to_host(Address p_addr) override { return OK; }
	Error poll(PollType p_type, int p_timeout) const override {
		if (!opened.is_set()) {
			return ERR_CONNECTION_ERROR;
		}
		if (p_type == POLL_TYPE_IN || (p_type == POLL_TYPE_OUT && (!writes_enabled.is_set() || send_allowance.get() == 0))) {
			return ERR_BUSY;
		}
		return OK;
	}
	Error recv(uint8_t *p_buffer, int p_len, int &r_read) override {
		r_read = 0;
		return ERR_BUSY;
	}
	Error recvfrom(uint8_t *p_buffer, int p_len, int &r_read, IPAddress &r_ip, uint16_t &r_port, bool p_peek) override { return ERR_UNAVAILABLE; }
	Error send(const uint8_t *p_buffer, int p_len, int &r_sent) override {
		r_sent = 0;
		if (fail_writes.is_set()) {
			return ERR_CONNECTION_ERROR;
		}
		MutexLock lock(mutex);
		r_sent = MIN(p_len, send_allowance.get());
		if (r_sent == 0) {
			return ERR_BUSY;
		}
		int offset = received.size();
		received.resize(offset + r_sent);
		memcpy(received.ptrw() + offset, p_buffer, r_sent);
		if (send_allowance.get() != INT_MAX) {
			send_allowance.sub(r_sent);
		}
		return OK;
	}
	Error sendto(const uint8_t *p_buffer, int p_len, int &r_sent, IPAddress p_ip, uint16_t p_port) override { return ERR_UNAVAILABLE; }
	Ref<NetSocket> accept(Address &r_addr) override { return Ref<NetSocket>(); }
	bool is_open() const override { return opened.is_set(); }
	int get_available_bytes() const override { return 0; }
	Error get_socket_address(Address *r_addr) const override { return OK; }
	Error set_broadcasting_enabled(bool p_enabled) override { return OK; }
	void set_blocking_enabled(bool p_enabled) override {}
	void set_ipv6_only_enabled(bool p_enabled) override {}
	void set_tcp_no_delay_enabled(bool p_enabled) override {}
	void set_reuse_address_enabled(bool p_enabled) override {}
	Error join_multicast_group(const IPAddress &p_address, const String &p_interface) override { return ERR_UNAVAILABLE; }
	Error leave_multicast_group(const IPAddress &p_address, const String &p_interface) override { return ERR_UNAVAILABLE; }
};

class TestStream : public StreamPeerSocket {
	GDSOFTCLASS(TestStream, StreamPeerSocket);

public:
	void accept_socket(Ref<NetSocket> p_socket, const NetSocket::Address &p_address) override {
		_sock = p_socket;
		status = STATUS_CONNECTED;
	}
};

class PeerFixture {
public:
	Ref<GatedSocket> socket;
	Ref<TestStream> stream;
	Ref<RemoteDebuggerPeerTCP> peer;

	PeerFixture(int p_max_messages = 4096) {
		socket.instantiate();
		stream.instantiate();
		stream->accept_socket(socket, NetSocket::Address());
		// The test runner does not initialize the runtime debugger settings.
		ProjectSettings *settings = ProjectSettings::get_singleton();
		const String setting = "network/limits/debugger/max_queued_messages";
		const bool had_setting = settings->has_setting(setting);
		const Variant previous = had_setting ? settings->get_setting(setting) : Variant();
		settings->set_setting(setting, p_max_messages);
		peer = memnew(RemoteDebuggerPeerTCP(stream));
		settings->set_setting(setting, previous);
	}

	bool wait_for_bytes(int p_bytes) {
		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
		while (OS::get_singleton()->get_ticks_msec() < deadline) {
			if (socket->get_received().size() >= p_bytes) {
				return true;
			}
			OS::get_singleton()->delay_usec(1000);
		}
		return false;
	}

	Vector<uint8_t> receive_packet() {
		socket->enable_writes();
		const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
		while (OS::get_singleton()->get_ticks_msec() < deadline) {
			Vector<uint8_t> bytes = socket->get_received();
			if (bytes.size() >= 4 && uint64_t(bytes.size()) >= uint64_t(decode_uint32(bytes.ptr())) + 4) {
				return bytes;
			}
			OS::get_singleton()->delay_usec(1000);
		}
		return socket->get_received();
	}
};

static Vector<uint8_t> encode_packet(const Array &p_message) {
	int size = 0;
	REQUIRE(encode_variant(p_message, nullptr, size) == OK);
	Vector<uint8_t> bytes;
	bytes.resize(size + 4);
	encode_uint32(size, bytes.ptrw());
	REQUIRE(encode_variant(p_message, bytes.ptrw() + 4, size) == OK);
	return bytes;
}

TEST_CASE("[RemoteDebuggerPeer] Message is owned at enqueue before producer clears it") {
	PeerFixture fixture;
	Array message;
	message.push_back("original");
	const Vector<uint8_t> expected = encode_packet(message);
	REQUIRE(fixture.peer->put_message(message) == OK);
	message.clear();
	CHECK(fixture.receive_packet() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Nested typed containers are owned at enqueue") {
	PeerFixture fixture;
	Array numbers;
	numbers.set_typed(Variant::INT, StringName(), Variant());
	numbers.push_back(7);
	numbers.push_back(11);
	Dictionary typed;
	typed.set_typed(Variant::STRING, StringName(), Variant(), Variant::ARRAY, StringName(), Variant());
	typed["numbers"] = numbers;
	Array key;
	key.push_back("container key");
	Dictionary nested;
	nested[key] = typed;
	nested["alias"] = numbers;
	Array message;
	message.push_back(nested);
	const Vector<uint8_t> expected = encode_packet(message);
	REQUIRE(fixture.peer->put_message(message) == OK);
	numbers.resize(100);
	numbers[0] = 99;
	typed.erase("numbers");
	nested.clear();
	key.clear();
	CHECK(fixture.receive_packet() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Object IDs are encoded before producer deletes objects") {
	PeerFixture fixture;
	Object *object = memnew(Object);
	Array message;
	message.push_back(object);
	const Vector<uint8_t> expected = encode_packet(message);
	const Error error = fixture.peer->put_message(message);
	memdelete(object);
	REQUIRE(error == OK);
	CHECK(fixture.receive_packet() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Recursive messages fail at enqueue without poisoning the queue") {
	PeerFixture fixture;
	Array recursive;
	recursive.push_back(recursive);
	ERR_PRINT_OFF;
	const Error error = fixture.peer->put_message(recursive);
	ERR_PRINT_ON;
	// Break the cycle even on the old implementation, to avoid leaks.
	recursive.clear();
	CHECK(error == ERR_OUT_OF_MEMORY);
	Array message;
	message.push_back("after rejected packet");
	const Vector<uint8_t> expected = encode_packet(message);
	REQUIRE(fixture.peer->put_message(message) == OK);
	CHECK(fixture.receive_packet() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Oversize messages fail at enqueue") {
	PeerFixture fixture;
	Vector<uint8_t> payload;
	payload.resize(fixture.peer->get_max_message_size());
	Array message;
	message.push_back(payload);
	CHECK(fixture.peer->put_message(message) == ERR_OUT_OF_MEMORY);
}

TEST_CASE("[RemoteDebuggerPeer] Repeated nested messages retain their enqueue generations") {
	PeerFixture fixture;
	Array values;
	values.push_back(0);
	Dictionary state;
	state["values"] = values;
	Array message;
	message.push_back(state);
	Vector<uint8_t> expected;
	for (int generation = 0; generation < 128; generation++) {
		values[0] = generation;
		expected.append_array(encode_packet(message));
		REQUIRE(fixture.peer->put_message(message) == OK);
	}
	state.clear();
	values.clear();
	fixture.socket->enable_writes();
	REQUIRE(fixture.wait_for_bytes(expected.size()));
	CHECK(fixture.socket->get_received() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Partial writes preserve owned packet bytes") {
	PeerFixture fixture;
	Array message;
	message.push_back("snapshot across partial writes");
	const Vector<uint8_t> expected = encode_packet(message);
	fixture.socket->set_send_allowance(3);
	REQUIRE(fixture.peer->put_message(message) == OK);
	fixture.socket->enable_writes();
	REQUIRE(fixture.wait_for_bytes(3));
	message.clear();
	CHECK(fixture.socket->get_received().size() == 3);
	fixture.socket->set_send_allowance(INT_MAX);
	CHECK(fixture.receive_packet() == expected);
}

TEST_CASE("[RemoteDebuggerPeer] Exact frame limit and aggregate byte budget include in-flight bytes") {
	PeerFixture fixture;
	const int frame_size = fixture.peer->get_max_message_size();
	Vector<uint8_t> payload;
	// Array and PackedByteArray headers plus the transport prefix occupy 20 bytes.
	payload.resize(frame_size - 20);
	Array message;
	message.push_back(payload);
	for (int i = 0; i < 4; i++) {
		REQUIRE(fixture.peer->put_message(message) == OK);
	}
	CHECK(fixture.peer->put_message(message) == ERR_OUT_OF_MEMORY);
	fixture.socket->set_send_allowance(frame_size - 1);
	fixture.socket->enable_writes();
	REQUIRE(fixture.wait_for_bytes(frame_size - 1));
	// Dequeuing and partly writing the first packet must not release its budget.
	CHECK(fixture.peer->put_message(message) == ERR_OUT_OF_MEMORY);
	fixture.socket->set_send_allowance(1);
	REQUIRE(fixture.wait_for_bytes(frame_size));
	// The socket signals receipt just before the worker releases the byte budget.
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	Error error = ERR_OUT_OF_MEMORY;
	while (error == ERR_OUT_OF_MEMORY && OS::get_singleton()->get_ticks_msec() < deadline) {
		error = fixture.peer->put_message(message);
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK(error == OK);
	fixture.peer->close();
	CHECK_FALSE(fixture.peer->is_peer_connected());
	CHECK(fixture.peer->put_message(message) == ERR_UNCONFIGURED);
}

TEST_CASE("[RemoteDebuggerPeer] Message count limit is retained") {
	PeerFixture fixture(2);
	Array message;
	message.push_back(42);
	CHECK(fixture.peer->put_message(message) == OK);
	CHECK(fixture.peer->put_message(message) == OK);
	CHECK(fixture.peer->put_message(message) == ERR_OUT_OF_MEMORY);
}

TEST_CASE("[RemoteDebuggerPeer] Write errors disconnect and discard queued packets") {
	PeerFixture fixture;
	Array message;
	message.push_back("write failure");
	REQUIRE(fixture.peer->put_message(message) == OK);
	REQUIRE(fixture.peer->put_message(message) == OK);
	fixture.socket->fail_next_write();
	ERR_PRINT_OFF;
	fixture.socket->enable_writes();
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	while (fixture.peer->is_peer_connected() && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	ERR_PRINT_ON;
	CHECK_FALSE(fixture.peer->is_peer_connected());
	CHECK(fixture.peer->put_message(message) == ERR_UNCONFIGURED);
	fixture.peer->close();
	CHECK(fixture.socket->get_received().is_empty());
}

TEST_CASE("[RemoteDebuggerPeer] Disconnect and repeated close safely discard queued bytes") {
	PeerFixture fixture;
	Array message;
	message.push_back("queued before disconnect");
	REQUIRE(fixture.peer->put_message(message) == OK);
	fixture.socket->close();
	const uint64_t deadline = OS::get_singleton()->get_ticks_msec() + 2000;
	while (fixture.peer->is_peer_connected() && OS::get_singleton()->get_ticks_msec() < deadline) {
		OS::get_singleton()->delay_usec(1000);
	}
	CHECK_FALSE(fixture.peer->is_peer_connected());
	CHECK(fixture.peer->put_message(message) == ERR_UNCONFIGURED);
	fixture.peer->close();
	fixture.peer->close();
	CHECK(fixture.socket->get_received().is_empty());

	ProjectSettings *settings = ProjectSettings::get_singleton();
	const String setting = "network/limits/debugger/max_queued_messages";
	const Variant previous = settings->get_setting(setting, Variant());
	settings->set_setting(setting, 4096);
	Ref<RemoteDebuggerPeerTCP> unconnected;
	unconnected.instantiate();
	settings->set_setting(setting, previous);
	CHECK(unconnected->put_message(message) == ERR_UNCONFIGURED);
	unconnected->close();
}

} // namespace TestRemoteDebuggerPeer

#endif // THREADS_ENABLED
