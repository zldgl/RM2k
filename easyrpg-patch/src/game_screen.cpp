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

// Headers
#include <cmath>
#include "bitmap.h"
#include <lcf/data.h>
#include "player.h"
#include "game_battle.h"
#include "game_battler.h"
#include "game_screen.h"
#include "game_system.h"
#include "game_variables.h"
#include "game_map.h"
#include "output.h"
#include "utils.h"
#include "options.h"
#include <lcf/reader_util.h>
#include "scene.h"
#include "weather.h"
#include "flash.h"
#include "shake.h"
#include "rand.h"
#include "filefinder.h"
#include "baseui.h"
#include "fmt/format.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <SDL.h>
#  include <SDL_syswm.h>
#endif

Game_Screen::Game_Screen()
{
}

Game_Screen::~Game_Screen() {
}

void Game_Screen::SetSaveData(lcf::rpg::SaveScreen screen)
{
	CancelBattleAnimation();

	data = std::move(screen);
}

void Game_Screen::InitGraphics() {
	weather = std::make_unique<Weather>();
	OnWeatherChanged();

	if (data.battleanim_active) {
		ShowBattleAnimation(data.battleanim_id,
				data.battleanim_target,
				data.battleanim_global,
				data.battleanim_frame);
	}
}

void Game_Screen::OnMapChange() {
	data.flash_red = 0;
	data.flash_green = 0;
	data.flash_blue = 0;
	data.flash_time_left = 0;
	data.flash_current_level = 0;
	flash_sat = 0;
	flash_period = 0;

	if (data.tint_current_red < 0 ||
		data.tint_current_green < 0 ||
		data.tint_current_blue < 0 ||
		data.tint_current_sat < 0) {
		data.tint_current_red = 100;
		data.tint_current_green = 100;
		data.tint_current_blue = 100;
		data.tint_current_sat = 100;
	}

	movie_filename = "";
	movie_pos_x = 0;
	movie_pos_y = 0;
	movie_res_x = 0;
	movie_res_y = 0;

#ifdef _WIN32
	movie_player.reset();
#endif

	data.battleanim_active = false;
	animation.reset();
}

void Game_Screen::TintScreen(int r, int g, int b, int s, int tenths) {
	data.tint_finish_red = r;
	data.tint_finish_green = g;
	data.tint_finish_blue = b;
	data.tint_finish_sat = s;

	data.tint_time_left = tenths;

	if (data.tint_time_left == 0) {
		data.tint_current_red = data.tint_finish_red;
		data.tint_current_green = data.tint_finish_green;
		data.tint_current_blue = data.tint_finish_blue;
		data.tint_current_sat = data.tint_finish_sat;
	}
}

void Game_Screen::FlashOnce(int r, int g, int b, int s, int frames) {
	data.flash_red = r;
	data.flash_green = g;
	data.flash_blue = b;
	flash_sat = s;
	data.flash_current_level = s;
	data.flash_time_left = frames;
	data.flash_continuous = false;
	flash_period = 0;
}

void Game_Screen::FlashBegin(int r, int g, int b, int s, int frames) {
	FlashOnce(r, g, b, s, frames);

	flash_period = frames;
	data.flash_continuous = true;
}

void Game_Screen::FlashEnd() {
	data.flash_time_left = 0;
	data.flash_current_level = 0;
	flash_period = 0;
	data.flash_continuous = false;
}

void Game_Screen::FlashMapStepDamage() {
	FlashOnce(31, 10, 10, 20, 6);
}

void Game_Screen::ShakeOnce(int power, int speed, int tenths) {
	data.shake_strength = power;
	data.shake_speed = speed;
	data.shake_time_left = tenths;
	data.shake_continuous = false;
	// Shake position is not reset in RPG_RT, so that multiple shakes
	// which interrupt each other flow smoothly.
}

void Game_Screen::ShakeBegin(int power, int speed) {
	data.shake_strength = power;
	data.shake_speed = speed;
	data.shake_time_left = Shake::kShakeContinuousTimeStart;
	data.shake_continuous = true;
	// Shake position is not reset in RPG_RT, so that multiple shakes
	// which interrupt each other flow smoothly.
}

