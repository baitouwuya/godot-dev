#pragma once

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <type_traits>

#ifdef WEB_ENABLED
#include <emscripten/emscripten.h>
#endif

namespace godot::webgpu {

inline bool webgpu_perf_is_enabled() {
#ifdef WEB_ENABLED
	static const bool enabled = EM_ASM_INT({ return new URLSearchParams(location.search).get('webgpu_perf') === '1'; });
	return enabled;
#else
	return false;
#endif
}

// One inventory for per-tick work, its sum, and its per-tick maximum.
#define WEBGPU_PERF_COUNTERS(m_field) \
	m_field(uint64_t, draws) \
	m_field(uint64_t, dispatches) \
	m_field(uint64_t, render_passes) \
	m_field(uint64_t, render_pass_splits_alias) \
	m_field(uint64_t, render_pass_splits_other) \
	m_field(uint64_t, compute_passes) \
	m_field(uint64_t, write_buffer_calls) \
	m_field(uint64_t, write_buffer_bytes) \
	m_field(uint64_t, push_write_calls) \
	m_field(uint64_t, push_write_bytes) \
	m_field(uint64_t, shader_module_calls) \
	m_field(double, shader_module_ms) \
	m_field(uint64_t, render_pso_calls) \
	m_field(double, render_pso_ms) \
	m_field(uint64_t, compute_pso_calls) \
	m_field(double, compute_pso_ms) \
	m_field(uint64_t, buffer_creates) \
	m_field(uint64_t, texture_creates) \
	m_field(uint64_t, bind_group_creates)

struct WebGPUPerfCounters {
#define WEBGPU_PERF_DECLARE(m_type, m_name) m_type m_name;
	WEBGPU_PERF_COUNTERS(WEBGPU_PERF_DECLARE)
#undef WEBGPU_PERF_DECLARE
};

struct WebGPUPerfFrame {
	WebGPUPerfCounters work;
	double raf_start_ms, iter_ms, pack_upgrade_ms, submit_to_done_ms;
	bool admitted, busy_serial, busy_map;
};

struct WebGPUPerfWindow {
	WebGPUPerfCounters sum, max;
	uint64_t raf_ticks, admitted_iters, skipped_serial_busy, skipped_map_busy, work_done_callbacks;
	double sum_iter_ms, max_iter_ms, sum_upgrade_ms, max_upgrade_ms, sum_submit_done_ms, max_submit_done_ms;
};

enum class WebGPUPerfBusy { SERIAL, MAP };

struct WebGPUPerfStats {
	WebGPUPerfFrame frame;
	WebGPUPerfWindow window;
	double window_start_ms;
	WebGPUPerfBusy last_busy; // Consumed only when the admission poll itself returns ERR_BUSY.
	bool armed, in_raf;
};

static_assert(std::is_trivial_v<WebGPUPerfStats> && std::is_standard_layout_v<WebGPUPerfStats>);
inline WebGPUPerfStats webgpu_perf_stats{}; // One instance across TUs; browser device owner thread only.

void webgpu_perf_install_raf_hooks();

inline void webgpu_perf_on_raf_begin(double p_now_ms) {
	auto &s = webgpu_perf_stats;
	const double last_done_ms = s.frame.submit_to_done_ms;
	s.frame = {};
	s.frame.raf_start_ms = p_now_ms;
	s.frame.submit_to_done_ms = last_done_ms;
	s.in_raf = true;
}

inline void webgpu_perf_on_work_done(double p_latency_ms) {
	// Spontaneous completions usually land between ticks. Do not count one sample per frame.
	auto &s = webgpu_perf_stats;
	s.frame.submit_to_done_ms = p_latency_ms;
	s.window.work_done_callbacks++;
	s.window.sum_submit_done_ms += p_latency_ms;
	s.window.max_submit_done_ms = std::max(s.window.max_submit_done_ms, p_latency_ms);
}

inline void webgpu_perf_accumulate(WebGPUPerfWindow &w, const WebGPUPerfFrame &f) {
	w.raf_ticks++;
	w.admitted_iters += f.admitted;
	w.skipped_serial_busy += f.busy_serial;
	w.skipped_map_busy += f.busy_map;
	w.sum_iter_ms += f.iter_ms;
	w.max_iter_ms = std::max(w.max_iter_ms, f.iter_ms);
	w.sum_upgrade_ms += f.pack_upgrade_ms;
	w.max_upgrade_ms = std::max(w.max_upgrade_ms, f.pack_upgrade_ms);
#define WEBGPU_PERF_ACCUMULATE(m_type, m_name) \
	w.sum.m_name += f.work.m_name; \
	w.max.m_name = std::max(w.max.m_name, f.work.m_name);
	WEBGPU_PERF_COUNTERS(WEBGPU_PERF_ACCUMULATE)
#undef WEBGPU_PERF_ACCUMULATE
}
#undef WEBGPU_PERF_COUNTERS

inline void webgpu_perf_on_raf_end(double p_now_ms) {
	auto &s = webgpu_perf_stats;
	const auto &f = s.frame;
	const auto &c = f.work;
	auto &w = s.window;
	s.in_raf = false;
	webgpu_perf_accumulate(w, f);

	const double raf_ms = p_now_ms - f.raf_start_ms;
	if (raf_ms >= 40.0) {
		std::printf("[WebGPU-Hitch] {\"raf_ms\":%.2f,\"iter_ms\":%.2f,\"upgrade_ms\":%.2f,"
				"\"draws\":%" PRIu64 ",\"rpass\":%" PRIu64 ",\"cpass\":%" PRIu64 ",\"wbuf_n\":%" PRIu64 ",\"wbuf_kb\":%.2f,"
				"\"shdr_n\":%" PRIu64 ",\"shdr_ms\":%.2f,\"rpso_n\":%" PRIu64 ",\"rpso_ms\":%.2f,\"cpso_n\":%" PRIu64 ",\"cpso_ms\":%.2f,"
				"\"buf_c\":%" PRIu64 ",\"tex_c\":%" PRIu64 ",\"bg_c\":%" PRIu64 ",\"admitted\":%s}\n",
				raf_ms, f.iter_ms, f.pack_upgrade_ms, c.draws, c.render_passes, c.compute_passes, c.write_buffer_calls, c.write_buffer_bytes / 1024.0,
				c.shader_module_calls, c.shader_module_ms, c.render_pso_calls, c.render_pso_ms, c.compute_pso_calls, c.compute_pso_ms,
				c.buffer_creates, c.texture_creates, c.bind_group_creates, f.admitted ? "true" : "false");
	}

	const double elapsed_ms = p_now_ms - s.window_start_ms;
	if (elapsed_ms < 1000.0) {
		return;
	}
	const double hz = 1000.0 / elapsed_ms;
	const double pf = w.admitted_iters ? 1.0 / w.admitted_iters : 0.0;
	const auto &sum = w.sum;
	std::printf("[WebGPU-Perf] {\"raf_hz\":%.2f,\"fps_admitted\":%.2f,\"skip_serial\":%" PRIu64 ",\"skip_map\":%" PRIu64 ","
			"\"iter_avg_ms\":%.2f,\"iter_max_ms\":%.2f,\"upg_max_ms\":%.2f,\"done_avg_ms\":%.2f,\"done_max_ms\":%.2f,\"done_n\":%" PRIu64 ","
			"\"draws_pf\":%.2f,\"disp_pf\":%.2f,\"rpass_pf\":%.2f,\"rpass_split_alias_pf\":%.2f,\"rpass_split_other_pf\":%.2f,\"cpass_pf\":%.2f,"
			"\"wbuf_pf\":%.2f,\"wbuf_kb_pf\":%.2f,\"push_pf\":%.2f,\"push_kb_pf\":%.2f,"
			"\"rpso_n\":%" PRIu64 ",\"rpso_ms\":%.2f,\"cpso_n\":%" PRIu64 ",\"cpso_ms\":%.2f,\"shdr_n\":%" PRIu64 ",\"shdr_ms\":%.2f,"
			"\"buf_c\":%" PRIu64 ",\"tex_c\":%" PRIu64 ",\"bg_c\":%" PRIu64 "}\n",
			w.raf_ticks * hz, w.admitted_iters * hz, w.skipped_serial_busy, w.skipped_map_busy,
			w.sum_iter_ms * pf, w.max_iter_ms, w.max_upgrade_ms, w.work_done_callbacks ? w.sum_submit_done_ms / w.work_done_callbacks : 0.0, w.max_submit_done_ms, w.work_done_callbacks,
			sum.draws * pf, sum.dispatches * pf, sum.render_passes * pf, sum.render_pass_splits_alias * pf, sum.render_pass_splits_other * pf, sum.compute_passes * pf,
			sum.write_buffer_calls * pf, sum.write_buffer_bytes * pf / 1024.0, sum.push_write_calls * pf, sum.push_write_bytes * pf / 1024.0,
			sum.render_pso_calls, sum.render_pso_ms, sum.compute_pso_calls, sum.compute_pso_ms, sum.shader_module_calls, sum.shader_module_ms, sum.buffer_creates, sum.texture_creates, sum.bind_group_creates);
	s.window_start_ms = p_now_ms;
	w = {};
}

} // namespace godot::webgpu
