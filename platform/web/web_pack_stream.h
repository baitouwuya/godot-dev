#pragma once

#include "core/io/file_access.h"

class Resource;

namespace WebPackStream {

struct PartialFile {
	int entry = -1;
	uint64_t head_end = 0;
	uint64_t tail_start = 0;
	explicit operator bool() const { return entry >= 0; }
};

using Upgrade = void (*)(const String &p_path, Resource &r_texture);

void initialize();
void mount_partial_files();
// A ready middle is reserved for the upgrade decoder, not ordinary loads.
PartialFile query_partial(const Ref<FileAccess> &p_file);
void register_texture_upgrade(PartialFile p_file, const String &p_path, Resource &r_texture, Upgrade p_upgrade);
void poll_upgrades(uint64_t p_budget_usec);
void clear_upgrades();

} // namespace WebPackStream
