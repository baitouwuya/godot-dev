#include "glslang_host.h"
#include "tint_wrapper.h"

#include <json/json.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using namespace godot::webgpu;
namespace fs = std::filesystem;

constexpr std::array<const char *, SHADER_STAGE_COUNT> stage_names{ "vertex", "fragment", "compute" };
constexpr const char *role_names[] = { "texture", "sampler", "storage_buffer", "storage_texture", "uniform_buffer", "push_constant" };

std::optional<Stage> stage_of(const std::string &p_name) {
	auto it = std::find(stage_names.begin(), stage_names.end(), p_name);
	return it == stage_names.end() ? std::optional<Stage>{} : std::optional<Stage>{ static_cast<Stage>(it - stage_names.begin()) };
}

std::string byte_hash(const std::string &p_bytes) {
	std::array<unsigned char, 32> digest{};
	if (mbedtls_sha256(reinterpret_cast<const unsigned char *>(p_bytes.data()), p_bytes.size(), digest.data(), 0) != 0) {
		throw std::runtime_error("SHA-256 computation failed");
	}
	constexpr char hex[] = "0123456789abcdef";
	std::string output;
	for (unsigned char byte : digest) {
		output.push_back(hex[byte >> 4]);
		output.push_back(hex[byte & 15]);
	}
	return output;
}

std::string encode_words(const std::vector<uint32_t> &p_words) {
	std::string bytes;
	bytes.reserve(p_words.size() * SPIRV_WORD_BYTES);
	for (auto word : p_words) {
		for (uint32_t i = 0; i < SPIRV_WORD_BYTES; ++i) {
			bytes.push_back(static_cast<char>(word >> (i * 8)));
		}
	}
	return bytes;
}

bool read_file(const fs::path &p_path, std::string &r_bytes) {
	std::ifstream file(p_path, std::ios::binary);
	if (!file) {
		return false;
	}
	r_bytes.assign(std::istreambuf_iterator<char>(file), {});
	return !file.bad();
}

uint32_t required_u32(const Json::Value &p_value, const char *p_name) {
	if (!p_value.isUInt()) {
		throw std::runtime_error(std::string("Expected uint32 for ") + p_name);
	}
	return p_value.asUInt();
}

BindingKey read_key(const Json::Value &p_value, Role p_role) {
	return { required_u32(p_value["set"], "set"), required_u32(p_value["binding"], "binding"),
			p_value.get("element", 0u).asUInt(), p_role };
}

CompileOptions read_options(const Json::Value &p_value) {
	CompileOptions options;
	options.uniform_memory_fences = p_value.get("uniform_memory_fences", false).asBool();
	options.texture_barrier = p_value.get("texture_barrier", false).asBool();
	options.promote_narrow_storage = p_value.get("promote_narrow_storage", true).asBool();
	options.non_uniform_derivatives = p_value.get("non_uniform_derivatives", false).asBool();
	options.flip_vertex_y = p_value.get("flip_vertex_y", true).asBool();
	const std::string subgroup = p_value.get("subgroup_lowering", "none").asString();
	if (subgroup == "clustered_scene") {
		options.subgroup_lowering = SubgroupLowering::CLUSTERED_SCENE;
	} else if (subgroup == "cluster_raster") {
		options.subgroup_lowering = SubgroupLowering::CLUSTER_RASTER;
	} else if (subgroup != "none") {
		throw std::runtime_error("Unknown subgroup lowering profile: " + subgroup);
	}
	for (const auto &item : p_value["specialization"]) {
		options.specialization.emplace(required_u32(item["id"], "id"), required_u32(item["bits"], "bits"));
	}
	for (const auto &item : p_value["immutable_samplers"]) {
		options.immutable_samplers.emplace(read_key(item, Role::SAMPLER), required_u32(item["descriptor"], "descriptor"));
	}
	for (const auto &item : p_value["raw_depth"]) {
		options.raw_depth.push_back(read_key(item, Role::TEXTURE));
	}
	for (const auto &item : p_value["unfilterable_float"]) {
		options.unfilterable_float.push_back(read_key(item, Role::TEXTURE));
	}
	for (const auto &item : p_value["storage_formats"]) {
		options.storage_formats.emplace(read_key(item, Role::STORAGE_TEXTURE), required_u32(item["format"], "format"));
	}
	for (const auto &item : p_value["atomic_images"]) {
		options.atomic_images.emplace(read_key(item, Role::STORAGE_TEXTURE), required_u32(item["format"], "format"));
	}
	return options;
}

