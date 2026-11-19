#include "web_pack_stream.h"

#include "godot_js.h"

#include "core/io/file_access_pack.h"
#include "core/io/resource.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "drivers/unix/file_access_unix.h"

#include <cstring>
#include <deque>
#include <unordered_map>

namespace WebPackStream {
namespace {

String pack_path;
uint64_t pack_size = 0;

class FileAccessWebStream final : public FileAccessUnix {
	GDSOFTCLASS(FileAccessWebStream, FileAccessUnix);

	String path;
	uint64_t offset = 0;
	uint64_t length = 0;
	mutable uint64_t position = 0;
	mutable Error error = OK;
	PartialFile partial;

public:
	FileAccessWebStream() = default;
	FileAccessWebStream(const String &p_path, uint64_t p_offset, uint64_t p_length, PartialFile p_partial) :
			path(p_path), offset(p_offset), length(p_length), partial(p_partial) {}

	PartialFile get_partial_file() const { return partial; }

	Error open_internal(const String &p_path, int p_flags) override {
		close();
		if (p_path != pack_path) {
			return FileAccessUnix::open_internal(p_path, p_flags);
		}
		ERR_FAIL_COND_V(p_flags & WRITE, ERR_UNAVAILABLE);
		path = p_path;
		length = pack_size;
		return OK;
	}
	void close() override {
		if (path.is_empty()) {
			FileAccessUnix::close();
		} else {
			path.clear();
			offset = length = position = 0;
			error = OK;
			partial = {};
		}
	}
	bool is_open() const override { return !path.is_empty() || FileAccessUnix::is_open(); }
	String get_path() const override { return path.is_empty() ? FileAccessUnix::get_path() : path; }
	String get_path_absolute() const override { return path.is_empty() ? FileAccessUnix::get_path_absolute() : path; }
	bool file_exists(const String &p_path) override { return p_path == pack_path || FileAccessUnix::file_exists(p_path); }
	int64_t _get_size(const String &p_path) override { return p_path == pack_path ? int64_t(pack_size) : FileAccessUnix::_get_size(p_path); }
	bool _get_read_only_attribute(const String &p_path) override { return p_path == pack_path || FileAccessUnix::_get_read_only_attribute(p_path); }
	void seek(uint64_t p_position) override {
		if (path.is_empty()) {
			FileAccessUnix::seek(p_position);
		} else {
			position = p_position;
			error = position > length ? ERR_FILE_EOF : OK;
		}
	}
	void seek_end(int64_t p_position) override {
		if (path.is_empty()) {
			FileAccessUnix::seek_end(p_position);
		} else {
			seek(p_position < -int64_t(length) ? 0 : length + p_position);
		}
	}
	uint64_t get_position() const override { return path.is_empty() ? FileAccessUnix::get_position() : position; }
	uint64_t get_length() const override { return path.is_empty() ? FileAccessUnix::get_length() : length; }
	bool eof_reached() const override { return path.is_empty() ? FileAccessUnix::eof_reached() : error == ERR_FILE_EOF; }
	Error get_error() const override { return path.is_empty() ? FileAccessUnix::get_error() : error; }

	uint64_t get_buffer(uint8_t *p_dst, uint64_t p_length) const override {
		if (path.is_empty()) {
			return FileAccessUnix::get_buffer(p_dst, p_length);
		}
		const uint64_t readable = position < length ? MIN(p_length, length - position) : 0;
		ERR_FAIL_COND_V(readable > UINT32_MAX, 0);
		if (readable && !godot_js_pck_read(double(offset + position), p_dst, uint32_t(readable))) {
			error = ERR_UNAVAILABLE;
			ERR_FAIL_V_MSG(0, vformat("Web pack bytes are not resident for '%s' at offset %d.", path, position));
		}
		position += readable;
		error = readable < p_length ? ERR_FILE_EOF : OK;
		return readable;
	}

	Error resize(int64_t p_length) override { return path.is_empty() ? FileAccessUnix::resize(p_length) : ERR_UNAVAILABLE; }
	bool store_buffer(const uint8_t *p_src, uint64_t p_length) override {
		ERR_FAIL_COND_V_MSG(!path.is_empty(), false, "Web pack files are read-only.");
		return FileAccessUnix::store_buffer(p_src, p_length);
	}
	void flush() override {
		ERR_FAIL_COND_MSG(!path.is_empty(), "Web pack files are read-only.");
		FileAccessUnix::flush();
	}
};

#ifndef THREADS_ENABLED
class PackSourceWebStream final : public PackSource {
	HashMap<uint64_t, PartialFile> files;

public:
	bool try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &) override {
		if (p_path != pack_path || p_offset != 0) {
			return false;
		}
		PackedData *packed = PackedData::get_singleton();
		for (int entry = 0;; ++entry) {
			const int path_size = godot_js_pck_entry_path_size(entry);
			if (path_size == 0) {
				break;
			}
			Vector<char> path_utf8;
			ERR_FAIL_COND_V(path_utf8.resize(path_size) != OK, false);
			double ranges[4];
			uint8_t md5[16];
			godot_js_pck_get_entry(entry, path_utf8.ptrw(), path_size, ranges, md5);
			const String path = String::utf8(path_utf8.ptr());
			const uint64_t offset = uint64_t(ranges[0]);
			const uint64_t size = uint64_t(ranges[1]);
			const uint8_t *mounted_md5 = packed->get_file_hash(path);
			// Preserve the result of any higher-priority pack or patch mounted during setup.
			if (mounted_md5 && packed->get_size(path) == int64_t(size) && !packed->has_delta_patches(path) && std::memcmp(mounted_md5, md5, sizeof(md5)) == 0) {
				files.insert(offset, { entry, uint64_t(ranges[2]), uint64_t(ranges[3]) });
				packed->add_path(p_path, path, offset, size, md5, this, p_replace_files);
			}
		}
		return true;
	}