void Game_Screen::ShakeEnd() {
	data.shake_position = 0;
	data.shake_time_left = 0;
	// RPG_RT does not turn off the continuous shake flag when shake is disabled.
}

void Game_Screen::SetWeatherEffect(int type, int strength) {
	// Some games call weather effects in a parallel process
	// This causes issues in the rendering (weather rendered too fast)
	if (data.weather != type ||
		data.weather_strength != strength) {
		data.weather = type;
		data.weather_strength = strength;
		OnWeatherChanged();
	}
}

// Helper: get the HWND from the SDL window
static HWND GetSdlWindowHandle() {
	// Try various SDL2 methods to get a valid window handle
	SDL_Window* sdl_win = nullptr;

	// Method 1: Get the window that has keyboard focus
	sdl_win = SDL_GetKeyboardFocus();
	if (!sdl_win) {
		// Method 2: Get first window by ID
		sdl_win = SDL_GetWindowFromID(1);
	}
	if (!sdl_win) {
		// Method 3: Get the window associated with the current mouse focus
		sdl_win = SDL_GetMouseFocus();
	}

	if (!sdl_win) {
		Output::Warning("Movie: Cannot find SDL window.");
		return nullptr;
	}

	SDL_SysWMinfo wminfo;
	SDL_VERSION(&wminfo.version);
	if (!SDL_GetWindowWMInfo(sdl_win, &wminfo)) {
		Output::Warning("Movie: Cannot get window HWND from SDL.");
		return nullptr;
	}

	return wminfo.info.win.window;
}

void Game_Screen::PlayMovie(std::string filename,
							int pos_x, int pos_y, int res_x, int res_y) {
	movie_filename = std::move(filename);
	movie_pos_x = pos_x;
	movie_pos_y = pos_y;
	movie_res_x = res_x;
	movie_res_y = res_y;

#ifdef _WIN32
	// Close any previous movie
	movie_player.reset();

	// Find the movie file
	std::string movie_rel = FileFinder::FindMovie(movie_filename);
	if (movie_rel.empty()) {
		auto found = FileFinder::Game().FindFile(movie_filename);
		if (found.empty()) {
			Output::Warning("Movie: '{}' not found in Movie/ dir.", movie_filename);
			movie_filename.clear();
			return;
		}
		movie_rel = found;
	}

	// Get the full physical path
	std::string root_path = FileFinder::GetFullFilesystemPath(FileFinder::Root());
	if (root_path.empty()) {
		char buf[MAX_PATH];
		if (GetCurrentDirectoryA(MAX_PATH, buf)) {
			root_path = buf;
		} else {
			Output::Warning("Movie: Cannot determine game path.");
			movie_filename.clear();
			return;
		}
	}

	// Build full path with backslashes
	for (auto& c : root_path) { if (c == '/') c = '\\'; }
	if (!root_path.empty() && root_path.back() != '\\') root_path += '\\';
	for (auto& c : movie_rel) { if (c == '/') c = '\\'; }
	std::string movie_full = root_path + movie_rel;

	// Get the SDL window HWND
	HWND hwnd = GetSdlWindowHandle();
	if (!hwnd) {
		Output::Warning("Movie: Cannot get SDL window handle.");
		movie_filename.clear();
		return;
	}

	Output::Debug("Movie: Playing '{}' embedded in window.", movie_full);

	// Create and open the DirectShow player
	auto player = std::make_unique<DShowVideo::VideoPlayer>();
	if (!player->Open(movie_full, hwnd)) {
		Output::Warning("Movie: Failed to open video file (DirectShow).");
		movie_filename.clear();
		return;
	}

	// Set video position inside the window
	if (res_x > 0 && res_y > 0) {
		RECT rc;
		GetClientRect(hwnd, &rc);
		player->SetVideoPos(pos_x, pos_y, res_x, res_y, rc.right, rc.bottom);
	}

	// Start playing
	if (!player->Play()) {
		Output::Warning("Movie: Failed to start playback.");
		movie_filename.clear();
		return;
	}

	movie_player = std::move(player);

	// ============================================================
	// Blocking playback loop:
	//   - Process COM events to detect EC_COMPLETE
	//   - Process Windows messages (keeps window responsive + renders video)
	//   - Each frame re-compute video position from current window size
	//     so the video scales properly when the user resizes the window
	//   - When video ends, loop exits and game continues
	// ============================================================
	Output::Debug("Movie: Entering blocking playback loop.");
	while (movie_player && !movie_player->IsEnded() && movie_player->IsPlaying()) {
		movie_player->ProcessEvents();

		// Process Windows messages (critical for DirectShow rendering + resize events)
		MSG msg;
		while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}

		// Re-compute video position from current window dimensions
		RECT wr;
		GetClientRect(hwnd, &wr);
		int win_w = wr.right;
		int win_h = wr.bottom;

		if (win_w > 0 && win_h > 0) {
			movie_player->SetVideoPos(pos_x, pos_y, res_x, res_y, win_w, win_h);
		} else {
			movie_player->RefreshVideoPos();
		}

		// Small sleep to prevent 100% CPU spin on fast machines
		// (DirectShow runs on its own threads, this just keeps the pump going)
		Sleep(1);
	}

	Output::Debug("Movie: Blocking playback loop ended.");
	movie_player.reset();
	movie_filename.clear();