Json::Value serialize_key(const BindingKey &p_key) {
	Json::Value value(Json::objectValue);
	value["set"] = p_key.set;
	value["binding"] = p_key.binding;
	value["element"] = p_key.element;
	value["role"] = role_names[static_cast<unsigned>(p_key.role)];
	return value;
}

Json::Value serialize_plan(const ShaderPlan &p_plan) {
	Json::Value value(Json::objectValue);
	value["bindings"] = Json::Value(Json::arrayValue);
	for (const BindingPlan &binding : p_plan.bindings) {
		Json::Value item = serialize_key(binding.key);
		item["group"] = binding.group;
		item["slot"] = binding.binding;
		item["stages"] = binding.stages;
		item["read_only"] = binding.read_only;
		item["sampler_type"] = static_cast<unsigned>(binding.sampler_type);
		item["aliases"] = Json::Value(Json::arrayValue);
		for (const BindingKey &key : binding.aliases) {
			item["aliases"].append(serialize_key(key));
		}
		const FormatPlan &format = binding.format;
		auto &out = item["format"];
		out["logical"] = format.logical_format;
		out["physical"] = format.physical_format;
		out["dimension"] = format.dimension;
		out["arrayed"] = format.arrayed;
		out["multisampled"] = format.multisampled;
		out["depth_only_view"] = format.depth_only_view;
		out["access"] = static_cast<unsigned>(format.access);
		out["sample_type"] = static_cast<unsigned>(format.sample_type);
		out["conversion"] = static_cast<unsigned>(format.conversion);
		out["bits"] = format.component_bits;
		out["channels"] = format.channels;
		out["atomic_buffer"] = format.atomic_buffer;
		value["bindings"].append(std::move(item));
	}
	for (size_t i = 0; i < stage_names.size(); ++i) {
		const StageLimits &stage = p_plan.limits[i];
		auto &limits = value["limits"][stage_names[i]];
		limits["T"] = stage.sampled_textures;
		limits["S"] = stage.samplers;
		limits["B"] = stage.storage_buffers;
		limits["I"] = stage.storage_textures;
		limits["U"] = stage.uniform_buffers;
	}
	auto &pc = value["push_constant"];
	pc["size"] = p_plan.push_constant.size;
	pc["stages"] = p_plan.push_constant.stages;
	pc["group"] = p_plan.push_constant.group;
	pc["slot"] = p_plan.push_constant.binding;
	value["uses_texture_barrier"] = p_plan.uses_texture_barrier;
	return value;
}

bool load_stage(const Json::Value &p_source, const fs::path &p_dir, StageInput &r_input, Json::Value &r_meta, std::string &r_error) {
	auto stage = stage_of(p_source["stage"].asString());
	if (!stage) {
		r_error = "Unknown shader stage: " + p_source["stage"].asString();
		return false;
	}
	r_input.stage = *stage;
	const std::string kind = p_source.get("kind", "glsl").asString();
	std::string source;
	if (p_source.isMember("file")) {
		const auto path = p_source["file"].asString();
		if (!read_file(p_dir / fs::u8path(path), source)) {
			r_error = "Cannot read " + path;
			return false;
		}
	} else if (kind == "glsl" && p_source["source"].isString()) {
		source = p_source["source"].asString();
	} else {
		r_error = "A stage requires a file or inline GLSL source";
		return false;
	}
	if (kind == "glsl") {
		if (source.find('\0') != std::string::npos) {
			r_error = "Embedded NUL in GLSL source";
			return false;
		}
		if (!host::compile_glsl(*stage, source, p_source.get("vulkan_version", SPIRV_VULKAN_1_1).asUInt(),
					p_source.get("spirv_version", SPIRV_VERSION_1_3).asUInt(), p_source.get("debug", false).asBool(), r_input.spirv, r_error)) {
			return false;
		}
	} else if (kind == "spv" && !source.empty() && source.size() % SPIRV_WORD_BYTES == 0) {
		for (size_t i = 0; i < source.size(); i += SPIRV_WORD_BYTES) {
			uint32_t word = 0;
			for (uint32_t j = 0; j < SPIRV_WORD_BYTES; ++j) {
				word |= uint32_t(static_cast<unsigned char>(source[i + j])) << (8 * j);
			}
			r_input.spirv.push_back(word);
		}
	} else {
		r_error = "Expected GLSL or word-aligned little-endian SPIR-V";
		return false;
	}
	const std::string hash = byte_hash(encode_words(r_input.spirv));
	r_meta["sha256"] = hash;
	r_meta["hash64"] = hash.substr(0, 16);
	return true;
}

