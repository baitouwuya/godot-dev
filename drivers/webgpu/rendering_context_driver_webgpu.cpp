#include "rendering_context_driver_webgpu.h"

#include "rendering_device_driver_webgpu.h"
#include "webgpu_perf_stats.h"

#include <array>
#include <emscripten.h>
#include <type_traits>

static_assert(!std::is_abstract_v<RenderingDeviceDriverWebGPU>);

extern "C" int godot_webgpu_has_bootstrap_device();
extern "C" int godot_webgpu_has_texture_formats_tier2();

namespace godot::webgpu {

struct DeviceState::Completion {
	DeviceLease state;
	SubmitSerial serial;
	struct Timed;
	void finish(WGPUQueueWorkDoneStatus p_status) const;
};

struct DeviceState::Completion::Timed {
	Completion completion;
	double overflow_ms;
	struct Stamp {
		double start_ms;
		bool occupied;
	};
	static inline std::array<Stamp, 16> stamps{};

	void stamp_submit(WGPUQueueWorkDoneCallbackInfo &r_info) {
		Stamp &slot = stamps[completion.serial % stamps.size()];
		r_info.callback = work_done;
		r_info.userdata1 = this;
		// Bind this callback to its slot. A colliding serial keeps time in its own ticket.
		if (slot.occupied) {
			overflow_ms = emscripten_get_now();
		} else {
			r_info.userdata2 = &slot;
			slot = { emscripten_get_now(), true };
		}
	}