#endif
}

static double interpolate(double d, double x0, double x1)
{
	return (x0 * (d - 1) + x1) / d;
}

void Game_Screen::StopWeather() {
	data.weather = Weather_None;
	OnWeatherChanged();
}

void Game_Screen::OnWeatherChanged() {
	int num_particles = Weather::GetMaxNumParticles(data.weather);

	InitParticles(num_particles);

	if (weather) {
		weather->OnWeatherChanged();
	}
}

void Game_Screen::InitParticles(int num_particles) {
	auto sz = static_cast<int>(particles.size());

	if (num_particles <= sz) {
		return;
	}

	particles.resize(num_particles);

	for (int i = sz; i < num_particles; ++i) {
		auto& p = particles[i];
		p.t = Rand::GetRandomNumber(0, 39);
		p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
		p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
	}
}

void Game_Screen::UpdateRain() {
	for (auto& p: particles) {
		if (p.t > 0) {
			--p.t;
			p.y += 4;
			p.x -= 1;
		} else if (Rand::PercentChance(10)) {
			p.t = 12;
			p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
			p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
		}
	}
}

void Game_Screen::UpdateSnow() {
	for (auto& p: particles) {
		if (p.t > 0) {
			--p.t;
			p.x -= Rand::GetRandomNumber(0, 1);
			p.y += Rand::GetRandomNumber(2, 3);
		} else if (Rand::PercentChance(5)) {
			p.t = 30;
			p.x = Rand::GetRandomNumber(0, GetPanLimitX() / 16 - 1);
			p.y = Rand::GetRandomNumber(0, GetPanLimitY() / 16 - 1);
		}
	}
}

void Game_Screen::UpdateFog() {
	++particles[0].x;
	++particles[1].x;
}

void Game_Screen::UpdateSandstorm() {
	constexpr auto epsilon = 1.0f / 128.0f;
	auto& rng = Rand::GetRNG();
	auto dist = std::uniform_real_distribution<float>(epsilon, M_PI - epsilon);

	UpdateFog();

	for (size_t i = 2; i < particles.size(); ++i) {
		auto& p = particles[i];
		if (p.t > 0) {
			--p.t;
			p.alpha += 2;
			p.x += static_cast<int>(p.vx);
			p.y += static_cast<int>(p.vy);
			p.vx += p.ax;
			p.vy += p.ay;
		} else if (Rand::PercentChance(10)) {
			p.t = 80;

			auto c = std::cos(dist(rng));
			auto s = std::sin(dist(rng));
			auto d = Rand::GetRandomNumber(16, 95);

			p.x = static_cast<int>(d * c * 2.0f) * Player::screen_width / 320 + Player::screen_width / 2;
			p.y = static_cast<int>(d * s) * Player::screen_height / 240;

			p.alpha = 180;
			p.vx = 0.0;
			p.vy = 0.0;
			p.ax = c * 2.0f * Player::screen_width / 320;
			p.ay = s * 2.0f * Player::screen_height / 240;
		}
	}
}

