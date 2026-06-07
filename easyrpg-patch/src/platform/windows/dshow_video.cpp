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

#ifdef _WIN32

#include "dshow_video.h"
#include "output.h"

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <d3d9.h>
#include <dshow.h>
#include <vmr9.h>
#include <evcode.h>
#include <control.h>

namespace DShowVideo {

// ── helpers ──────────────────────────────────────────────
static IPin* FindPinDir(IBaseFilter* f, PIN_DIRECTION d) {
	if (!f) return nullptr;
	IEnumPins* ep = nullptr;
	if (FAILED(f->EnumPins(&ep))) return nullptr;
	IPin* p = nullptr;
	while (ep->Next(1, &p, nullptr) == S_OK) {
		PIN_DIRECTION pd;
		if (SUCCEEDED(p->QueryDirection(&pd)) && pd == d) { ep->Release(); return p; }
		p->Release(); p = nullptr;
	}
	ep->Release(); return nullptr;
}
#define InPin(f)  FindPinDir(f,PINDIR_INPUT)
#define OutPin(f) FindPinDir(f,PINDIR_OUTPUT)

// ── VideoPlayer ──────────────────────────────────────────

VideoPlayer::VideoPlayer() = default;
VideoPlayer::~VideoPlayer() { Close(); }

bool VideoPlayer::Open(const std::string& file_path, HWND hwnd_target) {
	Close();
	if (!hwnd_target) { Output::Warning("DShow: null HWND"); return false; }
	this->hwnd_target = hwnd_target;

	HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
		{ Output::Warning("DShow: CoInit (0x{:08X})", (unsigned)hr); Close(); return false; }

	hr = CoCreateInstance(CLSID_FilterGraph,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&graph_builder));
	if (FAILED(hr)) { Output::Warning("DShow: graph (0x{:08X})",(unsigned)hr); Close(); return false; }

	int wl = MultiByteToWideChar(CP_UTF8,0,file_path.c_str(),-1,nullptr,0);
	if (wl<=0) { Output::Warning("DShow: bad path"); Close(); return false; }
	auto wp = std::make_unique<wchar_t[]>(wl);
	MultiByteToWideChar(CP_UTF8,0,file_path.c_str(),-1,wp.get(),wl);

	hr = graph_builder->RenderFile(wp.get(),nullptr);
	if (FAILED(hr)) { Output::Warning("DShow: RenderFile (0x{:08X})",(unsigned)hr); Close(); return false; }

	// ── Embed the video renderer window ──
	hr = graph_builder->QueryInterface(IID_PPV_ARGS(&video_window));
	if (SUCCEEDED(hr)) {
		video_window->put_AutoShow(OAFALSE);
		video_window->put_WindowStyle(WS_CHILD|WS_CLIPSIBLINGS);
		video_window->put_Owner((OAHWND)hwnd_target);

		// Read native video size
		IBasicVideo* bv = nullptr;
		if (SUCCEEDED(graph_builder->QueryInterface(IID_PPV_ARGS(&bv)))) {
			basic_video = bv;
			bv->GetVideoSize(&native_w, &native_h);
			Output::Debug("DShow: native {}x{}", native_w, native_h);
		} else {
			// Fallback: try VMR9 to get native size
			IBaseFilter* vmr=nullptr;
			if (SUCCEEDED(CoCreateInstance(CLSID_VideoMixingRenderer9,nullptr,CLSCTX_INPROC,IID_PPV_ARGS(&vmr)))){
				IVMRWindowlessControl9* wc=nullptr;
				if (SUCCEEDED(vmr->QueryInterface(IID_PPV_ARGS(&wc)))){
					wc->GetNativeVideoSize(&native_w,&native_h,nullptr,nullptr);
					wc->Release();
				} vmr->Release();
			}
		}
	} else {
		Output::Warning("DShow: no IVideoWindow, fallback VMR9");
		IBaseFilter* vmr=nullptr;
		if (SUCCEEDED(CoCreateInstance(CLSID_VideoMixingRenderer9,nullptr,CLSCTX_INPROC,IID_PPV_ARGS(&vmr)))){
			IVMRFilterConfig9* cfg=nullptr;
			if (SUCCEEDED(vmr->QueryInterface(IID_PPV_ARGS(&cfg)))){
				cfg->SetRenderingMode(VMR9Mode_Windowless); cfg->Release();
			}
			IVMRWindowlessControl9* wl2=nullptr;
			if (SUCCEEDED(vmr->QueryInterface(IID_PPV_ARGS(&wl2)))){
				wl2->SetVideoClippingWindow(hwnd_target);
				wl2->GetNativeVideoSize(&native_w,&native_h,nullptr,nullptr);
				vmr_windowless = wl2; vmr_filter = vmr; vmr->AddRef();
				Output::Debug("DShow: VMR9 fallback native {}x{}",native_w,native_h);
			} vmr->Release();
		}
	}

	hr = graph_builder->QueryInterface(IID_PPV_ARGS(&media_control));
	if (FAILED(hr)) { Output::Warning("DShow: no IMediaControl"); Close(); return false; }