	static void work_done(WGPUQueueWorkDoneStatus p_status, void *p_userdata, void *p_slot) {
		std::unique_ptr<Timed> ticket(static_cast<Timed *>(p_userdata));
		ticket->completion.state->assert_owner();
		Stamp *slot = static_cast<Stamp *>(p_slot);
		if (p_status == WGPUQueueWorkDoneStatus_Success) {
			webgpu_perf_on_work_done(emscripten_get_now() - (slot ? slot->start_ms : ticket->overflow_ms));
		}
		if (slot) {
			slot->occupied = false;
		}
		ticket->completion.finish(p_status);
	}
};

static void print_perf_caps(const DeviceState &p_state) {
	const auto has = [&](WGPUFeatureName p_feature) { return wgpuDeviceHasFeature(p_state.device.get(), p_feature) ? "true" : "false"; };
	// Neither swizzle nor formats-tier2 has a WGPUFeatureName in the pinned emdawnwebgpu port.
	const bool swizzle = EM_ASM_INT({ return Module['preinitializedWebGPUDevice']['features']['has']('texture-component-swizzle'); });
	std::printf("[WebGPU-Caps] {\"texture-compression-bc\":%s,\"texture-component-swizzle\":%s,\"timestamp-query\":%s,"
			"\"texture-formats-tier2\":%s,\"depth-clip-control\":%s,\"subgroups\":%s,\"float32-filterable\":%s,"
			"\"maxStorageTexturesPerShaderStage\":%" PRIu32 ",\"maxBindGroups\":%" PRIu32 ",\"maxBindGroupsPlusVertexBuffers\":%" PRIu32 "}\n",
			has(WGPUFeatureName_TextureCompressionBC), swizzle ? "true" : "false", has(WGPUFeatureName_TimestampQuery),
			p_state.has_texture_formats_tier2 ? "true" : "false", has(WGPUFeatureName_DepthClipControl), has(WGPUFeatureName_Subgroups), has(WGPUFeatureName_Float32Filterable),
			p_state.limits.maxStorageTexturesPerShaderStage, p_state.limits.maxBindGroups, p_state.limits.maxBindGroupsPlusVertexBuffers);
}

DeviceLease DeviceState::acquire() {
	ERR_FAIL_COND_V_MSG(!Thread::is_main_thread(), {}, "WebGPU device acquisition must run on the browser owner thread.");
	static std::weak_ptr<DeviceState> active;
	if (auto current = active.lock()) {
		return current->failed() ? DeviceLease{} : current;
	}
	ERR_FAIL_COND_V_MSG(!godot_webgpu_has_bootstrap_device(), {}, "No preinitialized WebGPU device.");
	auto state = std::make_shared<DeviceState>();
	state->instance.reset(wgpuCreateInstance(nullptr));
	state->device.reset(emscripten_webgpu_get_device());
	ERR_FAIL_COND_V(!state->instance || !state->device, {});
	state->queue.reset(wgpuDeviceGetQueue(state->device.get()));
	ERR_FAIL_COND_V(!state->queue, {});
	ERR_FAIL_COND_V(wgpuDeviceGetLimits(state->device.get(), &state->limits) != WGPUStatus_Success, {});
	WGPUSupportedFeatures supported{};
	wgpuDeviceGetFeatures(state->device.get(), &supported);
	for (size_t i = 0; i < supported.featureCount; ++i) {
		state->features.insert(supported.features[i]);
	}
	wgpuSupportedFeaturesFreeMembers(supported);
	state->has_read_write_storage_textures = wgpuInstanceHasWGSLLanguageFeature(state->instance.get(), WGPUWGSLLanguageFeatureName_ReadonlyAndReadwriteStorageTextures);
	state->has_texture_formats_tier2 = godot_webgpu_has_texture_formats_tier2();
	WGPUAdapterInfo info = WGPU_ADAPTER_INFO_INIT;
	if (wgpuDeviceGetAdapterInfo(state->device.get(), &info) == WGPUStatus_Success) {
		if (info.description.data && info.description.length) {
			state->description.name = String::utf8(info.description.data, info.description.length);
		}
		state->description.vendor = info.vendorID;
		switch (info.adapterType) {
			case WGPUAdapterType_DiscreteGPU:
				state->description.type = RenderingContextDriver::DEVICE_TYPE_DISCRETE_GPU;
				break;
			case WGPUAdapterType_IntegratedGPU:
				state->description.type = RenderingContextDriver::DEVICE_TYPE_INTEGRATED_GPU;
				break;
			case WGPUAdapterType_CPU:
				state->description.type = RenderingContextDriver::DEVICE_TYPE_CPU;
				break;
			default:
				break;
		}
		wgpuAdapterInfoFreeMembers(info);
	}
	active = state;
	if (webgpu_perf_is_enabled()) {
		webgpu_perf_install_raf_hooks();
		print_perf_caps(*state);
	}
	return state;
}

DeviceState::~DeviceState() {
	assert_owner();
	retired.clear(); // Release wrappers before releasing the queue/device; never Destroy an imported GPUDevice.
}

void DeviceState::retire(SubmitSerial p_serial, std::shared_ptr<const void> p_object) {
	assert_owner();
	DEV_ASSERT(p_serial <= submit_serial);
	if (p_serial > completed_serial) {
		retired.emplace(p_serial, std::move(p_object));
	}
}

SubmitSerial DeviceState::submit(const std::vector<WGPUCommandBuffer> &p_commands, UseList p_uses) {
	assert_owner();
	ERR_FAIL_COND_V_MSG(queue_failed, 0, "The WebGPU queue has failed.");
	const SubmitSerial serial = ++submit_serial;
	for (const auto &resource : p_uses) {
		if (resource->last_use_serial != serial) {
			resource->last_use_serial = serial;
			retire(serial, resource);
		}
	}
	WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
	info.mode = WGPUCallbackMode_AllowSpontaneous;
	if (webgpu_perf_stats.armed) {
		// Extend the already-required callback allocation; the ring itself never owns a lease.
		auto *ticket = new Completion::Timed{ { shared_from_this(), serial }, 0.0 };
		ticket->stamp_submit(info);
	} else {
		info.callback = work_done;
		info.userdata1 = new Completion{ shared_from_this(), serial };
	}
	wgpuQueueSubmit(queue.get(), p_commands.size(), p_commands.data());
	wgpuQueueOnSubmittedWorkDone(queue.get(), info);
	return serial;
}

void DeviceState::work_done(WGPUQueueWorkDoneStatus p_status, void *p_userdata, void *) {
	std::unique_ptr<Completion> completion(static_cast<Completion *>(p_userdata));
	completion->finish(p_status);
}

void DeviceState::Completion::finish(WGPUQueueWorkDoneStatus p_status) const {
	state->assert_owner();
	if (p_status != WGPUQueueWorkDoneStatus_Success) {
		state->queue_failed = true;
		return;
	}
	state->completed_serial = MAX(state->completed_serial, serial);
	state->retired.erase(state->retired.begin(), state->retired.upper_bound(state->completed_serial));
}

} // namespace godot::webgpu

Error RenderingContextDriverWebGPU::initialize() {
	state = godot::webgpu::DeviceState::acquire();
	return state ? OK : ERR_UNAVAILABLE;
}

const RenderingContextDriver::Device &RenderingContextDriverWebGPU::device_get(uint32_t p_device_index) const {
	DEV_ASSERT(state && p_device_index == 0);
	return state->description;
}

uint32_t RenderingContextDriverWebGPU::device_get_count() const {
	return state && !state->failed() ? 1 : 0;
}

bool RenderingContextDriverWebGPU::device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const {
	return p_device_index == 0 && device_get_count() && p_surface && surface_state(p_surface)->surface;
}

RenderingDeviceDriver *RenderingContextDriverWebGPU::driver_create() {
	DEV_ASSERT(state);
	return memnew(RenderingDeviceDriverWebGPU(state));
}

bool RenderingContextDriverWebGPU::is_device_available() {
	return Thread::is_main_thread() && godot_webgpu_has_bootstrap_device();
}

void RenderingContextDriverWebGPU::driver_free(RenderingDeviceDriver *p_driver) {
	state->assert_owner();
	memdelete(p_driver);
}
