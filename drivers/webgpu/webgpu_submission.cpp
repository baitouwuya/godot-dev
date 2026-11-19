#include "webgpu_commands.h"

#include <algorithm>

using namespace godot::webgpu;

RDD::CommandQueueFamilyID RenderingDeviceDriverWebGPU::command_queue_family_get(BitField<CommandQueueFamilyBits>, RenderingContextDriver::SurfaceID) {
	return CommandQueueFamilyID(1);
}

RDD::CommandQueueID RenderingDeviceDriverWebGPU::command_queue_create(CommandQueueFamilyID, bool) {
	state->assert_owner();
	return CommandQueueID(state->queue.get());
}

void RenderingDeviceDriverWebGPU::command_queue_free(CommandQueueID) {
	// Logical queues borrow the device's single, ordered queue.
}

RDD::SemaphoreID RenderingDeviceDriverWebGPU::semaphore_create() {
	// Submit order on the shared queue already supplies the semaphore dependency.
	return SemaphoreID(1);
}

void RenderingDeviceDriverWebGPU::semaphore_free(SemaphoreID) {}

RDD::FenceID RenderingDeviceDriverWebGPU::fence_create() {
	return FenceID(memnew(Fence));
}

Error RenderingDeviceDriverWebGPU::fence_wait(FenceID p_fence) {
	state->assert_owner();
	wgpuInstanceProcessEvents(state->instance.get());
	auto &fence = *reinterpret_cast<Fence *>(p_fence.id);
	if (state->failed()) {
		return ERR_CANT_ACQUIRE_RESOURCE;
	}
	if (fence.serial > state->completed()) {
		return ERR_BUSY;
	}
	for (const auto &buffer : fence.downloads) {
		if (buffer->download_state == DownloadState::FAILED) {
			return ERR_CANT_ACQUIRE_RESOURCE;
		}
		if (buffer->download_state != DownloadState::MAPPED) {
			return ERR_BUSY;
		}
	}
	fence.downloads.clear();
	return OK;
}

void RenderingDeviceDriverWebGPU::fence_free(FenceID p_fence) {
	state->assert_owner();
	memdelete(reinterpret_cast<Fence *>(p_fence.id));
}

RDD::CommandPoolID RenderingDeviceDriverWebGPU::command_pool_create(CommandQueueFamilyID, CommandBufferType p_type) {
	state->assert_owner();
	ERR_FAIL_COND_V_MSG(p_type != COMMAND_BUFFER_TYPE_PRIMARY, {}, "WebGPU does not record secondary command buffers.");
	return CommandPoolID(memnew(CommandPool));
}

bool RenderingDeviceDriverWebGPU::command_pool_reset(CommandPoolID p_pool) {
	state->assert_owner();
	for (auto &cmd : reinterpret_cast<CommandPool *>(p_pool.id)->buffers) {
		for (const auto &ticket : cmd->tickets) {
			cancel_readback(ticket);
		}
		*cmd = CommandRecording{};
	}
	return true;
}

void RenderingDeviceDriverWebGPU::command_pool_free(CommandPoolID p_pool) {
	command_pool_reset(p_pool);
	memdelete(reinterpret_cast<CommandPool *>(p_pool.id));
}

