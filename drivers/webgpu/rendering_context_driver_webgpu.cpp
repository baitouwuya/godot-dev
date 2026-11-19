#include "rendering_context_driver_webgpu.h"

#include "rendering_device_driver_webgpu.h"

#include <emscripten.h>
#include <type_traits>

static_assert(!std::is_abstract_v<RenderingDeviceDriverWebGPU>);

EM_JS(int, godot_webgpu_has_bootstrap_device, (), {
	return !!Module['preinitializedWebGPUDevice'];
});

EM_JS(int, godot_webgpu_has_texture_formats_tier2, (), {
	return Module['preinitializedWebGPUDevice']['features']['has']('texture-formats-tier2');
});

namespace godot::webgpu {

struct DeviceState::Completion {
	DeviceLease state;
	SubmitSerial serial;
};

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
	wgpuQueueSubmit(queue.get(), p_commands.size(), p_commands.data());
	WGPUQueueWorkDoneCallbackInfo info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
	info.mode = WGPUCallbackMode_AllowSpontaneous;
	info.callback = work_done;
	info.userdata1 = new Completion{ shared_from_this(), serial };
	wgpuQueueOnSubmittedWorkDone(queue.get(), info);
	return serial;
}

void DeviceState::work_done(WGPUQueueWorkDoneStatus p_status, void *p_userdata, void *) {
	std::unique_ptr<Completion> completion(static_cast<Completion *>(p_userdata));
	DeviceState &state = *completion->state;
	state.assert_owner();
	if (p_status != WGPUQueueWorkDoneStatus_Success) {
		state.queue_failed = true;
		return;
	}
	state.completed_serial = MAX(state.completed_serial, completion->serial);
	state.retired.erase(state.retired.begin(), state.retired.upper_bound(state.completed_serial));
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