	hr = graph_builder->QueryInterface(IID_PPV_ARGS(&media_event));
	if (FAILED(hr)) { Output::Warning("DShow: no IMediaEventEx"); Close(); return false; }

	graph_builder->QueryInterface(IID_PPV_ARGS(&media_seeking));

	return true;
}

bool VideoPlayer::Play() {
	if (!media_control) return false;
	HRESULT hr = media_control->Run();
	if (FAILED(hr)) { Output::Warning("DShow: Run (0x{:08X})",(unsigned)hr); return false; }
	playing=true; ended=false;
	return true;
}

// ── Position logic ──────────────────────────────────────

void VideoPlayer::SetVideoPos(int origin_x, int origin_y,
							  int logical_w, int logical_h,
							  int win_w, int win_h)
{
	if (!hwnd_target) return;
	if (win_w<=0) win_w=320;
	if (win_h<=0) win_h=240;

	// Scale the logical video rect to actual window pixels
	double sx = double(win_w) / 320.0;
	double sy = double(win_h) / 240.0;
	double s  = (sx < sy) ? sx : sy;  // letterbox: uniform scale

	long vw = (long)(double(native_w > 0 ? native_w : logical_w) * s);
	long vh = (long)(double(native_h > 0 ? native_h : logical_h) * s);
	// Clamp to window (never exceed)
	if (vw > win_w) vw = win_w;
	if (vh > win_h) vh = win_h;

	long ox = (win_w - vw) / 2;
	long oy = (win_h - vh) / 2;

	pos_x = ox; pos_y = oy; pos_w = vw; pos_h = vh;

	if (video_window) {
		video_window->SetWindowPosition(ox, oy, vw, vh);
		video_window->put_Visible(OATRUE);
	} else if (vmr_windowless) {
		RECT rc = {ox, oy, ox+vw, oy+vh};
		vmr_windowless->SetVideoPosition(nullptr, &rc);
	}
}

void VideoPlayer::RefreshVideoPos() {
	if (pos_w>0 && pos_h>0) {
		if (video_window) video_window->SetWindowPosition(pos_x,pos_y,pos_w,pos_h);
		else if (vmr_windowless) { RECT r={pos_x,pos_y,pos_x+pos_w,pos_y+pos_h}; vmr_windowless->SetVideoPosition(nullptr,&r); }
	}
}

void VideoPlayer::Pause() { if (media_control) media_control->Pause(); playing=false; }
void VideoPlayer::Stop() { if (media_control) media_control->Stop(); playing=false; }

void VideoPlayer::Close() {
	if (media_control) media_control->Stop();
	if (video_window) { video_window->put_Visible(OAFALSE); video_window->put_Owner(0); }
	if (media_seeking) { media_seeking->Release(); media_seeking=nullptr; }
	if (basic_video) { basic_video->Release(); basic_video=nullptr; }
	if (video_window) { video_window->Release(); video_window=nullptr; }
	if (media_event) { media_event->Release(); media_event=nullptr; }
	if (media_control) { media_control->Release(); media_control=nullptr; }
	if (vmr_windowless) { vmr_windowless->Release(); vmr_windowless=nullptr; }
	if (vmr_config) { vmr_config->Release(); vmr_config=nullptr; }
	if (vmr_filter) { vmr_filter->Release(); vmr_filter=nullptr; }
	if (graph_builder) { graph_builder->Release(); graph_builder=nullptr; }
	CoUninitialize();
	hwnd_target=nullptr; playing=false; ended=false;
	new_width=0; new_height=0;
}

bool VideoPlayer::IsPlaying() const { return playing; }
bool VideoPlayer::IsEnded() const { return ended; }

double VideoPlayer::GetDuration() const {
	if (!media_seeking) return 0.0; LONGLONG d=0;
	return SUCCEEDED(media_seeking->GetDuration(&d)) ? double(d)/10000000.0 : 0.0;
}
double VideoPlayer::GetPosition() const {
	if (!media_seeking) return 0.0; LONGLONG p=0;
	return SUCCEEDED(media_seeking->GetCurrentPosition(&p)) ? double(p)/10000000.0 : 0.0;
}
bool VideoPlayer::Seek(double s) {
	if (!media_seeking) return false;
	LONGLONG pos = (LONGLONG)(s*10000000.0);
	return SUCCEEDED(media_seeking->SetPositions(&pos,AM_SEEKING_AbsolutePositioning,nullptr,AM_SEEKING_NoPositioning));
}

void VideoPlayer::ProcessEvents() {
	if (!media_event) return;
	long c=0; LONG_PTR p1=0,p2=0;
	while (media_event->GetEvent(&c,&p1,&p2,0)==S_OK) {
		if (c==EC_COMPLETE||c==EC_USERABORT||c==EC_ERRORABORT) { ended=true; playing=false; }
		media_event->FreeEventParams(c,p1,p2);
	}
}

// Stubs
bool VideoPlayer::CreateGraph() { return true; }
bool VideoPlayer::AddFilterByCLSID(REFCLSID,IBaseFilter**,const wchar_t*) { return false; }
bool VideoPlayer::ConnectFilters() { return false; }

bool IsSupported() { return true; }
} // ns
#endif