Json::Value run_job(const Json::Value &p_job, const fs::path &p_dir) {
	Json::Value record(Json::objectValue);
	record["id"] = p_job["id"];
	record["ok"] = false;
	try {
		if (!p_job["id"].isString() || !p_job["stages"].isArray() || p_job["stages"].empty()) {
			throw std::runtime_error("A job requires a string id and nonempty stages array");
		}
		std::vector<StageInput> inputs;
		for (const Json::Value &source : p_job["stages"]) {
			std::string error;
			const auto name = source["stage"].asString();
			StageInput input{};
			if (!load_stage(source, p_dir, input, record["stages"][name], error)) {
				throw std::runtime_error(name + ": " + error);
			}
			inputs.push_back(std::move(input));
		}
		if (!p_job.get("compile_only", false).asBool()) {
			ShaderPlan plan = compile_shader(inputs, read_options(p_job["options"]));
			if (!plan.ok()) {
				throw std::runtime_error(plan.diagnostic);
			}
			record["plan"] = serialize_plan(plan);
			for (auto &stage : plan.stages) {
				auto &output = record["stages"][stage_names[static_cast<size_t>(stage.stage)]];
				output["entry_point"] = stage.entry_point;
				output["wgsl_sha256"] = byte_hash(stage.wgsl);
				output["wgsl"] = std::move(stage.wgsl);
			}
		}
		record["ok"] = true;
	} catch (const std::exception &e) {
		record["diagnostic"] = e.what();
	}
	return record;
}

} // namespace

int main(int argc, char **argv) {
#ifdef _WIN32
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
	if (argc != 5 || std::string(argv[1]) != "--manifest" || std::string(argv[3]) != "--out") {
		std::cerr << "Usage: tint_convert_cli --manifest input.json --out result.json\n";
		return 1;
	}
	const fs::path manifest = fs::u8path(argv[2]);
	std::ifstream file(manifest, std::ios::binary);
	Json::Value request;
	Json::CharReaderBuilder reader;
	reader["rejectDupKeys"] = true;
	reader["failIfExtra"] = true;
	std::string errors;
	if (!file || !Json::parseFromStream(reader, file, &request, &errors) || !request["jobs"].isArray() || request["jobs"].empty()) {
		std::cerr << "Invalid manifest (expected nonempty jobs array): " << errors << '\n';
		return 1;
	}
	Json::Value response(Json::objectValue);
	response["schema"] = SHADER_PLAN_VERSION;
	response["jobs"] = Json::Value(Json::arrayValue);
	bool success = true;
	for (const auto &job : request["jobs"]) {
		Json::Value result = run_job(job, manifest.parent_path());
		success &= result["ok"].asBool();
		response["jobs"].append(std::move(result));
	}
	std::ofstream out(fs::u8path(argv[4]), std::ios::binary);
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	out << Json::writeString(writer, response) << '\n';
	out.close();
	if (!out) {
		std::cerr << "Could not write result manifest\n";
		return 1;
	}
	return success ? 0 : 2;
}
