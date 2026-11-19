#pragma once

#include "webgpu_handles.h"

#include "core/os/thread.h"
#include "servers/rendering/rendering_context_driver.h"

#include <map>
#include <memory>
#include <set>
#include <vector>

namespace godot::webgpu {

using SubmitSerial = uint64_t;

struct GpuUse {
	SubmitSerial last_use_serial = 0; // Owner thread only; zero means not submitted.
};
using UseList = std::vector<std::shared_ptr<GpuUse>>;
using RetireQueue = std::multimap<SubmitSerial, std::shared_ptr<const void>>;

class DeviceState : public std::enable_shared_from_this<DeviceState> {
	struct Completion;
	static void work_done(WGPUQueueWorkDoneStatus p_status, void *p_userdata, void *);
	RetireQueue retired;
	SubmitSerial submit_serial = 0;
	SubmitSerial completed_serial = 0;
	bool queue_failed = false;

public:
	const Thread::ID owner_thread = Thread::get_caller_id();
	InstanceHandle instance;
	DeviceHandle device;
	QueueHandle queue;
	WGPULimits limits = WGPU_LIMITS_INIT;
	std::set<WGPUFeatureName> features;
	RenderingContextDriver::Device description;
	bool has_read_write_storage_textures = false;
	bool has_texture_formats_tier2 = false;

	static std::shared_ptr<DeviceState> acquire();
	~DeviceState();
	bool has(WGPUFeatureName p_feature) const { return features.count(p_feature) != 0; }
	void assert_owner() const { DEV_ASSERT(Thread::get_caller_id() == owner_thread); }
	bool failed() const { return queue_failed; }
	SubmitSerial submitted() const { return submit_serial; }
	SubmitSerial completed() const { return completed_serial; }
	SubmitSerial submit(const std::vector<WGPUCommandBuffer> &p_commands, UseList p_uses);
	void retire(SubmitSerial p_serial, std::shared_ptr<const void> p_object);
};
using DeviceLease = std::shared_ptr<DeviceState>;

struct WebGPUSurfacePlatformData {
	const char *canvas_selector = nullptr;
	bool premultiplied_alpha = false;
};

struct SurfaceState {
	DeviceLease state;
	SurfaceHandle surface;
	std::string canvas_selector;
	WGPUTextureFormat format = WGPUTextureFormat_Undefined;
	WGPUCompositeAlphaMode alpha = WGPUCompositeAlphaMode_Opaque;
	WGPUPresentMode present_mode = WGPUPresentMode_Fifo;
	uint32_t width = 0;
	uint32_t height = 0;
	bool needs_resize = true;
	bool wants_transparency = false;
	bool configured = false;

	~SurfaceState();
	Error create();
	Error configure();
	WGPUSurfaceGetCurrentTextureStatus acquire(TextureHandle &r_texture);
};

} // namespace godot::webgpu

class RenderingContextDriverWebGPU final : public RenderingContextDriver {
public:
	static bool is_device_available();
	Error initialize() override;
	const Device &device_get(uint32_t p_device_index) const override;
	uint32_t device_get_count() const override;
	bool device_supports_present(uint32_t p_device_index, SurfaceID p_surface) const override;
	RenderingDeviceDriver *driver_create() override;
	void driver_free(RenderingDeviceDriver *p_driver) override;
	SurfaceID surface_create(const void *p_platform_data) override;
	void surface_destroy(SurfaceID p_surface) override;
	void surface_set_size(SurfaceID p_surface, uint32_t p_width, uint32_t p_height) override;
	void surface_set_vsync_mode(SurfaceID p_surface, DisplayServerEnums::VSyncMode p_vsync_mode) override;
	DisplayServerEnums::VSyncMode surface_get_vsync_mode(SurfaceID p_surface) const override;
	void surface_set_hdr_output_enabled(SurfaceID p_surface, bool p_enabled) override;
	bool surface_get_hdr_output_enabled(SurfaceID p_surface) const override { return false; }
	void surface_set_hdr_output_reference_luminance(SurfaceID, float) override {}
	float surface_get_hdr_output_reference_luminance(SurfaceID) const override { return 1.0f; }
	void surface_set_hdr_output_max_luminance(SurfaceID, float) override {}
	float surface_get_hdr_output_max_luminance(SurfaceID) const override { return 1.0f; }
	void surface_set_hdr_output_linear_luminance_scale(SurfaceID, float) override {}
	float surface_get_hdr_output_linear_luminance_scale(SurfaceID) const override { return 1.0f; }
	float surface_get_hdr_output_max_value(SurfaceID) const override { return 1.0f; }
	uint32_t surface_get_width(SurfaceID p_surface) const override;
	uint32_t surface_get_height(SurfaceID p_surface) const override;
	void surface_set_needs_resize(SurfaceID p_surface, bool p_needs_resize) override;
	bool surface_get_needs_resize(SurfaceID p_surface) const override;
	bool is_debug_utils_enabled() const override { return true; }
	static const std::shared_ptr<godot::webgpu::SurfaceState> &surface_state(SurfaceID p_surface);

private:
	godot::webgpu::DeviceLease state;
};
