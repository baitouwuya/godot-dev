#include "webgpu_commands.h"

using namespace godot::webgpu;

RDD::QueryPoolID RenderingDeviceDriverWebGPU::timestamp_query_pool_create(uint32_t p_count) {
	state->assert_owner();
	if (!state->has(WGPUFeatureName_TimestampQuery)) {
		return {};
	}
	auto *pool = memnew(TimestampPool);
	pool->capacity = p_count;
	return QueryPoolID(pool);
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_free(QueryPoolID p_pool) {
	state->assert_owner();
	memdelete(reinterpret_cast<TimestampPool *>(p_pool.id));
}

void RenderingDeviceDriverWebGPU::timestamp_query_pool_get_results(QueryPoolID p_pool, uint32_t p_count, uint64_t *r_results) {
	state->assert_owner();
	const auto &capture = reinterpret_cast<TimestampPool *>(p_pool.id)->latest;
	DEV_ASSERT(capture && p_count <= capture->count && capture->download->download_state == DownloadState::MAPPED);
	const auto &download = *capture->download;
	const void *data = wgpuBufferGetConstMappedRange(download.buffer.get(), 0, download.allocation_size);
	ERR_FAIL_NULL(data);
	std::memcpy(r_results, data, p_count * sizeof(uint64_t));
}

uint64_t RenderingDeviceDriverWebGPU::timestamp_query_result_to_time(uint64_t p_result) {
	return p_result; // Browser timestamps are nanoseconds, subject to privacy quantization.
}

void RenderingDeviceDriverWebGPU::command_timestamp_query_pool_reset(CommandBufferID, QueryPoolID p_pool, uint32_t) {
	state->assert_owner();
	reinterpret_cast<TimestampPool *>(p_pool.id)->latest.reset();
}

void RenderingDeviceDriverWebGPU::command_timestamp_write(CommandBufferID p_buffer, QueryPoolID p_pool, uint32_t p_index) {
	auto &cmd = recording(*state, p_buffer);
	auto *pool = reinterpret_cast<TimestampPool *>(p_pool.id);
	DEV_ASSERT(p_index < pool->capacity);
	auto &capture = cmd.timestamps[pool];
	if (!capture) {
		capture = std::make_shared<TimestampCapture>();
		WGPUQuerySetDescriptor desc = WGPU_QUERY_SET_DESCRIPTOR_INIT;
		desc.type = WGPUQueryType_Timestamp;
		desc.count = pool->capacity;
		capture->queries.reset(wgpuDeviceCreateQuerySet(state->device.get(), &desc));
		ERR_FAIL_COND(!capture->queries);
	}
	cmd.outside_pass();
	WGPUPassTimestampWrites writes = WGPU_PASS_TIMESTAMP_WRITES_INIT;
	writes.querySet = capture->queries.get();
	writes.beginningOfPassWriteIndex = p_index;
	WGPUComputePassDescriptor desc = WGPU_COMPUTE_PASS_DESCRIPTOR_INIT;
	desc.timestampWrites = &writes;
	ComputePassEncoderHandle marker(wgpuCommandEncoderBeginComputePass(cmd.encoder.get(), &desc));
	ERR_FAIL_COND(!marker);
	wgpuComputePassEncoderEnd(marker.get());
	capture->count = MAX(capture->count, p_index + 1);
}

void RenderingDeviceDriverWebGPU::encode_queries(CommandRecording &r_cmd) {
	for (const auto &[pool, capture] : r_cmd.timestamps) {
		if (!capture->count) {
			continue;
		}
		const uint64_t size = capture->count * sizeof(uint64_t);
		WGPUBufferDescriptor desc = WGPU_BUFFER_DESCRIPTOR_INIT;
		desc.usage = WGPUBufferUsage_QueryResolve | WGPUBufferUsage_CopySrc;
		desc.size = align_up(size, QUERY_RESOLVE_ALIGNMENT);
		capture->resolve.reset(wgpuDeviceCreateBuffer(state->device.get(), &desc));
		capture->download = make_transfer_buffer(*state, size, WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead);
		ERR_FAIL_COND(!capture->resolve || !capture->download);
		wgpuCommandEncoderResolveQuerySet(r_cmd.encoder.get(), capture->queries.get(), 0, capture->count, capture->resolve.get(), 0);
		wgpuCommandEncoderCopyBufferToBuffer(r_cmd.encoder.get(), capture->resolve.get(), 0, capture->download->buffer.get(), 0, size);
		r_cmd.download(capture->download);
		r_cmd.keep(capture);
		pool->latest = capture;
	}
}

void RenderingDeviceDriverWebGPU::command_begin_label(CommandBufferID p_buffer, const char *p_name, const Color &) {
	auto &cmd = recording(*state, p_buffer);
	cmd.labels.emplace_back(p_name);
	const auto label = wg_string(cmd.labels.back());
	if (cmd.render) {
		wgpuRenderPassEncoderPushDebugGroup(cmd.render.get(), label);
	} else if (cmd.compute) {
		wgpuComputePassEncoderPushDebugGroup(cmd.compute.get(), label);
	} else {
		wgpuCommandEncoderInsertDebugMarker(cmd.encoder.get(), label);
	}
}

void RenderingDeviceDriverWebGPU::command_end_label(CommandBufferID p_buffer) {
	auto &cmd = recording(*state, p_buffer);
	if (cmd.labels.empty()) {
		// RenderingDeviceGraph ends cross-command-buffer labels on the post-split command buffer.
		return;
	}
	if (cmd.render) {
		wgpuRenderPassEncoderPopDebugGroup(cmd.render.get());
	} else if (cmd.compute) {
		wgpuComputePassEncoderPopDebugGroup(cmd.compute.get());
	}
	cmd.labels.pop_back();
}

void RenderingDeviceDriverWebGPU::command_insert_breadcrumb(CommandBufferID p_buffer, uint32_t p_data) {
	auto &cmd = recording(*state, p_buffer);
	const std::string label = "Breadcrumb " + std::to_string(p_data);
	if (cmd.render) {
		wgpuRenderPassEncoderInsertDebugMarker(cmd.render.get(), wg_string(label));
	} else if (cmd.compute) {
		wgpuComputePassEncoderInsertDebugMarker(cmd.compute.get(), wg_string(label));
	} else {
		wgpuCommandEncoderInsertDebugMarker(cmd.encoder.get(), wg_string(label));
	}
}
