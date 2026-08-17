// SPDX-FileCopyrightText: sumu Authors
// SPDX-License-Identifier: AGPL-3.0

#include "player.h"

PYBIND11_MODULE(sumu_core, m)
{
    m.doc() = "sumu native present kernel: clock-driven decode/present/AI-mix with seek=reposition";

    py::class_<Player>(m, "Player")
        .def(py::init<int, int, bool>(),
            py::arg("width_hint") = 1280, py::arg("height_hint") = 720, py::arg("maximized") = false)
        .def("pick_open_file", &Player::pick_open_file) // blocking Win32 dialog, call before open()
        .def("pick_folder", &Player::pick_folder) // web-stream video-root folder picker (blocking)
        .def("set_stream_running", &Player::set_stream_running,
            py::arg("running"), py::arg("url") = std::string("")) // 停止/启动 toggle + access URL
        .def("set_stream_defaults", &Player::set_stream_defaults, py::arg("port"), py::arg("root"),
            py::arg("no_token"), py::arg("token"))
        // Offline-export full-screen mode (Phase 2 extension).
        .def("set_export_mode", &Player::set_export_mode, py::arg("on"))
        .def("set_export_snapshot", &Player::set_export_snapshot, py::arg("snapshot"))
        .def("pick_save_file", &Player::pick_save_file,
            py::arg("default_name") = std::string(""))
        .def("close_current_session", &Player::close_current_session,
            py::call_guard<py::gil_scoped_release>())
        // gil_scoped_release: network open/reopen can block tens of seconds; Python's main loop
        // (and async open worker) must keep pumping UI. CUDA context is rebound inside open_session.
        .def("open", &Player::open, py::arg("path"),
            py::call_guard<py::gil_scoped_release>())
        .def("reopen", &Player::reopen, py::arg("path"), // M-C2: runtime file swap, present never stops
            py::call_guard<py::gil_scoped_release>())
        .def("is_network", &Player::is_network) // http(s) session: shallow ring, scrub off
        .def("notify_open_url_finished", &Player::notify_open_url_finished, py::arg("ok"))
        .def("play", &Player::play)
        .def("pause", &Player::pause)
        .def("is_playing", &Player::is_playing)
        .def("has_audio", &Player::has_audio)
        .def("set_volume", &Player::set_volume, py::arg("volume"))
        .def("get_volume", &Player::get_volume)
        .def("set_muted", &Player::set_muted, py::arg("muted"))
        .def("toggle_mute", &Player::toggle_mute)
        .def("is_muted", &Player::is_muted)
        .def("set_ai_enabled", &Player::set_ai_enabled, py::arg("enabled")) // Phase 6 M-D
        .def("is_ai_enabled", &Player::is_ai_enabled)
        .def("set_fps_div", &Player::set_fps_div, py::arg("fps_div"))
        .def("fps_div", &Player::fps_div)
        .def("source_fps", &Player::source_fps)
        .def("ui_tick", &Player::ui_tick) // M2: main-thread NewFrame/build/Render/publish tick
        .def("ui_ready", &Player::ui_ready)
        .def("path", &Player::path)
        .def("set_ui_config", &Player::set_ui_config,
             py::arg("clip_length"), py::arg("max_regions"),
             py::arg("cold_start_s") = 1.0f, py::arg("target_fps") = 0,
             py::arg("ai_restore_fps") = -1.0f, py::arg("lead") = 180)
        .def("set_status_text", &Player::set_status_text, py::arg("text"))
        .def("set_compile_ui", &Player::set_compile_ui,
             py::arg("state"), py::arg("progress"), py::arg("text")) // first-screen TRT compile prompt
        .def("set_ui_strings", &Player::set_ui_strings, py::arg("strings")) // i18n label table

        .def("take_ui_intents", &Player::take_ui_intents) // M3: drains + clears ui_intents_
        .def("seek", &Player::seek, py::arg("frame_num"))
        .def("push_ai_frame", &Player::push_ai_frame,
            py::arg("frame_num"), py::arg("dev_ptr"), py::arg("width"), py::arg("height"), py::arg("pitch_bytes"))
        .def("get_cuda_nv12_by_frame", &Player::get_cuda_nv12_by_frame, py::arg("frame_num"))
        .def("current_frame", &Player::current_frame)
        .def("frame_count", &Player::frame_count)
        .def("fps", &Player::fps)
        .def("dims", &Player::dims)
        .def("ring_capacity", &Player::ring_capacity)
        .def("ai_ring_capacity", &Player::ai_ring_capacity)
        .def("decode_ahead_max", &Player::decode_ahead_max)
        .def("ai_hit_rate", &Player::ai_hit_rate)
        .def("present_stats", &Player::present_stats)
        .def("stats", &Player::stats)
        .def("dump_present_trace", &Player::dump_present_trace, py::arg("path"))
        .def("pump_messages", &Player::pump_messages)
        .def("should_quit", &Player::should_quit)
        .def("close", &Player::close);

    // Headless decode bridge (transcode / web-streaming / offline-export): implemented in
    // headless_decode.cpp, bound here so it lands in the same sumu_core module.
    init_headless_decode(m);
}