RDD::CommandBufferID RenderingDeviceDriverWebGPU::command_buffer_create(CommandPoolID p_pool) {
	state->assert_owner();
	auto &buffers = reinterpret_cast<CommandPool *>(p_pool.id)->buffers;
	buffers.push_back(std::make_unique<CommandRecording>());
	return CommandBufferID(buffers.back().get());
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin(CommandBufferID p_buffer) {
	auto &cmd = recording(*state, p_buffer);
	DEV_ASSERT(!cmd.encoder && !cmd.finished);
	cmd.encoder.reset(wgpuDeviceCreateCommandEncoder(state->device.get(), nullptr));
	ERR_FAIL_COND_V(!cmd.encoder, false);
	return true;
}

void RenderingDeviceDriverWebGPU::command_buffer_end(CommandBufferID p_buffer) {
	auto &cmd = recording(*state, p_buffer);
	DEV_ASSERT(!cmd.layout);
	cmd.outside_pass();
	cmd.labels.clear(); // RenderingDeviceGraph may split command buffers while a graph-level label is active.
	encode_queries(cmd);
	cmd.finished.reset(wgpuCommandEncoderFinish(cmd.encoder.get(), nullptr));
	cmd.encoder.reset();
	ERR_FAIL_COND(!cmd.finished);
}

bool RenderingDeviceDriverWebGPU::command_buffer_begin_secondary(CommandBufferID, RenderPassID, uint32_t, FramebufferID) {
	ERR_FAIL_V_MSG(false, "WebGPU does not record secondary command buffers.");
}

void RenderingDeviceDriverWebGPU::command_buffer_execute_secondary(CommandBufferID, VectorView<CommandBufferID>) {
	ERR_FAIL_MSG("WebGPU does not record secondary command buffers.");
}

Error RenderingDeviceDriverWebGPU::command_queue_execute_and_present(CommandQueueID, VectorView<SemaphoreID>, VectorView<CommandBufferID> p_buffers,
		VectorView<SemaphoreID>, FenceID p_fence, VectorView<SwapChainID> p_chains) {
	state->assert_owner();
	std::vector<WGPUCommandBuffer> commands;
	UseList uses;
	for (uint32_t i = 0; i < p_buffers.size(); ++i) {
		auto &cmd = recording(*state, p_buffers[i]);
		DEV_ASSERT(cmd.finished);
		commands.push_back(cmd.finished.get());
		for (const auto &page : cmd.push_pages) {
			wgpuQueueWriteBuffer(state->queue.get(), page->gpu->buffer.get(), 0, page->bytes.data(), align_up(page->used, BUFFER_COPY_ALIGNMENT));
			uses.push_back(page);
		}
		uses.insert(uses.end(), cmd.uses.begin(), cmd.uses.end());
	}
	const SubmitSerial serial = commands.empty() ? state->submitted() : state->submit(commands, std::move(uses));
	ERR_FAIL_COND_V(!commands.empty() && !serial, ERR_CANT_ACQUIRE_RESOURCE);
	Fence *fence = p_fence ? reinterpret_cast<Fence *>(p_fence.id) : nullptr;
	if (fence) {
		fence->serial = serial;
		fence->downloads.clear();
	}
	for (uint32_t i = 0; i < p_buffers.size(); ++i) {
		auto &cmd = recording(*state, p_buffers[i]);
		for (const auto &buffer : cmd.downloads) {
			if (buffer->download_state == DownloadState::IDLE) {
				arm_download(state, buffer);
			}
			if (fence) {
				fence->downloads.push_back(buffer);
			}
		}
		for (auto &ticket : cmd.tickets) {
			arm_readback(ticket, serial);
		}
		cmd.tickets.clear();
		cmd = CommandRecording{};
	}
	for (uint32_t i = 0; i < p_chains.size(); ++i) {
		swap_chain_drop_frame(p_chains[i]); // Browser presents the current texture at the end of the callback.
	}
	return OK;
}

std::shared_ptr<PushPage> RenderingDeviceDriverWebGPU::acquire_push_page(uint32_t p_size) {
	for (const auto &page : push_pool) {
		if (page.use_count() == 1 && page->last_use_serial <= state->completed() && page->bytes.size() >= p_size) {
			page->used = 0;
			return page;
		}
	}
	const uint32_t page_size = std::max(PUSH_PAGE_BYTES, p_size);
	auto page = std::make_shared<PushPage>();
	page->gpu = make_transfer_buffer(*state, page_size, WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
	ERR_FAIL_COND_V(!page->gpu, {});
	page->bytes.resize(page_size);
	push_pool.push_back(page);
	return page;
}

void RenderingDeviceDriverWebGPU::command_bind_push_constants(CommandBufferID p_buffer, ShaderID p_shader, uint32_t p_first, VectorView<uint32_t> p_values) {
	auto &cmd = recording(*state, p_buffer);
	const uint32_t first = p_first * sizeof(uint32_t);
	const uint32_t size = resource<ShaderResource>(p_shader)->reflection.push_constant_size;
	DEV_ASSERT(first + p_values.size() * sizeof(uint32_t) <= size && size <= cmd.push_data.size());
	for (uint32_t i = 0; i < p_values.size(); ++i) {
		std::memcpy(cmd.push_data.data() + first + i * sizeof(uint32_t), &p_values[i], sizeof(uint32_t));
	}
	const uint32_t alignment = state->limits.minStorageBufferOffsetAlignment;
	auto page = cmd.push_pages.empty() ? nullptr : cmd.push_pages.back();
	if (!page || align_up(page->used, alignment) + size > page->bytes.size()) {
		page = acquire_push_page(size);
		ERR_FAIL_COND(!page);
		cmd.push_pages.push_back(page);
	}
	const uint32_t offset = align_up(page->used, alignment);
	std::memcpy(page->bytes.data() + offset, cmd.push_data.data(), size);
	page->used = offset + size;
	cmd.push = { page->gpu, page, offset, size };
}

void RenderingDeviceDriverWebGPU::begin_segment(uint32_t, uint32_t) {
	state->assert_owner();
	wgpuInstanceProcessEvents(state->instance.get());
}

void RenderingDeviceDriverWebGPU::end_segment() {}
