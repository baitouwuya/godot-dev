#pragma once

#include <webgpu/webgpu.h>

#include <string>
#include <utility>

namespace godot::webgpu {

template <class T, void (*Release)(T)>
class WgHandle {
	T handle = nullptr;

public:
	WgHandle() = default;
	explicit WgHandle(T p_owned) : handle(p_owned) {}
	~WgHandle() { reset(); }
	WgHandle(const WgHandle &) = delete;
	WgHandle &operator=(const WgHandle &) = delete;
	WgHandle(WgHandle &&p_other) noexcept : handle(std::exchange(p_other.handle, nullptr)) {}
	WgHandle &operator=(WgHandle &&p_other) noexcept {
		if (this != &p_other) {
			reset(std::exchange(p_other.handle, nullptr));
		}
		return *this;
	}
	void reset(T p_owned = nullptr) {
		if (T old = std::exchange(handle, p_owned)) {
			Release(old);
		}
	}
	T get() const { return handle; }
	explicit operator bool() const { return handle != nullptr; }
};

#define GODOT_WEBGPU_HANDLE(m_type) using m_type##Handle = WgHandle<WGPU##m_type, wgpu##m_type##Release>
GODOT_WEBGPU_HANDLE(Instance);
GODOT_WEBGPU_HANDLE(Device);
GODOT_WEBGPU_HANDLE(Queue);
GODOT_WEBGPU_HANDLE(Buffer);
GODOT_WEBGPU_HANDLE(Texture);
GODOT_WEBGPU_HANDLE(TextureView);
GODOT_WEBGPU_HANDLE(Sampler);
GODOT_WEBGPU_HANDLE(BindGroupLayout);
GODOT_WEBGPU_HANDLE(BindGroup);
GODOT_WEBGPU_HANDLE(ShaderModule);
GODOT_WEBGPU_HANDLE(RenderPipeline);
GODOT_WEBGPU_HANDLE(ComputePipeline);
GODOT_WEBGPU_HANDLE(PipelineLayout);
GODOT_WEBGPU_HANDLE(Surface);
GODOT_WEBGPU_HANDLE(CommandEncoder);
GODOT_WEBGPU_HANDLE(CommandBuffer);
GODOT_WEBGPU_HANDLE(RenderPassEncoder);
GODOT_WEBGPU_HANDLE(ComputePassEncoder);
GODOT_WEBGPU_HANDLE(QuerySet);
#undef GODOT_WEBGPU_HANDLE

inline WGPUStringView wg_string(const std::string &p_string) {
	return { p_string.data(), p_string.size() };
}

} // namespace godot::webgpu
