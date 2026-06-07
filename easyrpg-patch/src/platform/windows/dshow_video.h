/*
 * This file is part of EasyRPG Player.
 *
 * EasyRPG Player is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * EasyRPG Player is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with EasyRPG Player. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef EP_DSHOW_VIDEO_H
#define EP_DSHOW_VIDEO_H

#ifdef _WIN32

#include <string>
#include <memory>
#include <cstdint>
#include <windows.h>
#include <atomic>

struct IGraphBuilder;
struct IMediaControl;
struct IMediaEventEx;
struct IVideoWindow;
struct IBasicVideo;
struct IMediaSeeking;
struct IVMRWindowlessControl9;
struct IVMRFilterConfig9;
struct IBaseFilter;

namespace DShowVideo {

/**
 * DirectShow-based video player that embeds video into a Win32 window.
 *
 * Uses IVideoWindow::put_Owner to embed the Video Renderer's window
 * as a child of the target HWND, then controls position/size.
 * Falls back to VMR9 windowless mode if IVideoWindow is unavailable.
 */
class VideoPlayer {
public:
	VideoPlayer();
	~VideoPlayer();

	VideoPlayer(const VideoPlayer&) = delete;
	VideoPlayer& operator=(const VideoPlayer&) = delete;

	/**
	 * Open and prepare a video file for playback.
	 * @param file_path Full path to video (UTF-8)
	 * @param hwnd_target Parent window handle
	 * @return true on success
	 */
	bool Open(const std::string& file_path, HWND hwnd_target);

	/** Start or resume playback */
	bool Play();

	/** Pause playback */
	void Pause();

	/** Stop and reset */
	void Stop();

	/** Close and release resources */
	void Close();

	/**
	/**
	 * Set video position within the window, letterboxed to preserve aspect ratio.
	 * @param origin_x, origin_y Position of video area in game logical pixels
	 * @param logical_w, logical_h Size of video area in game logical pixels
	 * @param window_w, window_h  Actual window client size in pixels
	 */
	void SetVideoPos(int origin_x, int origin_y, int logical_w, int logical_h, int window_w, int window_h);

	/** Re-apply the last video position (e.g. after window resize) */
	void RefreshVideoPos();

	bool IsPlaying() const;
	bool IsEnded() const;

	double GetDuration() const;
	double GetPosition() const;
	bool Seek(double seconds);

	/** Process DirectShow events. Call periodically. */
	void ProcessEvents();

private:
	bool CreateGraph();
	bool AddFilterByCLSID(REFCLSID clsid, IBaseFilter** out_filter, const wchar_t* name);
	bool ConnectFilters();

	IGraphBuilder* graph_builder = nullptr;
	IMediaControl* media_control = nullptr;
	IMediaEventEx* media_event = nullptr;
	IVideoWindow* video_window = nullptr;
	IBasicVideo* basic_video = nullptr;
	IMediaSeeking* media_seeking = nullptr;

	// VMR9 fallback
	IVMRWindowlessControl9* vmr_windowless = nullptr;
	IVMRFilterConfig9* vmr_config = nullptr;
	IBaseFilter* vmr_filter = nullptr;

	HWND hwnd_target = nullptr;
	std::atomic<bool> ended{false};
	std::atomic<bool> playing{false};

	// Video geometry
	long native_w = 0;
	long native_h = 0;
	long pos_x = 0, pos_y = 0, pos_w = 0, pos_h = 0;
	// Window dimensions for resize tracking
	int new_width = 0, new_height = 0;
};

/** DirectShow is supported on all modern Windows */
bool IsSupported();

} // namespace DShowVideo

#endif // _WIN32

#endif // EP_DSHOW_VIDEO_H