void Game_Screen::OnMapScrolled(int dx, int dy) {
	auto pan_limit_x = GetPanLimitX();
	auto pan_limit_y = GetPanLimitY();

	data.pan_x = (data.pan_x - dx + pan_limit_x) % pan_limit_x;
	data.pan_y = (data.pan_y - dy + pan_limit_y) % pan_limit_y;
}

void Game_Screen::UpdateScreenEffects() {
	if (data.tint_time_left > 0) {
		data.tint_current_red = interpolate(data.tint_time_left, data.tint_current_red, data.tint_finish_red);
		data.tint_current_green = interpolate(data.tint_time_left, data.tint_current_green, data.tint_finish_green);
		data.tint_current_blue = interpolate(data.tint_time_left, data.tint_current_blue, data.tint_finish_blue);
		data.tint_current_sat = interpolate(data.tint_time_left, data.tint_current_sat, data.tint_finish_sat);
		data.tint_time_left = data.tint_time_left - 1;
	}

	Flash::Update(data.flash_current_level,
			data.flash_time_left,
			data.flash_continuous,
			flash_period,
			flash_sat);

	Shake::Update(data.shake_position,
			data.shake_time_left,
			data.shake_strength,
			data.shake_speed,
			data.shake_continuous);
}

void Game_Screen::UpdateMovie() {
	// Now unused - blocking playback happens in PlayMovie()
}

void Game_Screen::UpdateWeather() {
	switch (data.weather) {
		case Weather_None:
			break;
		case Weather_Rain:
			UpdateRain();
			break;
		case Weather_Snow:
			UpdateSnow();
			break;
		case Weather_Fog:
			UpdateFog();
			break;
		case Weather_Sandstorm:
			UpdateSandstorm();
			break;
	}
}

void Game_Screen::Update() {
	UpdateScreenEffects();
	UpdateMovie();
	UpdateWeather();
	UpdateBattleAnimation();
}

int Game_Screen::ShowBattleAnimation(int animation_id, int target_id, bool global, int start_frame) {
	const lcf::rpg::Animation* anim = lcf::ReaderUtil::GetElement(lcf::Data::animations, animation_id);
	if (!anim) {
		Output::Warning("ShowBattleAnimation: Invalid battle animation ID {}", animation_id);
		return 0;
	}

	auto* chara = Game_Character::GetCharacter(target_id, target_id);
	if (!chara) {
		Output::Warning("ShowBattleAnimation: Invalid target event ID {}", target_id);
		CancelBattleAnimation();
		return 0;
	}

	data.battleanim_id = animation_id;
	data.battleanim_target = target_id;
	data.battleanim_global = global;
	data.battleanim_active = true;
	data.battleanim_frame = start_frame;

	animation.reset(new BattleAnimationMap(*anim, *chara, global));

	if (start_frame) {
		animation->SetFrame(start_frame);
	}

	return animation->GetFrames();
}

void Game_Screen::UpdateBattleAnimation() {
	if (animation) {
		if (!animation->IsDone()) {
			animation->Update();
			data.battleanim_frame = animation->GetFrame();
		}

		if (animation->IsDone() && !Game_Battle::IsBattleRunning()) {
			CancelBattleAnimation();
		}
	}
}

void Game_Screen::CancelBattleAnimation() {
	data.battleanim_frame = animation ?
		animation->GetFrames() : 0;
	data.battleanim_active = false;
	animation.reset();
}

void Game_Screen::UpdateUnderlyingEventReferences() {
	if (!IsBattleAnimationWaiting()) {
		return;
	}

	auto* chara = Game_Character::GetCharacter(data.battleanim_target, data.battleanim_target);
	if (!chara) {
		CancelBattleAnimation();
	} else {
		animation->SetTarget(*chara);
	}
}