	Ref<FileAccess> get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &) override {
		ERR_FAIL_COND_V_MSG(PackedData::get_singleton()->has_delta_patches(p_path), Ref<FileAccess>(),
				vformat("Runtime delta patches require fully resident texture bytes for '%s'. Disable progressiveTextureMips.", p_path));
		return memnew(FileAccessWebStream(p_path, p_file->offset, p_file->size, files.get(p_file->offset)));
	}
};

struct PendingUpgrade {
	ObjectID id;
	String path;
	Upgrade apply;
};

std::unordered_map<int, std::deque<PendingUpgrade>> upgrades;
int ready_entry = -1;
bool ready_success = false;
int decoding_entry = -1;

void cancel_upgrade(int p_entry, uint64_t p_id) {
	auto &pending = upgrades.at(p_entry);
	for (auto it = pending.begin(); it != pending.end(); ++it) {
		if (it->id == ObjectID(p_id)) {
			pending.erase(it);
			break;
		}
	}
}

Callable upgrade_cancellation(int p_entry, ObjectID p_id) {
	return callable_mp_static(&cancel_upgrade).bind(p_entry, int64_t(uint64_t(p_id)));
}
#endif

} // namespace

void initialize() {
	pack_size = uint64_t(godot_js_pck_get_size());
	if (pack_size > 0) {
		const int path_size = godot_js_pck_path_size();
		Vector<char> path_utf8;
		ERR_FAIL_COND(path_utf8.resize(path_size) != OK);
		godot_js_pck_get_path(path_utf8.ptrw(), path_size);
		pack_path = String::utf8(path_utf8.ptr());
		FileAccess::make_default<FileAccessWebStream>(FileAccess::ACCESS_FILESYSTEM);
	}
}

void mount_partial_files() {
#ifndef THREADS_ENABLED
	if (godot_js_pck_entry_path_size(0) > 0) {
		// Main::setup has created PackedData and the native parser has mounted the base pack.
		PackSourceWebStream *source = memnew(PackSourceWebStream);
		PackedData::get_singleton()->add_pack_source(source);
		source->try_open_pack(pack_path, true, 0, {});
	}
#endif
}

PartialFile query_partial(const Ref<FileAccess> &p_file) {
#ifndef THREADS_ENABLED
	if (const auto *stream = Object::cast_to<FileAccessWebStream>(p_file.ptr())) {
		const PartialFile partial = stream->get_partial_file();
		if (partial.entry != decoding_entry) {
			return partial;
		}
	}
#endif
	return {};
}

void register_texture_upgrade(PartialFile p_file, const String &p_path, Resource &r_texture, Upgrade p_upgrade) {
#ifndef THREADS_ENABLED
	const ObjectID id = r_texture.get_instance_id();
	const auto current = upgrades.find(p_file.entry);
	if (current != upgrades.end()) {
		for (const auto &upgrade : current->second) {
			if (upgrade.id == id) {
				return; // A reentrant load registered the same texture already.
			}
		}
	}
	const Error error = r_texture.connect(SNAME("changed"), upgrade_cancellation(p_file.entry, id), Object::CONNECT_ONE_SHOT);
	ERR_FAIL_COND(error != OK);
	upgrades[p_file.entry].push_back({ id, p_path, p_upgrade });
	godot_js_pck_request_full(p_file.entry);
#endif
}

void poll_upgrades(uint64_t p_budget_usec) {
#ifndef THREADS_ENABLED
	if (upgrades.empty()) {
		return;
	}
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	do {
		if (ready_entry < 0) {
			int success;
			ready_entry = godot_js_pck_poll_ready(&success);
			if (ready_entry < 0) {
				return;
			}
			ready_success = success != 0;
		}
		auto &pending = upgrades.at(ready_entry);
		if (!pending.empty()) {
			PendingUpgrade upgrade = std::move(pending.front());
			pending.pop_front();
			// Progressive residency is only enabled in no-threads builds. Pin the
			// live object while applying the change, including its synchronous signals.
			Ref<Resource> resource(Object::cast_to<Resource>(ObjectDB::get_instance(upgrade.id)));
			if (resource.is_valid()) {
				resource->disconnect(SNAME("changed"), upgrade_cancellation(ready_entry, upgrade.id));
				if (ready_success) {
					const Ref<FileAccess> file = FileAccess::open(upgrade.path, FileAccess::READ);
					if (query_partial(file).entry == ready_entry) {
						decoding_entry = ready_entry;
						upgrade.apply(upgrade.path, **resource);
						decoding_entry = -1;
					}
				}
			}
		}
		if (pending.empty()) {
			upgrades.erase(ready_entry);
			godot_js_pck_evict_middle(ready_entry);
			ready_entry = -1;
		}
	} while (OS::get_singleton()->get_ticks_usec() - start < p_budget_usec);
#endif
}

void clear_upgrades() {
#ifndef THREADS_ENABLED
	for (const auto &[entry, pending] : upgrades) {
		for (const auto &upgrade : pending) {
			if (Object *resource = ObjectDB::get_instance(upgrade.id)) {
				resource->disconnect(SNAME("changed"), upgrade_cancellation(entry, upgrade.id));
			}
		}
	}
	upgrades.clear();
	ready_entry = -1;
#endif
}

} // namespace WebPackStream
