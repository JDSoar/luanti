// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "game_internal.h"

#include <cmath>
#include <csignal>
#include "client/gameui.h"
#include "client/inputhandler.h"
#include "client/texturepaths.h"
#include "client/keys.h"
#include "client/joystick_controller.h"
#include "client/mapblock_mesh.h"
#include "client/sound.h"
#include "clientmap.h"
#include "clientmedia.h" // For clientMediaUpdateCacheCopy
#include "mapblock.h"    // For getNodeBlockPos (used by split-screen safe-spawn search)
#include "servermap.h"   // For ServerMap::emergeBlock
#include "config.h"
#include "content_cao.h"
#include "content/subgames.h"
#include "client/event_manager.h"
#include "fontengine.h"
#include "itemdef.h"
#include "gameparams.h"
#include "gettext.h"
#include "gui/guiChatConsole.h"
#include "texturesource.h"
#include "gui/mainmenumanager.h"
#include "gui/profilergraph.h"
#include "localplayer.h"
#include "minimap.h"
#include "network/networkexceptions.h"
#include "nodedef.h"         // Needed for determining pointing to nodes
#include "nodemetadata.h"
#include "particles.h"
#include "porting.h"
#include "profiler.h"
#include "raycast.h"
#include "server.h"
#include "serverenvironment.h"
#include "server/player_sao.h"
#include "remoteplayer.h"
#include "settings.h"
#include "shader.h"
#include "sound_maker.h"
#include "threading/lambda.h"
#include "translation.h"
#include "util/basic_macros.h"
#include "util/directiontables.h"
#include "util/quicktune_shortcutter.h"
#include "version.h"
#include "script/scripting_client.h"
#include "hud.h"
#include <AnimatedMeshSceneNode.h>
#include <ICameraSceneNode.h>
#include "util/tracy_wrapper.h"
#include "item_visuals_manager.h"

#if USE_SOUND
	#include "client/sound/sound_openal.h"
#endif

typedef s32 SamplerLayer_t;


class GameGlobalShaderUniformSetter : public IShaderUniformSetter
{
	Sky *m_sky;
	Client *m_client;

	CachedVertexShaderSetting<float> m_animation_timer_vertex{"animationTimer"};
	CachedPixelShaderSetting<float> m_animation_timer_pixel{"animationTimer"};
	CachedVertexShaderSetting<float>
		m_animation_timer_delta_vertex{"animationTimerDelta"};
	CachedPixelShaderSetting<float>
		m_animation_timer_delta_pixel{"animationTimerDelta"};
	int m_crack_animation_length_i;
	CachedPixelShaderSetting<float> m_crack_animation_length{"crackAnimationLength"};
	int m_crack_level_i = -1;
	CachedPixelShaderSetting<float> m_crack_level{"crackLevel"};
	int m_crack_texture_scale_i = 0;
	CachedPixelShaderSetting<float> m_crack_texture_scale{"crackTextureScale"};
	CachedPixelShaderSetting<float, 3> m_day_light{"dayLight"};
	CachedPixelShaderSetting<float, 3> m_minimap_yaw{"yawVec"};
	CachedPixelShaderSetting<float, 3> m_camera_offset_pixel{"cameraOffset"};
	CachedVertexShaderSetting<float, 3> m_camera_offset_vertex{"cameraOffset"};
	CachedPixelShaderSetting<float, 3> m_camera_position_pixel{"cameraPosition"};
	CachedVertexShaderSetting<float, 3> m_camera_position_vertex{"cameraPosition"};
	CachedVertexShaderSetting<float, 2> m_texel_size0_vertex{"texelSize0"};
	CachedPixelShaderSetting<float, 2> m_texel_size0_pixel{"texelSize0"};
	v2f m_texel_size0;

	CachedStructPixelShaderSetting<float, 7> m_exposure_params_pixel{
		"exposureParams",
		std::array<const char*, 7> {
			"luminanceMin", "luminanceMax", "exposureCorrection",
			"speedDarkBright", "speedBrightDark", "centerWeightPower",
			"compensationFactor"
		}};
	float m_user_exposure_compensation;
	bool m_bloom_enabled;
	CachedPixelShaderSetting<float> m_bloom_intensity_pixel{"bloomIntensity"};
	CachedPixelShaderSetting<float> m_bloom_strength_pixel{"bloomStrength"};
	CachedPixelShaderSetting<float> m_bloom_radius_pixel{"bloomRadius"};
	CachedPixelShaderSetting<float> m_saturation_pixel{"saturation"};
	bool m_volumetric_light_enabled;
	CachedPixelShaderSetting<float, 3>
		m_sun_position_pixel{"sunPositionScreen"};
	CachedPixelShaderSetting<float> m_sun_brightness_pixel{"sunBrightness"};
	CachedPixelShaderSetting<float, 3>
		m_moon_position_pixel{"moonPositionScreen"};
	CachedPixelShaderSetting<float> m_moon_brightness_pixel{"moonBrightness"};
	CachedPixelShaderSetting<float>
		m_volumetric_light_strength_pixel{"volumetricLightStrength"};

	static constexpr std::array<const char*, 1> SETTING_CALLBACKS = {
		"exposure_compensation",
	};

public:
	void onSettingsChange(const std::string &name)
	{
		if (name == "exposure_compensation")
			m_user_exposure_compensation = g_settings->getFloat("exposure_compensation", -1.0f, 1.0f);
	}

	static void settingsCallback(const std::string &name, void *userdata)
	{
		reinterpret_cast<GameGlobalShaderUniformSetter*>(userdata)->onSettingsChange(name);
	}

	void setSky(Sky *sky) { m_sky = sky; }

	GameGlobalShaderUniformSetter(Sky *sky, Game *game) :
		m_sky(sky),
		m_client(game->getClient())
	{
		for (auto &name : SETTING_CALLBACKS)
			g_settings->registerChangedCallback(name, settingsCallback, this);

		m_user_exposure_compensation = g_settings->getFloat("exposure_compensation", -1.0f, 1.0f);
		m_bloom_enabled = g_settings->getBool("enable_bloom");
		m_volumetric_light_enabled = g_settings->getBool("enable_volumetric_lighting") && m_bloom_enabled;
		m_crack_animation_length_i = game->crack_animation_length;
	}

	~GameGlobalShaderUniformSetter()
	{
		g_settings->deregisterAllChangedCallbacks(this);
	}

	void onSetUniforms(video::IMaterialRendererServices *services) override
	{
		u32 daynight_ratio = (float)m_client->getEnv().getDayNightRatio();
		video::SColorf sunlight;
		get_sunlight_color(&sunlight, daynight_ratio);
		m_day_light.set(sunlight, services);

		u32 animation_timer = m_client->getEnv().getFrameTime() % 1000000;
		float animation_timer_f = (float)animation_timer / 100000.f;
		m_animation_timer_vertex.set(&animation_timer_f, services);
		m_animation_timer_pixel.set(&animation_timer_f, services);

		float animation_timer_delta_f = (float)m_client->getEnv().getFrameTimeDelta() / 100000.f;
		m_animation_timer_delta_vertex.set(&animation_timer_delta_f, services);
		m_animation_timer_delta_pixel.set(&animation_timer_delta_f, services);

		if (m_client->getMinimap()) {
			v3f minimap_yaw = m_client->getMinimap()->getYawVec();
			m_minimap_yaw.set(minimap_yaw, services);
		}

		v3f offset = intToFloat(m_client->getCamera()->getOffset(), BS);
		m_camera_offset_pixel.set(offset, services);
		m_camera_offset_vertex.set(offset, services);

		v3f camera_position = m_client->getCamera()->getPosition();
		m_camera_position_pixel.set(camera_position, services);
		m_camera_position_pixel.set(camera_position, services);

		m_texel_size0_vertex.set(m_texel_size0, services);
		m_texel_size0_pixel.set(m_texel_size0, services);

		{
			float tmp = m_crack_animation_length_i;
			m_crack_animation_length.set(&tmp, services);
			tmp = m_crack_level_i;
			m_crack_level.set(&tmp, services);
			tmp = m_crack_texture_scale_i;
			m_crack_texture_scale.set(&tmp, services);
		}

		const auto &lighting = m_client->getEnv().getLocalPlayer()->getLighting();

		const AutoExposure &exposure_params = lighting.exposure;
		std::array<float, 7> exposure_buffer = {
			std::pow(2.0f, exposure_params.luminance_min),
			std::pow(2.0f, exposure_params.luminance_max),
			exposure_params.exposure_correction,
			exposure_params.speed_dark_bright,
			exposure_params.speed_bright_dark,
			exposure_params.center_weight_power,
			powf(2.f, m_user_exposure_compensation)
		};
		m_exposure_params_pixel.set(exposure_buffer.data(), services);

		if (m_bloom_enabled) {
			float intensity = std::max(lighting.bloom_intensity, 0.0f);
			m_bloom_intensity_pixel.set(&intensity, services);
			float strength_factor = std::max(lighting.bloom_strength_factor, 0.0f);
			m_bloom_strength_pixel.set(&strength_factor, services);
			float radius = std::max(lighting.bloom_radius, 0.0f);
			m_bloom_radius_pixel.set(&radius, services);
		}

		float saturation = lighting.saturation;
		m_saturation_pixel.set(&saturation, services);

		if (m_volumetric_light_enabled) {
			// Map directional light to screen space
			auto camera_node = m_client->getCamera()->getCameraNode();
			core::matrix4 transform = camera_node->getProjectionMatrix();
			transform *= camera_node->getViewMatrix();

			if (m_sky->getSunVisible()) {
				v3f sun_position = camera_node->getAbsolutePosition() +
						10000.f * m_sky->getSunDirection();
				transform.transformVect(sun_position);
				sun_position.normalize();

				m_sun_position_pixel.set(sun_position, services);

				float sun_brightness = core::clamp(107.143f * m_sky->getSunDirection().Y, 0.f, 1.f);
				m_sun_brightness_pixel.set(&sun_brightness, services);
			} else {
				m_sun_position_pixel.set(v3f(0.f, 0.f, -1.f), services);

				float sun_brightness = 0.f;
				m_sun_brightness_pixel.set(&sun_brightness, services);
			}

			if (m_sky->getMoonVisible()) {
				v3f moon_position = camera_node->getAbsolutePosition() +
						10000.f * m_sky->getMoonDirection();
				transform.transformVect(moon_position);
				moon_position.normalize();

				m_moon_position_pixel.set(moon_position, services);

				float moon_brightness = core::clamp(107.143f * m_sky->getMoonDirection().Y, 0.f, 1.f);
				m_moon_brightness_pixel.set(&moon_brightness, services);
			} else {
				m_moon_position_pixel.set(v3f(0.f, 0.f, -1.f), services);

				float moon_brightness = 0.f;
				m_moon_brightness_pixel.set(&moon_brightness, services);
			}

			float volumetric_light_strength = lighting.volumetric_light_strength;
			m_volumetric_light_strength_pixel.set(&volumetric_light_strength, services);
		}
	}

	void onSetMaterial(const video::SMaterial &material) override
	{
		// This is set only for node materials which have a crack, see mapblock_mesh.cpp.
		auto pair = MapBlockMesh::unpackCrackMaterialParam(material.MaterialTypeParam);
		m_crack_level_i = pair.first;
		m_crack_texture_scale_i = pair.second;

		video::ITexture *texture = material.getTexture(0);
		if (texture) {
			core::dimension2du size = texture->getSize();
			m_texel_size0 = v2f(1.f / size.Width, 1.f / size.Height);
		} else {
			m_texel_size0 = v2f();
		}
	}
};


class GameGlobalShaderUniformSetterFactory : public IShaderUniformSetterFactory
{
	Sky *m_sky = nullptr;
	Game *m_game;
	std::vector<GameGlobalShaderUniformSetter*> created_nosky;
public:
	GameGlobalShaderUniformSetterFactory(Game *game) :
		m_game(game)
	{}

	void setSky(Sky *sky)
	{
		m_sky = sky;
		for (GameGlobalShaderUniformSetter *ggscs : created_nosky) {
			ggscs->setSky(m_sky);
		}
		created_nosky.clear();
	}

	virtual IShaderUniformSetter* create(const std::string &name)
	{
		if (str_starts_with(name, "shadow/"))
			return nullptr;
		auto *scs = new GameGlobalShaderUniformSetter(m_sky, m_game);
		if (!m_sky)
			created_nosky.push_back(scs);
		return scs;
	}
};

class NodeShaderConstantSetter : public IShaderConstantSetter
{
public:
	NodeShaderConstantSetter() = default;
	~NodeShaderConstantSetter() = default;

	void onGenerate(const std::string &name, ShaderConstants &constants) override
	{
		if (constants.find("MATERIAL_TYPE") == constants.end())
			return; // not a node shader
		[[maybe_unused]] const auto material_type =
			static_cast<MaterialType>(std::get<int>(constants["MATERIAL_TYPE"]));

#define PROVIDE(constant) constants[ #constant ] = (int)constant

		PROVIDE(TILE_MATERIAL_BASIC);
		PROVIDE(TILE_MATERIAL_ALPHA);
		PROVIDE(TILE_MATERIAL_LIQUID_TRANSPARENT);
		PROVIDE(TILE_MATERIAL_LIQUID_OPAQUE);
		PROVIDE(TILE_MATERIAL_WAVING_LEAVES);
		PROVIDE(TILE_MATERIAL_WAVING_PLANTS);
		PROVIDE(TILE_MATERIAL_OPAQUE);
		PROVIDE(TILE_MATERIAL_WAVING_LIQUID_BASIC);
		PROVIDE(TILE_MATERIAL_WAVING_LIQUID_TRANSPARENT);
		PROVIDE(TILE_MATERIAL_WAVING_LIQUID_OPAQUE);
		PROVIDE(TILE_MATERIAL_PLAIN);
		PROVIDE(TILE_MATERIAL_PLAIN_ALPHA);

#undef PROVIDE

		bool enable_waving_water = g_settings->getBool("enable_waving_water");
		constants["ENABLE_WAVING_WATER"] = enable_waving_water ? 1 : 0;
		if (enable_waving_water) {
			constants["WATER_WAVE_HEIGHT"] = g_settings->getFloat("water_wave_height");
			constants["WATER_WAVE_LENGTH"] = g_settings->getFloat("water_wave_length");
			constants["WATER_WAVE_SPEED"] = g_settings->getFloat("water_wave_speed");
		}
		switch (material_type) {
			case TILE_MATERIAL_WAVING_LIQUID_TRANSPARENT:
			case TILE_MATERIAL_WAVING_LIQUID_OPAQUE:
			case TILE_MATERIAL_WAVING_LIQUID_BASIC:
				constants["MATERIAL_WAVING_LIQUID"] = 1;
				break;
			default:
				constants["MATERIAL_WAVING_LIQUID"] = 0;
				break;
		}
		switch (material_type) {
			case TILE_MATERIAL_WAVING_LIQUID_TRANSPARENT:
			case TILE_MATERIAL_WAVING_LIQUID_OPAQUE:
			case TILE_MATERIAL_WAVING_LIQUID_BASIC:
			case TILE_MATERIAL_LIQUID_TRANSPARENT:
				constants["MATERIAL_WATER_REFLECTIONS"] = 1;
				break;
			default:
				constants["MATERIAL_WATER_REFLECTIONS"] = 0;
				break;
		}

		constants["ENABLE_WAVING_LEAVES"] = g_settings->getBool("enable_waving_leaves") ? 1 : 0;
		constants["ENABLE_WAVING_PLANTS"] = g_settings->getBool("enable_waving_plants") ? 1 : 0;
	}
};

/****************************************************************************
 ****************************************************************************/

Game::Game() :
	m_chat_log_buf(g_logger),
	m_game_ui(new GameUI())
{
	clearTextureNameCache();

	const char *settings[] = {
		"chat_log_level", "doubletap_jump", "toggle_sneak_key", "toggle_aux1_key",
		"enable_joysticks", "enable_fog", "mouse_sensitivity", "joystick_frustum_sensitivity",
		"repeat_place_time", "repeat_dig_time", "noclip", "free_move", "fog_start",
		"cinematic", "cinematic_camera_smoothing", "camera_smoothing", "invert_mouse",
		"enable_hotbar_mouse_wheel", "invert_hotbar_mouse_wheel", "pause_on_lost_focus",
		"keyboard_camera_speed",
	};
	for (auto s : settings)
		g_settings->registerChangedCallback(s, &settingChangedCallback, this);

	readSettings();
}


Game::~Game()
{
	delete client;
	soundmaker.reset();
	sound_manager.reset();

	delete server;

	delete hud;
	delete camera;
	delete quicktune;
	delete eventmgr;
	delete texture_src;
	delete shader_src;
	delete nodedef_manager;
	delete itemdef_manager;
	delete draw_control;

	clearTextureNameCache();

	g_settings->deregisterAllChangedCallbacks(this);

	if (m_rendering_engine)
		m_rendering_engine->finalize();
}

bool Game::startup(volatile std::sig_atomic_t *kill,
		InputHandler *input,
		RenderingEngine *rendering_engine,
		const GameStartData &start_data,
		std::string &error_message,
		bool *reconnect,
		ChatBackend *chat_backend)
{

	// "cache"
	m_rendering_engine        = rendering_engine;
	device                    = m_rendering_engine->get_raw_device();
	this->kill                = kill;
	this->error_message       = &error_message;
	reconnect_requested       = reconnect;
	this->input               = input;
	this->chat_backend        = chat_backend;
	simple_singleplayer_mode  = start_data.isSinglePlayer();

	input->reloadKeybindings();

	driver = device->getVideoDriver();
	smgr = m_rendering_engine->get_scene_manager();

	driver->setTextureCreationFlag(video::ETCF_CREATE_MIP_MAPS, g_settings->getBool("mip_map"));

	// Reinit runData
	runData = GameRunData();
	runData.time_from_last_punch = 10.0;

	m_game_ui->initFlags();
	if (g_settings->getBool("show_debug")) {
		m_flags.debug_state = 1;
		m_game_ui->m_flags.show_minimal_debug = true;
	}

	m_first_loop_after_window_activation = true;

	g_client_translations->clear();

	if (!init(start_data.world_spec.path, start_data.address,
			start_data.socket_port, start_data.game_spec))
		return false;

	// Couch / split-screen: when we're running the embedded server in simple
	// singleplayer mode but want >1 local seat, lift its hard "1 client max"
	// cap so the additional in-process clients can actually join.
	if (server && simple_singleplayer_mode && start_data.splitscreen_enable) {
		server->setSimpleSingleplayerMaxSeats(
			rangelim<u16>(start_data.splitscreen_seats, 1, 4));
	}

	if (!createClient(start_data))
		return false;

	m_rendering_engine->initialize(client, hud);

	m_game_formspec.init(client, m_rendering_engine, input);

	return true;
}


void Game::run()
{
	ZoneScoped;

	ProfilerGraph graph;
	RunStats stats = {};
	CameraOrientation cam_view_target = {};
	CameraOrientation cam_view = {};
	FpsControl draw_times;
	f32 dtime; // in seconds

	// Clear the profiler
	{
		Profiler::GraphValues dummyvalues;
		g_profiler->graphPop(dummyvalues);
	}

	draw_times.reset();

	set_light_curve(g_settings->getFloat("display_gamma"));

	m_touch_simulate_aux1 = g_settings->getBool("fast_move")
			&& client->checkPrivilege("fast");

	const core::dimension2du initial_screen_size(
			g_settings->getU16("screen_w"),
			g_settings->getU16("screen_h")
		);
	const bool initial_window_maximized = !g_settings->getBool("fullscreen") &&
			g_settings->getBool("window_maximized");

#ifdef __ANDROID__
	porting::setPlayingNowNotification(true);
#endif

	auto framemarker = FrameMarker("Game::run()-frame").started();

	while (m_rendering_engine->run()
			&& !(*kill || g_gamecallback->shutdown_requested
			|| (server && server->isShutdownRequested()))) {

		framemarker.end();

		// Calculate dtime =
		//    m_rendering_engine->run() from this iteration
		//  + Sleep time until the wanted FPS are reached
		draw_times.limit(device, &dtime);

		framemarker.start();

		g_fontengine->handleReload();

		const auto current_dynamic_info = ClientDynamicInfo::getCurrent();
		if (!current_dynamic_info.equal(client_display_info)) {
			client_display_info = current_dynamic_info;
			dynamic_info_send_timer = 0.2f;
		}

		if (dynamic_info_send_timer > 0.0f) {
			dynamic_info_send_timer -= dtime;
			if (dynamic_info_send_timer <= 0.0f) {
				client->sendUpdateClientInfo(current_dynamic_info);
			}
		}

		// Prepare render data for next iteration

		updateStats(&stats, draw_times, dtime);
		updateInteractTimers(dtime);

		if (!checkConnection())
			break;
		if (!m_game_formspec.handleCallbacks())
			break;

		processQueues();

		m_game_ui->clearInfoText();

		updateProfilers(stats, draw_times, dtime);

		// Update camera offset once before doing anything.
		// In contrast to other updates the latency of this doesn't matter,
		// since it's invisible to the user. But it needs to be consistent.
		updateCameraOffset();

		processUserInput(dtime);
		// Update camera before player movement to avoid camera lag of one frame
		updateCameraDirection(&cam_view_target, dtime);
		if (m_cache_cam_smoothing <= 0.0f) {
			cam_view.camera_yaw = cam_view_target.camera_yaw;
			cam_view.camera_pitch = cam_view_target.camera_pitch;
		} else {
			f32 cam_damp_lambda = 1.0f / m_cache_cam_smoothing * dtime;
			cam_view.camera_yaw = damp(
					cam_view.camera_yaw,
					cam_view_target.camera_yaw,
					cam_damp_lambda
			);
			cam_view.camera_pitch = damp(
					cam_view.camera_pitch,
					cam_view_target.camera_pitch,
					cam_damp_lambda
			);
		}
		// Seat 0 uses the mouse/keyboard-driven cam_view from above. Keep
		// SeatRuntime[0]'s copy in sync so any code that reads it later
		// (HUD layout, debug overlay, etc.) sees the same orientation.
		m_seats[0].cam_view = cam_view;
		if (m_game_formspec.isSeatMenuActive(0))
			applyIdlePlayerControlForOpenMenu(cam_view);
		else
			updatePlayerControl(cam_view);
		// Split-screen: apply control for additional seats. Each extra seat
		// gets its OWN cam_view, driven by its gamepad's right stick (the
		// "frustum" axes), so seat 0's mouse never rotates seat 1's player
		// or vice-versa.
		if (m_splitscreen_seats > 1) {
			InputHandler *saved_input = input;
			Client *saved_client = client;
			const f32 sens_scale = getSensitivityScaleFactor();
			const f32 stick_rate =
					m_cache_joystick_frustum_sensitivity * dtime * sens_scale;
			const f32 key_rate =
					m_cache_keyboard_camera_speed * dtime * sens_scale;
			for (u8 i = 1; i < m_splitscreen_seats; i++) {
				if (!m_seats[i].client || !m_seats[i].input)
					continue;

				CameraOrientation &sv = m_seats[i].cam_view;
				InputHandler *seat_in = m_seats[i].input.get();

				// Don't rotate the camera from the right stick while this
				// seat has a formspec open — that input drives the menu cursor.
				if (!m_game_formspec.isSeatMenuActive(i)) {
					// Right-stick look (gamepad).
					if (m_cache_enable_joysticks) {
						sv.camera_yaw -=
							seat_in->joystick.getAxisWithoutDead(
								JA_FRUSTUM_HORIZONTAL) * stick_rate;
						sv.camera_pitch +=
							seat_in->joystick.getAxisWithoutDead(
								JA_FRUSTUM_VERTICAL) * stick_rate;
					}
					// Keybind-mapped look (keyboard `keymap_camera_*`).
					// The Xbox D-pad no longer maps here — it is reserved
					// for Minecraft-style chat / minimap / hotbar cycling.
					if (seat_in->isKeyDown(KeyType::CAMERA_YAW_LEFT))
						sv.camera_yaw += key_rate;
					if (seat_in->isKeyDown(KeyType::CAMERA_YAW_RIGHT))
						sv.camera_yaw -= key_rate;
					if (seat_in->isKeyDown(KeyType::CAMERA_PITCH_UP))
						sv.camera_pitch -= key_rate;
					if (seat_in->isKeyDown(KeyType::CAMERA_PITCH_DOWN))
						sv.camera_pitch += key_rate;
				}
				sv.camera_pitch = rangelim(sv.camera_pitch, -89, 89);

				client = m_seats[i].client;
				input = seat_in;
				if (m_game_formspec.isSeatMenuActive(i))
					applyIdlePlayerControlForOpenMenu(sv);
				else
					updatePlayerControl(sv);
			}
			client = saved_client;
			input = saved_input;
		}

		updatePauseState();
		if (m_is_paused)
			dtime = 0.0f;

		step(dtime);

		processClientEvents(&cam_view_target);
		updateDebugState();
		// Update camera here so it is in-sync with CAO position
		updateCamera(dtime);
		updateSound(dtime);
		if (!m_game_formspec.isSeatMenuActive(0))
			processPlayerInteraction(dtime, m_game_ui->m_flags.show_hud);
		// Split-screen: also let extra seats interact (dig / place / use).
		// Without this, only seat 0's triggers do anything in the world -
		// seats 1+ receive their joystick events into per-seat
		// JoystickControllers, but the function that actually consumes
		// DIG / PLACE state runs solely against the global `client` /
		// `camera` / `hud` / `input`. Reuse it by swapping those globals
		// to each seat in turn (matches the pattern already used for
		// updatePlayerControl above).
		//
		// We std::swap the seat's per-seat runData in too, so each player
		// gets independent dig progress, their own pointed_old (and
		// therefore their own selection-box halo), independent
		// btn_down_for_dig / repeat_place_timer, etc. Without this the
		// previous seat's "I'm currently digging block A" state would
		// bleed into the next seat's frame and either interrupt the
		// digger or leave runData.digging stuck across players.
		if (m_splitscreen_seats > 1) {
			InputHandler *saved_input = input;
			Client *saved_client = client;
			Camera *saved_camera = camera;
			Hud *saved_hud = hud;
			for (u8 i = 1; i < m_splitscreen_seats; i++) {
				if (!m_seats[i].client || !m_seats[i].camera ||
						!m_seats[i].hud || !m_seats[i].input)
					continue;
				if (m_game_formspec.isSeatMenuActive(i))
					continue;
				client = m_seats[i].client;
				camera = m_seats[i].camera;
				hud    = m_seats[i].hud;
				input  = m_seats[i].input.get();
				std::swap(runData, m_seats[i].run_data);
				processPlayerInteraction(dtime, false);
				std::swap(runData, m_seats[i].run_data);
			}
			input  = saved_input;
			client = saved_client;
			camera = saved_camera;
			hud    = saved_hud;
		}
		updateFrame(&graph, &stats, dtime, cam_view);
		updateProfilerGraphs(&graph);

		if (m_does_lost_focus_pause_game && !device->isWindowFocused() && !isMenuActive()) {
			m_game_formspec.showPauseMenu();
		}
	}

	framemarker.end();

#ifdef __ANDROID__
	porting::setPlayingNowNotification(false);
#endif

	RenderingEngine::autosaveScreensizeAndCo(initial_screen_size, initial_window_maximized);
}


void Game::shutdown()
{
	// Delete text and menus first
	m_game_ui->clearText();
	m_game_formspec.reset();
	while (g_menumgr.menuCount() > 0) {
		g_menumgr.deleteFront();
	}

	if (g_touchcontrols)
		g_touchcontrols->hide();

	// Restore normal mouse cursor
	auto *cur_control = device->getCursorControl();
	if (cur_control) {
		cur_control->setVisible(true);
		cur_control->setRelativeMode(false);
	}

	clouds.reset();

	gui_chat_console.reset();

	sky.reset();

	// only if the shutdown progress bar isn't shown yet
	if (m_shutdown_progress == 0.0f)
		showOverlayMessage(N_("Shutting down..."), 0, 0);

	chat_backend->addMessage(L"", L"# Disconnected.");
	chat_backend->addMessage(L"", L"");

	// Stop all seats
	for (u8 i = 0; i < m_splitscreen_seats; i++) {
		if (!m_seats[i].client)
			continue;
		m_seats[i].client->Stop();
		while (!m_seats[i].client->isShutdown()) {
			assert(texture_src != NULL);
			assert(shader_src != NULL);
			texture_src->processQueue();
			shader_src->processQueue();
			sleep_ms(100);
		}
	}

	for (u8 i = 0; i < m_splitscreen_seats; i++) {
		delete m_seats[i].hud;
		m_seats[i].hud = nullptr;
		delete m_seats[i].camera;
		m_seats[i].camera = nullptr;
		delete m_seats[i].client;
		m_seats[i].client = nullptr;
		m_seats[i].scene_root = nullptr;
		m_seats[i].input.reset();
		if (m_seats[i].render_tex) {
			driver->removeTexture(m_seats[i].render_tex);
			m_seats[i].render_tex = nullptr;
		}
	}

	client = nullptr;
	camera = nullptr;
	hud = nullptr;
	client = nullptr;
	soundmaker.reset();
	sound_manager.reset();

	auto stop_thread = runInThread([=] {
		delete server;
		server = nullptr;
	}, "ServerStop");

	FpsControl fps_control;
	fps_control.reset();

	while (stop_thread->isRunning()) {
		m_rendering_engine->run();
		f32 dtime;
		fps_control.limit(device, &dtime);
		showOverlayMessage(N_("Shutting down..."), dtime, 0, &m_shutdown_progress);
	}

	stop_thread->rethrow();

	// to be continued in Game::~Game
}


/****************************************************************************/
/****************************************************************************
 Startup
 ****************************************************************************/
/****************************************************************************/

bool Game::init(
		const std::string &map_dir,
		const std::string &address,
		u16 port,
		const SubgameSpec &gamespec)
{
	texture_src = createTextureSource();

	showOverlayMessage(N_("Loading..."), 0, 0);

	shader_src = createShaderSource();

	itemdef_manager = createItemDefManager();
	nodedef_manager = createNodeDefManager();

	m_item_visuals_manager = std::make_unique<ItemVisualsManager>();

	eventmgr = new EventManager();
	quicktune = new QuicktuneShortcutter();

	if (!(texture_src && shader_src && itemdef_manager && nodedef_manager
			&& eventmgr && quicktune))
		return false;

	if (!initSound())
		return false;

	// Create a server if not connecting to an existing one
	if (address.empty()) {
		if (!createServer(map_dir, gamespec, port))
			return false;
	}

	return true;
}

bool Game::initSound()
{
#if USE_SOUND
	if (g_sound_manager_singleton.get()) {
		infostream << "Attempting to use OpenAL audio" << std::endl;
		sound_manager = createOpenALSoundManager(g_sound_manager_singleton.get(),
				std::make_unique<SoundFallbackPathProvider>());
		if (!sound_manager)
			infostream << "Failed to initialize OpenAL audio" << std::endl;
	} else {
		infostream << "Sound disabled." << std::endl;
	}
#endif

	if (!sound_manager) {
		infostream << "Using dummy audio." << std::endl;
		sound_manager = std::make_unique<DummySoundManager>();
	}

	soundmaker = std::make_unique<SoundMaker>(sound_manager.get(), nodedef_manager);
	soundmaker->registerReceiver(eventmgr);

	return true;
}

bool Game::createServer(const std::string &map_dir,
		const SubgameSpec &gamespec, u16 port)
{
	showOverlayMessage(N_("Creating server..."), 0, 5);

	std::string bind_str;
	if (simple_singleplayer_mode) {
		// Make the simple singleplayer server only accept connections from localhost,
		// which also makes Windows Defender not show a warning.
		bind_str = "127.0.0.1";
	} else {
		bind_str = g_settings->get("bind_address");
	}

	Address bind_addr(0, 0, 0, 0, port);

	if (g_settings->getBool("ipv6_server"))
		bind_addr.setAddress(static_cast<IPv6AddressBytes*>(nullptr));
	try {
		bind_addr.Resolve(bind_str.c_str());
	} catch (const ResolveError &e) {
		warningstream << "Resolving bind address \"" << bind_str
			<< "\" failed: " << e.what()
			<< " -- Listening on all addresses." << std::endl;
	}
	if (bind_addr.isIPv6() && !g_settings->getBool("enable_ipv6")) {
		*error_message = fmtgettext("Unable to listen on %s because IPv6 is disabled",
			bind_addr.serializeString().c_str());
		errorstream << *error_message << std::endl;
		return false;
	}

	server = new Server(map_dir, gamespec, simple_singleplayer_mode, bind_addr,
			false, nullptr, error_message);

	auto start_thread = runInThread([=] {
		server->start();
		copyServerClientCache();
	}, "ServerStart");

	input->clear();
	bool success = true;

	FpsControl fps_control;
	fps_control.reset();

	while (start_thread->isRunning()) {
		if (!m_rendering_engine->run() || input->cancelPressed())
			success = false;
		f32 dtime;
		fps_control.limit(device, &dtime);

		if (success)
			showOverlayMessage(N_("Creating server..."), dtime, 5);
		else
			showOverlayMessage(N_("Shutting down..."), dtime, 0, &m_shutdown_progress);
	}

	start_thread->rethrow();

	return success;
}

void Game::copyServerClientCache()
{
	// It would be possible to let the client directly read the media files
	// from where the server knows they are. But aside from being more complicated
	// it would also *not* fill the media cache and cause slower joining of
	// remote servers.
	// (Imagine that you launch a game once locally and then connect to a server.)

	assert(server);
	auto map = server->getMediaList();
	u32 n = 0;
	for (auto &it : map) {
		assert(it.first.size() == 20); // SHA1
		if (clientMediaUpdateCacheCopy(it.first, it.second))
			n++;
	}
	infostream << "Copied " << n << " files directly from server to client cache"
		<< std::endl;
}

bool Game::createClient(const GameStartData &start_data)
{
	showOverlayMessage(N_("Creating client..."), 0, 10);

	draw_control = new MapDrawControl();
	if (!draw_control)
		return false;

	bool could_connect, connect_aborted;
	if (!connectToServer(start_data, &could_connect, &connect_aborted))
		return false;

	if (!could_connect) {
		if (error_message->empty() && !connect_aborted) {
			// Should not happen if error messages are set properly
			*error_message = gettext("Connection failed for unknown reason");
			errorstream << *error_message << std::endl;
		}
		return false;
	}

	if (!getServerContent(&connect_aborted)) {
		if (error_message->empty() && !connect_aborted) {
			// Should not happen if error messages are set properly
			*error_message = gettext("Connection failed for unknown reason");
			errorstream << *error_message << std::endl;
		}
		return false;
	}

	// Pre-calculate crack length
	{
		auto size = texture_src->getTextureDimensions("crack_anylength.png");
		if (size.Width && size.Height)
			crack_animation_length = size.Height / size.Width;
		else
			crack_animation_length = 5;
	}

	shader_src->addShaderConstantSetter(
		std::make_unique<NodeShaderConstantSetter>());

	auto scsf_up = std::make_unique<GameGlobalShaderUniformSetterFactory>(this);
	auto* scsf = scsf_up.get();
	shader_src->addShaderUniformSetterFactory(std::move(scsf_up));

	shader_src->addShaderUniformSetterFactory(
		std::make_unique<FogShaderUniformSetterFactory>());

	ShadowRenderer::preInit(shader_src);

	// Update cached textures, meshes and materials
	client->afterContentReceived();

	/* Camera
	 */
	camera = new Camera(*draw_control, client, m_rendering_engine);
	if (client->modsLoaded())
		client->getScript()->on_camera_ready(camera);
	client->setCamera(camera);

	/* Clouds
	 */
	clouds = make_irr<Clouds>(smgr, shader_src, -1, myrand());

	/* Skybox
	 */
	sky = make_irr<Sky>(-1, m_rendering_engine, texture_src, shader_src);
	scsf->setSky(sky.get());

	if (!initGui())
		return false;

	/* Set window caption
	 */
	auto driver_name = driver->getName();
	std::string str = std::string(PROJECT_NAME_C) +
			" " + g_version_hash + " [";
	str += simple_singleplayer_mode ? gettext("Singleplayer")
			: gettext("Multiplayer");
	str += "] [";
	str += driver_name;
	str += "]";

	device->setWindowCaption(utf8_to_wide(str).c_str());

	LocalPlayer *player = client->getEnv().getLocalPlayer();
	player->hurt_tilt_timer = 0;
	player->hurt_tilt_strength = 0;

	hud = new Hud(client, player, &player->inventory);

	mapper = client->getMinimap();

	if (mapper && client->modsLoaded())
		client->getScript()->on_minimap_ready(mapper);

	// Split-screen additional seats (MVP: sequential connect, no extra GUI).
	m_splitscreen_seats = start_data.splitscreen_enable ?
			rangelim<u8>(start_data.splitscreen_seats, 1, 4) : 1;
	m_seats[0].client = client;
	m_seats[0].camera = camera;
	m_seats[0].hud = hud;
	m_seats[0].scene_root = client->getSceneRoot();
	m_seats[0].input.reset();

	// Couch / online: spin up extra local clients whenever we have an embedded
	// server (simple singleplayer, etc.) or a normal multiplayer destination.
	if (m_splitscreen_seats >= 2 &&
			(server || !start_data.isSinglePlayer())) {
		auto *receiver = dynamic_cast<RealInputHandler *>(input) ?
				static_cast<RealInputHandler *>(input)->getReceiver() : nullptr;

		// Resolve address once for additional seats (mirrors Game::connectToServer).
		Address connect_address(0, 0, 0, 0, start_data.socket_port);
		Address fallback_address;
		try {
			connect_address.Resolve(start_data.address.c_str(), &fallback_address);
			if (connect_address.isAny()) {
				if (connect_address.isIPv6()) {
					IPv6AddressBytes addr_bytes;
					addr_bytes.bytes[15] = 1;
					connect_address.setAddress(&addr_bytes);
				} else {
					connect_address.setAddress(127, 0, 0, 1);
				}
			}
		} catch (...) {
			// If seat0 connected, resolution already worked; keep going with any() addr.
		}

		for (u8 i = 1; i < m_splitscreen_seats; i++) {
			std::string pname = start_data.splitscreen_names[i];
			if (pname.empty())
				pname = "Player" + itos(static_cast<s32>(i + 1));

			// Per-seat scene manager. createNewSceneManager(false) shares
			// the video driver and mesh cache with the engine's main
			// scene manager but gives this seat a fully independent
			// scene tree (its own root, its own active camera, its own
			// CAOs). This is what makes split-screen "just work" - each
			// seat draws its own world without seeing other seats'
			// players, attachments, etc., so we don't need any visibility
			// hacks. Client takes a reference (via grab()) and drops it
			// in its destructor.
			scene::ISceneManager *seat_smgr = smgr->createNewSceneManager(false);
			m_seats[i].scene_root = seat_smgr->getRootSceneNode();

			Client *c = nullptr;
			try {
				c = new Client(pname.c_str(),
						start_data.splitscreen_passwords[i],
						*draw_control, texture_src, shader_src,
						itemdef_manager, nodedef_manager, sound_manager.get(), eventmgr,
						m_rendering_engine,
						m_item_visuals_manager.get(),
						start_data.allow_login_or_register,
						seat_smgr,
						(s32)(666 + i));
			} catch (const BaseException &e) {
				// Best-effort: keep seat0 playable.
				warningstream << "Split-screen: failed creating seat " << int(i)
						<< " client: " << e.what() << std::endl;
				seat_smgr->drop();
				m_seats[i] = SeatRuntime{};
				continue;
			}

			// Bind controller for this seat: seat i uses physical joystick index i
			// so players 1–4 map to devices 0–3 without overlapping seat 0.
			if (receiver) {
				auto seat_input = std::make_unique<GamepadInputHandler>(receiver, i);
				if (RealInputHandler *primary_in =
								dynamic_cast<RealInputHandler *>(input)) {
					// Secondary JoystickControllers skip ClientLauncher::
					// onJoystickConnect(); without a layout copy they keep an
					// empty/uninitialized binding map — no buttons or sticks.
					seat_input->joystick.copyLayoutFrom(primary_in->joystick);
				}
				m_seats[i].input = std::move(seat_input);
			}

			c->m_internal_server = !!server;
			c->m_simple_singleplayer_mode = simple_singleplayer_mode;

			// Mark this seat as a secondary that shares all content
			// with the primary (seat 0). This MUST happen before
			// connect() / step() runs for this seat, otherwise the
			// TOCLIENT_NODEDEF / TOCLIENT_ITEMDEF / TOCLIENT_*MEDIA*
			// handlers will mutate the primary's still-in-use shared
			// NodeDefManager / IItemDefManager / TextureSource and
			// crash the primary's mesh thread (use-after-free in
			// MapblockMeshGenerator::drawSolidNode reading
			// `f2.visuals`). See Client::setSharesContentWithPrimary.
			//
			// Passing the primary client also copies its per-Client
			// m_mesh_data cache so getMesh("character.b3d") etc.
			// resolves on this seat (m_mesh_data is per-Client, not
			// shared like the TextureSource).
			c->setSharesContentWithPrimary(client);

			// Create + bind the seat's Camera *before* connecting, so the
			// local player CAO that arrives during the handshake sees a
			// valid Client::getCamera() and can hide its own mesh from
			// this seat's first-person view. Doing this after connect()
			// loses the race: addToScene() -> updateMeshCulling() runs
			// while m_camera is still null, so the seat's own player
			// model never gets culled and ends up filling the camera.
			auto *cam = new Camera(*draw_control, c, m_rendering_engine);
			c->setCamera(cam);
			m_seats[i].camera = cam;

			c->connect(connect_address, start_data.address);

			// Register this seat's client immediately so Game::step() (and the
			// inner loops here) actually advance it. Without this, the client
			// never processes incoming packets and we hang forever on the
			// seat 0 "Done!" loading screen because mediaReceived() etc. stay
			// false.
			m_seats[i].client = c;

			// Temporarily make the global `client` pointer reference this seat
			// so Game::getServerContent() / Game::checkConnection() inspect
			// the right Client instance during the wait phases below.
			Client *old_client = client;
			client = c;

			{
				FpsControl fps_control;
				f32 dtime;
				fps_control.reset();
				float wait_time = 0.f;
				while (m_rendering_engine->run()) {
					fps_control.limit(device, &dtime);
					// Step *all* seats: keeps seat 0 alive while this seat
					// progresses through the handshake.
					step(dtime);
					if (c->getState() == LC_Init)
						break;
					if (input->cancelPressed())
						break;
					wait_time += dtime;
					if (!server && wait_time > GAME_CONNECTION_TIMEOUT)
						break;
				}
			}

			// Fetch server content for this seat (may no-op due to cache).
			bool aborted = false;
			(void)getServerContent(&aborted);

			// IMPORTANT: pass false here. Additional seats share the
			// primary's TextureSource / ShaderSource / NodeDefManager.
			// Letting them rebuild those again would invalidate every
			// texture pointer cached in the already-built MapBlockMesh
			// materials of seats 0..i-1 and crash the renderer next
			// frame. See Client::afterContentReceived() for details.
			c->afterContentReceived(false);

			// Restore the primary client pointer.
			client = old_client;

			auto *lp = c->getEnv().getLocalPlayer();
			auto *h = new Hud(c, lp, &lp->inventory);

			// Camera defaults to first-person (matches Camera's own
			// default + seat 0). First-person also gives each seat the
			// wielded item / arm overlay (only rendered in
			// CAMERA_MODE_FIRST).

			// Belt-and-suspenders: re-apply local-player mesh culling now
			// that everything is wired up. addToScene() already does this
			// during the handshake (see the early Camera setup above), but
			// re-running here is cheap and protects against any ordering
			// quirks that leave the seat's own player mesh visible.
			if (lp) {
				if (GenericCAO *pcao = lp->getCAO()) {
					pcao->updateMeshCulling();
					pcao->setChildrenVisible(
							cam->getCameraMode() > CAMERA_MODE_FIRST);
				}
			}

			m_seats[i].hud = h;

			// Mirror the GameRunData init that Game::createClient() does
			// for seat 0 (see `runData = GameRunData(); runData.
			// time_from_last_punch = 10.0;` near the top of this method).
			// SeatRuntime::run_data is value-initialised to all zeros at
			// SeatRuntime construction, but time_from_last_punch must
			// start at a "long ago" sentinel so the first punch isn't
			// mis-counted as part of a chain.
			m_seats[i].run_data = GameRunData();
			m_seats[i].run_data.time_from_last_punch = 10.0;
		}

		// Align seat 0 with joystick 0 so it never shares a device with seat 1+
		// (the `joystick_id` setting may otherwise point seat 0 elsewhere).
		auto *primary_input = dynamic_cast<RealInputHandler *>(input);
		if (primary_input)
			primary_input->joystick.setJoystickId(0);
	}

	return true;
}

bool Game::shouldShowTouchControls()
{
	if (!device->supportsTouchEvents())
		return false;

	const std::string &touch_controls = g_settings->get("touch_controls");
	if (touch_controls == "auto")
		return RenderingEngine::getLastPointerType() == PointerType::Touch;
	return is_yes(touch_controls);
}

bool Game::initGui()
{
	m_game_ui->init();

	// Remove stale "recent" chat messages from previous connections
	chat_backend->clearRecentChat();

	// Make sure the size of the recent messages buffer is right
	chat_backend->applySettings();

	// Chat backend and console
	gui_chat_console = make_irr<GUIChatConsole>(guienv, guienv->getRootGUIElement(),
			-1, chat_backend, client, &g_menumgr);

	if (shouldShowTouchControls())
		g_touchcontrols = new TouchControls(device, texture_src);

	return true;
}

bool Game::connectToServer(const GameStartData &start_data,
		bool *connect_ok, bool *connection_aborted)
{
	*connect_ok = false;	// Let's not be overly optimistic
	*connection_aborted = false;
	const auto &address_name = start_data.address;

	showOverlayMessage(N_("Resolving address..."), 0, 15);

	Address connect_address(0, 0, 0, 0, start_data.socket_port);
	Address fallback_address;

	try {
		connect_address.Resolve(address_name.c_str(), &fallback_address);

		if (connect_address.isAny()) {
			// replace with localhost IP
			if (connect_address.isIPv6()) {
				IPv6AddressBytes addr_bytes;
				addr_bytes.bytes[15] = 1;
				connect_address.setAddress(&addr_bytes);
			} else {
				connect_address.setAddress(127, 0, 0, 1);
			}
		}
	} catch (ResolveError &e) {
		*error_message = fmtgettext("Couldn't resolve address: %s", e.what());

		errorstream << *error_message << std::endl;
		return false;
	}

	// this shouldn't normally happen since Address::Resolve() checks for enable_ipv6
	if (g_settings->getBool("enable_ipv6")) {
		// empty
	} else if (connect_address.isIPv6()) {
		*error_message = fmtgettext("Unable to connect to %s because IPv6 is disabled", connect_address.serializeString().c_str());
		errorstream << *error_message << std::endl;
		return false;
	} else if (fallback_address.isIPv6()) {
		fallback_address = Address();
	}

	fallback_address.setPort(connect_address.getPort());
	if (fallback_address.isValid()) {
		infostream << "Resolved two addresses for \"" << address_name
			<< "\" isIPv6[0]=" << connect_address.isIPv6()
			<< " isIPv6[1]=" << fallback_address.isIPv6() << std::endl;
	} else {
		infostream << "Resolved one address for \"" << address_name
			<< "\" isIPv6=" << connect_address.isIPv6() << std::endl;
	}


	// Seat 0 uses the engine's main scene manager (the same one that holds
	// the Sky, clouds, and the rest of the rendering pipeline). Passing
	// nullptr for the scene_manager argument tells Client to default to it.
	// Split-screen seats (1..N) get their own independent sub-managers
	// created later in initializeSeats(); each one is fully isolated, so
	// no per-seat visibility tricks are needed.
	try {
		client = new Client(start_data.name.c_str(),
				start_data.password,
				*draw_control, texture_src, shader_src,
				itemdef_manager, nodedef_manager, sound_manager.get(), eventmgr,
				m_rendering_engine,
				m_item_visuals_manager.get(),
				start_data.allow_login_or_register,
				nullptr,
				666);
	} catch (const BaseException &e) {
		*error_message = fmtgettext("Error creating client: %s", e.what());
		errorstream << *error_message << std::endl;
		return false;
	}

	client->migrateModStorage();
	client->m_simple_singleplayer_mode = simple_singleplayer_mode;
	client->m_internal_server = !!server;

	/*
		Wait for server to accept connection
	*/

	client->connect(connect_address, address_name);

	try {
		input->clear();

		FpsControl fps_control;
		f32 dtime;
		f32 wait_time = 0; // in seconds
		bool did_fallback = false;

		fps_control.reset();

		auto framemarker = FrameMarker("Game::connectToServer()-frame").started();

		while (m_rendering_engine->run()) {

			framemarker.end();
			fps_control.limit(device, &dtime);
			framemarker.start();

			// Update client and server
			step(dtime);

			// End condition
			if (client->getState() == LC_Init) {
				*connect_ok = true;
				break;
			}

			// Break conditions
			if (*connection_aborted)
				break;

			if (!checkConnection())
				break;

			if (input->cancelPressed()) {
				*connection_aborted = true;
				infostream << "Connect aborted [Escape]" << std::endl;
				break;
			}

			wait_time += dtime;
			if (server) {
				// never time out
			} else if (wait_time > GAME_FALLBACK_TIMEOUT && !did_fallback) {
				if (!client->hasServerReplied() && fallback_address.isValid()) {
					client->connect(fallback_address, address_name);
				}
				did_fallback = true;
			} else if (wait_time > GAME_CONNECTION_TIMEOUT) {
				*error_message = gettext("Connection timed out.");
				errorstream << *error_message << std::endl;
				break;
			}

			// Update status
			showOverlayMessage(N_("Connecting to server..."), dtime, 20);
		}
		framemarker.end();
	} catch (con::PeerNotFoundException &e) {
		warningstream << "This should not happen. Please report a bug." << std::endl;
		return false;
	}

	return true;
}

bool Game::getServerContent(bool *aborted)
{
	input->clear();

	FpsControl fps_control;
	f32 dtime; // in seconds

	fps_control.reset();

	auto framemarker = FrameMarker("Game::getServerContent()-frame").started();
	while (m_rendering_engine->run()) {
		framemarker.end();
		fps_control.limit(device, &dtime);
		framemarker.start();

		// Update client and server
		step(dtime);

		// End condition
		if (client->mediaReceived() && client->itemdefReceived() &&
				client->nodedefReceived()) {
			return true;
		}

		// Error conditions
		if (!checkConnection())
			return false;

		if (client->getState() < LC_Init) {
			*error_message = gettext("Client disconnected");
			errorstream << *error_message << std::endl;
			return false;
		}

		if (input->cancelPressed()) {
			*aborted = true;
			infostream << "Connect aborted [Escape]" << std::endl;
			return false;
		}

		// Display status
		int progress = 25;

		if (!client->itemdefReceived()) {
			progress = 25;
			m_rendering_engine->draw_load_screen(wstrgettext("Item definitions..."),
					guienv, texture_src, dtime, progress);
		} else if (!client->nodedefReceived()) {
			progress = 30;
			m_rendering_engine->draw_load_screen(wstrgettext("Node definitions..."),
					guienv, texture_src, dtime, progress);
		} else {
			std::ostringstream message;
			std::fixed(message);
			message.precision(0);
			float receive = client->mediaReceiveProgress() * 100;
			message << gettext("Media...");
			if (receive > 0)
				message << " " << receive << "%";
			message.precision(2);

			if ((USE_CURL == 0) ||
					(!g_settings->getBool("enable_remote_media_server"))) {
				float cur = client->getCurRate();
				std::string cur_unit = gettext("KiB/s");

				if (cur > 900) {
					cur /= 1024.0;
					cur_unit = gettext("MiB/s");
				}

				message << " (" << cur << ' ' << cur_unit << ")";
			}

			// 30% -> 65%
			progress = 30 + std::ceil(client->mediaReceiveProgress() * 35 + 0.5f);
			m_rendering_engine->draw_load_screen(utf8_to_wide(message.str()), guienv,
				texture_src, dtime, progress);
		}
	}
	framemarker.end();

	*aborted = true;
	infostream << "Connect aborted [device]" << std::endl;
	return false;
}


/****************************************************************************/
/****************************************************************************
 Run
 ****************************************************************************/
/****************************************************************************/

inline void Game::updateInteractTimers(f32 dtime)
{
	auto tick = [&](GameRunData &rd) {
		if (rd.nodig_delay_timer >= 0)
			rd.nodig_delay_timer -= dtime;

		if (rd.object_hit_delay_timer >= 0)
			rd.object_hit_delay_timer -= dtime;

		rd.time_from_last_punch += dtime;
	};

	tick(runData);

	// Extra seats keep their own GameRunData (swapped into `runData` only
	// during processPlayerInteraction). Those timers must still advance every
	// frame or e.g. nodig_delay_timer never reaches 0 and the player cannot
	// start a second dig after breaking one block.
	for (u8 i = 1; i < m_splitscreen_seats; i++)
		tick(m_seats[i].run_data);
}


/* returns false if game should exit, otherwise true
 */
bool Game::checkConnection()
{
	if (client->accessDenied()) {
		// May be mod-provided, thus may contain color and translation
		const std::string reason = wide_to_utf8(
			unescape_translate(utf8_to_wide(client->accessDeniedReason())));

		*error_message = fmtgettext("Access denied. Reason: %s", reason.c_str());
		*reconnect_requested = client->reconnectRequested();
		errorstream << *error_message << std::endl;
		return false;
	}

	return true;
}

void Game::processQueues()
{
	texture_src->processQueue();
	shader_src->processQueue();
}

void Game::updateDebugState()
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	// debug UI and wireframe
	bool has_debug = client->checkPrivilege("debug");
	bool has_basic_debug = has_debug || (player->hud_flags & HUD_FLAG_BASIC_DEBUG);

	if (m_game_ui->m_flags.show_basic_debug) {
		if (!has_basic_debug)
			m_game_ui->m_flags.show_basic_debug = false;
	} else if (m_game_ui->m_flags.show_minimal_debug) {
		if (has_basic_debug)
			m_game_ui->m_flags.show_basic_debug = true;
	}
	if (!has_basic_debug)
		hud->disableBlockBounds();
	if (!has_debug) {
		draw_control->show_wireframe = false;
		smgr->setGlobalDebugData(0, bbox_debug_flag);
		m_flags.disable_camera_update = false;
		m_game_formspec.disableDebugView();
	}

	// noclip
	draw_control->allow_noclip = m_cache_enable_noclip && client->checkPrivilege("noclip");
}

void Game::updateProfilers(const RunStats &stats, const FpsControl &draw_times,
		f32 dtime)
{
	float profiler_print_interval =
			g_settings->getFloat("profiler_print_interval");
	bool print_to_log = true;

	// Update game UI anyway but don't log
	if (profiler_print_interval <= 0) {
		print_to_log = false;
		profiler_print_interval = 3;
	}

	// Update graphs
	g_profiler->graphAdd("Time non-rendering [us]",
		draw_times.busy_time - stats.drawtime);
	g_profiler->graphAdd("Sleep [us]", draw_times.sleep_time);

	g_profiler->graphSet("FPS", 1.0f / dtime);

	auto stats2 = driver->getFrameStats();
	g_profiler->avg("Irr: drawcalls", stats2.Drawcalls);
	if (stats2.Drawcalls > 0)
		g_profiler->avg("Irr: primitives per drawcall",
			stats2.PrimitivesDrawn / float(stats2.Drawcalls));
	g_profiler->avg("Irr: HW buffers uploaded", stats2.HWBuffersUploaded);
	g_profiler->avg("Irr: HW buffers active", stats2.HWBuffersActive);
	u32 skinned_meshes = stats2.SWSkinnedMeshes + stats2.HWSkinnedMeshes;
	if (skinned_meshes > 0) {
		f32 use_pct = std::floor(100.0f * stats2.HWSkinnedMeshes / skinned_meshes);
		g_profiler->avg("Irr: HW skinning use [%]", use_pct);
	}

	if (profiler_interval.step(dtime, profiler_print_interval)) {
		if (print_to_log) {
			infostream << "Profiler:" << std::endl;
			g_profiler->print(infostream);
		}

		m_game_ui->updateProfiler();
		g_profiler->clear();
	}
}

void Game::updateStats(RunStats *stats, const FpsControl &draw_times,
		f32 dtime)
{

	f32 jitter;
	Jitter *jp;

	/* Time average and jitter calculation
	 */
	jp = &stats->dtime_jitter;
	jp->avg = jp->avg * 0.96 + dtime * 0.04;

	jitter = dtime - jp->avg;

	if (jitter > jp->max)
		jp->max = jitter;

	jp->counter += dtime;

	if (jp->counter > 0.0) {
		jp->counter -= 3.0;
		jp->max_sample = jp->max;
		jp->max_fraction = jp->max_sample / (jp->avg + 0.001);
		jp->max = 0.0;
	}

	/* Busytime average and jitter calculation
	 */
	jp = &stats->busy_time_jitter;
	jp->avg = jp->avg + draw_times.getBusyMs() * 0.02;

	jitter = draw_times.getBusyMs() - jp->avg;

	if (jitter > jp->max)
		jp->max = jitter;
	if (jitter < jp->min)
		jp->min = jitter;

	jp->counter += dtime;

	if (jp->counter > 0.0) {
		jp->counter -= 3.0;
		jp->max_sample = jp->max;
		jp->min_sample = jp->min;
		jp->max = 0.0;
		jp->min = 0.0;
	}
}



bool Game::primaryLocalInputBlockedByMenus() const
{
	if (!isMenuActive())
		return false;
	if (m_splitscreen_seats <= 1)
		return true;

	if (m_game_formspec.isSeatMenuActive(0))
		return true;

	// Fullscreen modals (pause, settings from pause, password, …) are not
	// clipped to a viewport and should still capture the primary pointer/keys.
	GUIModalMenu *top = g_menumgr.tryGetTopMenu();
	if (!top)
		return false;
	const core::rect<s32> &vp = top->getViewport();
	return vp.getWidth() <= 0 || vp.getHeight() <= 0;
}

/****************************************************************************
 Input handling
 ****************************************************************************/

void Game::processUserInput(f32 dtime)
{
	bool desired = shouldShowTouchControls();
	if (desired && !g_touchcontrols) {
		g_touchcontrols = new TouchControls(device, texture_src);

	} else if (!desired && g_touchcontrols) {
		delete g_touchcontrols;
		g_touchcontrols = nullptr;
	}

	// Reset input if window not active or some menu is active
	if (!device->isWindowActive() || primaryLocalInputBlockedByMenus() ||
			guienv->hasFocus(gui_chat_console.get())) {
		if (m_game_focused) {
			m_game_focused = false;
			infostream << "Game lost focus" << std::endl;
			input->releaseAllKeys();
		} else {
			input->clear();
		}

		if (g_touchcontrols)
			g_touchcontrols->hide();

	} else {
		if (g_touchcontrols) {
			/* on touchcontrols step may generate own input events which ain't
			 * what we want in case we just did clear them */
			g_touchcontrols->show();
			g_touchcontrols->step(dtime);
		}

		m_game_focused = true;
	}

	if (!guienv->hasFocus(gui_chat_console.get()) && gui_chat_console->isOpen()
		&& !gui_chat_console->isMyChild(guienv->getFocus()))
	{
		gui_chat_console->closeConsoleAtOnce();
	}

	// Input handler step() (used by the random input generator)
	input->step(dtime);

#ifdef __ANDROID__
	if (!m_game_formspec.handleAndroidUIInput())
		handleAndroidChatInput();
#endif

	// Increase timer for double tap of "keymap_jump"
	if (m_cache_doubletap_jump && runData.jump_timer_up <= 0.2f)
		runData.jump_timer_up += dtime;
	if (m_cache_doubletap_jump && runData.jump_timer_down <= 0.4f)
		runData.jump_timer_down += dtime;

	processKeyInput();
	processItemSelection(&runData.new_playeritem);

	// Split-screen: each extra seat gets its own pass through the
	// player-facing key/hotbar handlers, driven by that seat's own
	// InputHandler (typically a gamepad). Without this, only seat 0
	// could open the inventory, drop items, or change the wielded slot.
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++) {
			if (!m_seats[i].client || !m_seats[i].input)
				continue;
			processKeyInputForSeat(i);
			processItemSelectionForSeat(i);
		}
	}
}


void Game::processKeyInput()
{
	if (wasKeyDown(KeyType::DROP)) {
		dropSelectedItem(isKeyDown(KeyType::SNEAK));
	} else if (wasKeyDown(KeyType::AUTOFORWARD)) {
		toggleAutoforward();
	} else if (wasKeyDown(KeyType::BACKWARD)) {
		if (g_settings->getBool("continuous_forward"))
			toggleAutoforward();
	} else if (wasKeyDown(KeyType::INVENTORY)) {
		// Single-player: pass an empty viewport so the menu uses the
		// full window (legacy behaviour). Split-screen: clamp seat 0's
		// inventory to its own panel, matching how seats 1..N open
		// theirs in processKeyInputForSeat().
		if (m_splitscreen_seats > 1)
			m_game_formspec.showPlayerInventory(nullptr, 0, client,
					&input->joystick, getSeatViewport(0));
		else
			m_game_formspec.showPlayerInventory(nullptr);
	} else if (input->cancelPressed()) {
#ifdef __ANDROID__
		m_android_chat_open = false;
#endif
		if (!gui_chat_console->isOpenInhibited()) {
			m_game_formspec.showPauseMenu();
		}
	} else if (wasKeyDown(KeyType::CHAT)) {
		openConsole(0.2, L"");
	} else if (wasKeyDown(KeyType::CMD)) {
		openConsole(0.2, L"/");
	} else if (wasKeyDown(KeyType::CMD_LOCAL)) {
		if (client->modsLoaded())
			openConsole(0.2, L".");
		else
			m_game_ui->showTranslatedStatusText("Client side scripting is disabled");
	} else if (wasKeyDown(KeyType::CONSOLE)) {
		openConsole(core::clamp(g_settings->getFloat("console_height"), 0.1f, 1.0f));
	} else if (wasKeyDown(KeyType::FREEMOVE)) {
		toggleFreeMove();
	} else if (wasKeyDown(KeyType::JUMP)) {
		toggleFreeMoveAlt();
	} else if (wasKeyDown(KeyType::PITCHMOVE)) {
		togglePitchMove();
	} else if (wasKeyDown(KeyType::FASTMOVE)) {
		toggleFast();
	} else if (wasKeyDown(KeyType::NOCLIP)) {
		toggleNoClip();
#if USE_SOUND
	} else if (wasKeyDown(KeyType::MUTE)) {
		bool new_mute_sound = !g_settings->getBool("mute_sound");
		g_settings->setBool("mute_sound", new_mute_sound);
		if (new_mute_sound)
			m_game_ui->showTranslatedStatusText("Sound muted");
		else
			m_game_ui->showTranslatedStatusText("Sound unmuted");
	} else if (wasKeyDown(KeyType::INC_VOLUME)) {
		float new_volume = g_settings->getFloat("sound_volume", 0.0f, 0.9f) + 0.1f;
		g_settings->setFloat("sound_volume", new_volume);
		std::wstring msg = fwgettext("Volume changed to %d%%", myround(new_volume * 100));
		m_game_ui->showStatusText(msg);
	} else if (wasKeyDown(KeyType::DEC_VOLUME)) {
		float new_volume = g_settings->getFloat("sound_volume", 0.1f, 1.0f) - 0.1f;
		g_settings->setFloat("sound_volume", new_volume);
		std::wstring msg = fwgettext("Volume changed to %d%%", myround(new_volume * 100));
		m_game_ui->showStatusText(msg);
#else
	} else if (wasKeyDown(KeyType::MUTE) || wasKeyDown(KeyType::INC_VOLUME)
			|| wasKeyDown(KeyType::DEC_VOLUME)) {
		m_game_ui->showTranslatedStatusText("Sound system is not supported on this build");
#endif
	} else if (wasKeyDown(KeyType::CINEMATIC)) {
		toggleCinematic();
	} else if (wasKeyPressed(KeyType::SCREENSHOT)) {
		client->makeScreenshot();
	} else if (wasKeyPressed(KeyType::TOGGLE_BLOCK_BOUNDS)) {
		toggleBlockBounds();
	} else if (wasKeyPressed(KeyType::TOGGLE_HUD)) {
		m_game_ui->toggleHud();
	} else if (wasKeyPressed(KeyType::MINIMAP)) {
		toggleMinimap(isKeyDown(KeyType::SNEAK));
	} else if (wasKeyPressed(KeyType::TOGGLE_CHAT)) {
		m_game_ui->toggleChat(client);
	} else if (wasKeyPressed(KeyType::TOGGLE_FOG)) {
		toggleFog();
	} else if (wasKeyDown(KeyType::TOGGLE_UPDATE_CAMERA)) {
		toggleUpdateCamera();
	} else if (wasKeyPressed(KeyType::CAMERA_MODE)) {
		camera->toggleCameraMode();
		updateCameraMode();
	} else if (wasKeyPressed(KeyType::TOGGLE_DEBUG)) {
		toggleDebug();
	} else if (wasKeyPressed(KeyType::TOGGLE_PROFILER)) {
		m_game_ui->toggleProfiler();
	} else if (wasKeyDown(KeyType::INCREASE_VIEWING_RANGE)) {
		increaseViewRange();
	} else if (wasKeyDown(KeyType::DECREASE_VIEWING_RANGE)) {
		decreaseViewRange();
	} else if (wasKeyPressed(KeyType::RANGESELECT)) {
		toggleFullViewRange();
	} else if (wasKeyDown(KeyType::ZOOM)) {
		checkZoomEnabled();
	} else if (wasKeyDown(KeyType::QUICKTUNE_NEXT)) {
		quicktune->next();
	} else if (wasKeyDown(KeyType::QUICKTUNE_PREV)) {
		quicktune->prev();
	} else if (wasKeyDown(KeyType::QUICKTUNE_INC)) {
		quicktune->inc();
	} else if (wasKeyDown(KeyType::QUICKTUNE_DEC)) {
		quicktune->dec();
	}

	if (!isKeyDown(KeyType::JUMP) && runData.reset_jump_timer) {
		runData.reset_jump_timer = false;
		runData.jump_timer_up = 0.0f;
	}

	if (quicktune->hasMessage()) {
		m_game_ui->showStatusText(utf8_to_wide(quicktune->getMessage()));
	}
}

void Game::processItemSelection(u16 *new_playeritem)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	*new_playeritem = player->getWieldIndex();
	u16 max_item = player->getMaxHotbarItemcount();
	if (max_item == 0)
		return;
	max_item -= 1;

	/* Item selection using mouse wheel
	 */
	s32 wheel = input->getMouseWheel();
	if (!m_enable_hotbar_mouse_wheel)
		wheel = 0;
	if (m_invert_hotbar_mouse_wheel)
		wheel *= -1;

	s32 dir = wheel;

	if (wasKeyDown(KeyType::HOTBAR_NEXT))
		dir = -1;

	if (wasKeyDown(KeyType::HOTBAR_PREV))
		dir = 1;

	if (dir < 0)
		*new_playeritem = *new_playeritem < max_item ? *new_playeritem + 1 : 0;
	else if (dir > 0)
		*new_playeritem = *new_playeritem > 0 ? *new_playeritem - 1 : max_item;
	// else dir == 0

	/* Item selection using hotbar slot keys
	 */
	for (u16 i = 0; i <= max_item; i++) {
		if (wasKeyDown((GameKeyType) (KeyType::SLOT_1 + i))) {
			*new_playeritem = i;
			break;
		}
	}

	if (g_touchcontrols) {
		std::optional<u16> selection = g_touchcontrols->getHotbarSelection();
		if (selection)
			*new_playeritem = *selection;
	}

	// Clamp selection again in case it wasn't changed but max_item was
	*new_playeritem = MYMIN(*new_playeritem, max_item);
}


core::rect<s32> Game::getSeatViewport(u8 seat_idx) const
{
	const v2u32 screensize = driver->getScreenSize();
	const s32 W = (s32)screensize.X;
	const s32 H = (s32)screensize.Y;
	const core::rect<s32> fullvp(0, 0, W, H);

	if (m_splitscreen_seats <= 1)
		return fullvp;

	const s32 HW = W / 2;
	const s32 HH = H / 2;
	switch (m_splitscreen_seats) {
	case 2:
		return seat_idx == 0 ? core::rect<s32>(0, 0, W, HH)
				     : core::rect<s32>(0, HH, W, H);
	case 3:
		if (seat_idx == 0) return core::rect<s32>(0, 0, HW, HH);
		if (seat_idx == 1) return core::rect<s32>(HW, 0, W, HH);
		return core::rect<s32>(0, HH, W, H);
	case 4:
	default:
		if (seat_idx == 0) return core::rect<s32>(0, 0, HW, HH);
		if (seat_idx == 1) return core::rect<s32>(HW, 0, W, HH);
		if (seat_idx == 2) return core::rect<s32>(0, HH, HW, H);
		return core::rect<s32>(HW, HH, W, H);
	}
}


void Game::processKeyInputForSeat(u8 seat_idx)
{
	// Note: this runs *in addition* to processKeyInput() (which handles
	// seat 0). It deliberately handles only the subset of keys that make
	// sense per-player; truly global toggles (sound mute / volume,
	// pause menu, screenshot, debug overlays, ...) intentionally stay
	// seat-0-only because they affect the shared engine state and we do
	// not want a second controller to silently mute the whole game.
	auto &seat = m_seats[seat_idx];
	InputHandler *seat_in = seat.input.get();
	Client *seat_client = seat.client;

	// Drop the wielded stack from this seat's inventory. This goes through
	// the seat's own Client so the inventory action is mirrored on the
	// server-side player matching that seat.
	if (seat_in->wasKeyDown(KeyType::DROP)) {
		LocalPlayer *sp = seat_client->getEnv().getLocalPlayer();
		if (sp) {
			IDropAction *a = new IDropAction();
			a->count = seat_in->isKeyDown(KeyType::SNEAK) ? 1 : 0;
			a->from_inv.setCurrentPlayer();
			a->from_list = "main";
			a->from_i = sp->getWieldIndex();
			seat_client->inventoryAction(a);
		}
	}

	// Auto-forward toggle. continuous_forward is a global setting today
	// (see updatePlayerControl), so this still affects every seat - but
	// we still want the gamepad button to *work* for non-primary seats
	// instead of silently doing nothing.
	if (seat_in->wasKeyDown(KeyType::AUTOFORWARD)) {
		toggleAutoforward();
	} else if (seat_in->wasKeyDown(KeyType::BACKWARD)) {
		if (g_settings->getBool("continuous_forward"))
			toggleAutoforward();
	}

	// Inventory: open *this seat's* player inventory. The check is
	// per-seat now: each seat has its own formspec slot (see
	// `GameFormSpec::m_seat_formspec`) so seat 1 can open / close
	// their inventory while seat 2 already has theirs open without
	// either menu stealing the other's focus. The viewport restricts
	// layout to this seat's panel.
	if (seat_in->wasKeyDown(KeyType::INVENTORY) &&
			!m_game_formspec.isSeatMenuActive(seat_idx)) {
		m_game_formspec.showPlayerInventory(nullptr, seat_idx,
				seat_client, &seat_in->joystick,
				getSeatViewport(seat_idx));
	}
}


void Game::processItemSelectionForSeat(u8 seat_idx)
{
	auto &seat = m_seats[seat_idx];
	InputHandler *seat_in = seat.input.get();
	Client *seat_client = seat.client;
	LocalPlayer *player = seat_client->getEnv().getLocalPlayer();
	if (!player)
		return;

	if (!seat.new_playeritem_initialised) {
		seat.new_playeritem = player->getWieldIndex();
		seat.new_playeritem_initialised = true;
	}

	u16 max_item = player->getMaxHotbarItemcount();
	if (max_item == 0)
		return;
	max_item -= 1;

	// Hotbar cycle keys (gamepad shoulder buttons / d-pad). Mouse wheel
	// is intentionally ignored here: it is owned by the keyboard+mouse
	// seat (seat 0).
	s32 dir = 0;
	if (seat_in->wasKeyDown(KeyType::HOTBAR_NEXT))
		dir = -1;
	if (seat_in->wasKeyDown(KeyType::HOTBAR_PREV))
		dir = 1;

	if (dir < 0)
		seat.new_playeritem =
				seat.new_playeritem < max_item ? seat.new_playeritem + 1 : 0;
	else if (dir > 0)
		seat.new_playeritem =
				seat.new_playeritem > 0 ? seat.new_playeritem - 1 : max_item;

	for (u16 i = 0; i <= max_item; i++) {
		if (seat_in->wasKeyDown((GameKeyType) (KeyType::SLOT_1 + i))) {
			seat.new_playeritem = i;
			break;
		}
	}

	seat.new_playeritem = MYMIN(seat.new_playeritem, max_item);

	// Push the selection to the seat's server connection so the
	// authoritative wield index updates and other clients see the
	// correct held item.
	if (player->getWieldIndex() != seat.new_playeritem)
		seat_client->setPlayerItem(seat.new_playeritem);
}


void Game::dropSelectedItem(bool single_item)
{
	IDropAction *a = new IDropAction();
	a->count = single_item ? 1 : 0;
	a->from_inv.setCurrentPlayer();
	a->from_list = "main";
	a->from_i = client->getEnv().getLocalPlayer()->getWieldIndex();
	client->inventoryAction(a);
}

void Game::openConsole(float scale, const wchar_t *line)
{
	assert(scale > 0.0f && scale <= 1.0f);

#ifdef __ANDROID__
	if (!porting::hasPhysicalKeyboardAndroid()) {
		porting::showTextInputDialog("", "", 2);
		m_android_chat_open = true;
	} else {
#endif
	if (gui_chat_console->isOpenInhibited())
		return;
	gui_chat_console->openConsole(scale);
	if (line) {
		gui_chat_console->setCloseOnEnter(true);
		gui_chat_console->replaceAndAddToHistory(line);
	}
#ifdef __ANDROID__
	} // else
#endif
}

#ifdef __ANDROID__
void Game::handleAndroidChatInput()
{
	// It has to be a text input
	if (m_android_chat_open && porting::getLastInputDialogType() == porting::TEXT_INPUT) {
		porting::AndroidDialogState dialogState = porting::getInputDialogState();
		if (dialogState == porting::DIALOG_INPUTTED) {
			std::string text = porting::getInputDialogMessage();
			client->typeChatMessage(utf8_to_wide(text));
		}
		if (dialogState != porting::DIALOG_SHOWN)
			m_android_chat_open = false;
	}
}
#endif

void Game::toggleFreeMove()
{
	bool free_move = !g_settings->getBool("free_move");
	g_settings->set("free_move", bool_to_cstr(free_move));

	if (free_move) {
		if (client->checkPrivilege("fly")) {
			m_game_ui->showTranslatedStatusText("Fly mode enabled");
		} else {
			m_game_ui->showTranslatedStatusText("Fly mode enabled (note: no 'fly' privilege)");
		}
	} else {
		m_game_ui->showTranslatedStatusText("Fly mode disabled");
	}
}

void Game::toggleFreeMoveAlt()
{
	if (!runData.reset_jump_timer) {
		runData.jump_timer_down_before = runData.jump_timer_down;
		runData.jump_timer_down = 0.0f;
	}

	// key down (0.2 s max.), then key up (0.2 s max.), then key down
	if (m_cache_doubletap_jump && runData.jump_timer_up < 0.2f &&
			runData.jump_timer_down_before < 0.4f) // 0.2 + 0.2
		toggleFreeMove();

	runData.reset_jump_timer = true;
}


void Game::togglePitchMove()
{
	bool pitch_move = !g_settings->getBool("pitch_move");
	g_settings->set("pitch_move", bool_to_cstr(pitch_move));

	if (pitch_move) {
		m_game_ui->showTranslatedStatusText("Pitch move mode enabled");
	} else {
		m_game_ui->showTranslatedStatusText("Pitch move mode disabled");
	}
}


void Game::toggleFast()
{
	bool fast_move = !g_settings->getBool("fast_move");
	bool has_fast_privs = client->checkPrivilege("fast");
	g_settings->set("fast_move", bool_to_cstr(fast_move));

	if (fast_move) {
		if (has_fast_privs) {
			m_game_ui->showTranslatedStatusText("Fast mode enabled");
		} else {
			m_game_ui->showTranslatedStatusText("Fast mode enabled (note: no 'fast' privilege)");
		}
	} else {
		m_game_ui->showTranslatedStatusText("Fast mode disabled");
	}

	m_touch_simulate_aux1 = fast_move && has_fast_privs;
}


void Game::toggleNoClip()
{
	bool noclip = !g_settings->getBool("noclip");
	g_settings->set("noclip", bool_to_cstr(noclip));

	if (noclip) {
		if (client->checkPrivilege("noclip")) {
			m_game_ui->showTranslatedStatusText("Noclip mode enabled");
		} else {
			m_game_ui->showTranslatedStatusText("Noclip mode enabled (note: no 'noclip' privilege)");
		}
	} else {
		m_game_ui->showTranslatedStatusText("Noclip mode disabled");
	}
}

void Game::toggleCinematic()
{
	bool cinematic = !g_settings->getBool("cinematic");
	g_settings->set("cinematic", bool_to_cstr(cinematic));

	if (cinematic)
		m_game_ui->showTranslatedStatusText("Cinematic mode enabled");
	else
		m_game_ui->showTranslatedStatusText("Cinematic mode disabled");
}

void Game::toggleBlockBounds()
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	if (!(client->checkPrivilege("debug") || (player->hud_flags & HUD_FLAG_BASIC_DEBUG))) {
		m_game_ui->showTranslatedStatusText("Can't show block bounds (disabled by game or mod)");
		return;
	}
	enum Hud::BlockBoundsMode newmode = hud->toggleBlockBounds();
	switch (newmode) {
		case Hud::BLOCK_BOUNDS_OFF:
			m_game_ui->showTranslatedStatusText("Block bounds hidden");
			break;
		case Hud::BLOCK_BOUNDS_CURRENT:
			m_game_ui->showTranslatedStatusText("Block bounds shown for current block");
			break;
		case Hud::BLOCK_BOUNDS_NEAR:
			m_game_ui->showTranslatedStatusText("Block bounds shown for nearby blocks");
			break;
		default:
			break;
	}
}

// Autoforward by toggling continuous forward.
void Game::toggleAutoforward()
{
	bool autorun_enabled = !g_settings->getBool("continuous_forward");
	g_settings->set("continuous_forward", bool_to_cstr(autorun_enabled));

	if (autorun_enabled)
		m_game_ui->showTranslatedStatusText("Automatic forward enabled");
	else
		m_game_ui->showTranslatedStatusText("Automatic forward disabled");
}

void Game::toggleMinimap(bool shift_pressed)
{
	if (!mapper || !m_game_ui->m_flags.show_hud || !g_settings->getBool("enable_minimap"))
		return;

	if (shift_pressed)
		mapper->toggleMinimapShape();
	else
		mapper->nextMode();

	// TODO: When legacy minimap is deprecated, keep only HUD minimap stuff here

	// Not so satisying code to keep compatibility with old fixed mode system
	// -->
	u32 hud_flags = client->getEnv().getLocalPlayer()->hud_flags;

	// If radar is disabled, try to find a non radar mode or fall back to 0
	if (!(hud_flags & HUD_FLAG_MINIMAP_RADAR_VISIBLE))
		while (mapper->getModeIndex() &&
				mapper->getModeDef().type == MINIMAP_TYPE_RADAR)
			mapper->nextMode();
	// <--
	// End of 'not so satifying code'
	if (hud && hud->hasElementOfType(HUD_ELEM_MINIMAP))
		m_game_ui->showStatusText(utf8_to_wide(mapper->getModeDef().label));
	else
		m_game_ui->showTranslatedStatusText("Minimap currently disabled by game or mod");
}

void Game::toggleFog()
{
	bool flag = !g_settings->getBool("enable_fog");
	g_settings->setBool("enable_fog", flag);
	bool allowed = sky->getFogDistance() < 0 || client->checkPrivilege("debug");
	if (!allowed)
		m_game_ui->showTranslatedStatusText("Fog enabled by game or mod");
	else if (flag)
		m_game_ui->showTranslatedStatusText("Fog enabled");
	else
		m_game_ui->showTranslatedStatusText("Fog disabled");
}


void Game::toggleDebug()
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	bool has_debug = client->checkPrivilege("debug");
	bool has_basic_debug = has_debug || (player->hud_flags & HUD_FLAG_BASIC_DEBUG);

	// Initial: No debug info
	// 1x toggle: Debug text
	// 2x toggle: Debug text with profiler graph
	// 3x toggle: Debug text and wireframe (needs "debug" priv)
	// 4x toggle: Debug text and bbox (needs "debug" priv)
	//
	// The debug text can be in 2 modes: minimal and basic.
	// * Minimal: Only technical client info that not gameplay-relevant
	// * Basic: Info that might give gameplay advantage, e.g. pos, angle
	// Basic mode is used when player has the debug HUD flag set,
	// otherwise the Minimal mode is used.

	auto &state = m_flags.debug_state;
	state = (state + 1) % 5;
	if (state >= 3 && !has_debug)
		state = 0;

	m_game_ui->m_flags.show_minimal_debug = state > 0;
	m_game_ui->m_flags.show_basic_debug = state > 0 && has_basic_debug;
	m_game_ui->m_flags.show_profiler_graph = state == 2;
	draw_control->show_wireframe = state == 3;
	smgr->setGlobalDebugData(state == 4 ? bbox_debug_flag : 0,
			state == 4 ? 0 : bbox_debug_flag);

	if (state == 1) {
		m_game_ui->showTranslatedStatusText("Debug info shown");
	} else if (state == 2) {
		m_game_ui->showTranslatedStatusText("Profiler graph shown");
	} else if (state == 3) {
		if (driver->getDriverType() == video::EDT_OGLES2)
			m_game_ui->showTranslatedStatusText("Wireframe not supported by video driver");
		else
			m_game_ui->showTranslatedStatusText("Wireframe shown");
	} else if (state == 4) {
		m_game_ui->showTranslatedStatusText("Bounding boxes shown");
	} else {
		m_game_ui->showTranslatedStatusText("All debug info hidden");
	}
}


void Game::toggleUpdateCamera()
{
	auto &flag = m_flags.disable_camera_update;
	flag = client->checkPrivilege("debug") ? !flag : false;
	if (flag)
		m_game_ui->showTranslatedStatusText("Camera update disabled");
	else
		m_game_ui->showTranslatedStatusText("Camera update enabled");
}


void Game::increaseViewRange()
{
	s16 range = g_settings->getS16("viewing_range");
	s16 range_new = range + 10;
	s16 server_limit = sky->getFogDistance();

	if (range_new >= 4000) {
		range_new = 4000;
		std::wstring msg = server_limit >= 0 && range_new > server_limit ?
				fwgettext("Viewing range changed to %d (the maximum), but limited to %d by game or mod", range_new, server_limit) :
				fwgettext("Viewing range changed to %d (the maximum)", range_new);
		m_game_ui->showStatusText(msg);
	} else {
		std::wstring msg = server_limit >= 0 && range_new > server_limit ?
				fwgettext("Viewing range changed to %d, but limited to %d by game or mod", range_new, server_limit) :
				fwgettext("Viewing range changed to %d", range_new);
		m_game_ui->showStatusText(msg);
	}
	g_settings->set("viewing_range", itos(range_new));
}


void Game::decreaseViewRange()
{
	s16 range = g_settings->getS16("viewing_range");
	s16 range_new = range - 10;
	s16 server_limit = sky->getFogDistance();

	if (range_new <= 20) {
		range_new = 20;
		std::wstring msg = server_limit >= 0 && range_new > server_limit ?
				fwgettext("Viewing changed to %d (the minimum), but limited to %d by game or mod", range_new, server_limit) :
				fwgettext("Viewing changed to %d (the minimum)", range_new);
		m_game_ui->showStatusText(msg);
	} else {
		std::wstring msg = server_limit >= 0 && range_new > server_limit ?
				fwgettext("Viewing range changed to %d, but limited to %d by game or mod", range_new, server_limit) :
				fwgettext("Viewing range changed to %d", range_new);
		m_game_ui->showStatusText(msg);
	}
	g_settings->set("viewing_range", itos(range_new));
}


void Game::toggleFullViewRange()
{
	draw_control->range_all = !draw_control->range_all;
	if (draw_control->range_all) {
		if (sky->getFogDistance() >= 0) {
			m_game_ui->showTranslatedStatusText("Unlimited viewing range enabled, but forbidden by game or mod");
		} else {
			m_game_ui->showTranslatedStatusText("Unlimited viewing range enabled");
		}
	} else {
		m_game_ui->showTranslatedStatusText("Unlimited viewing range disabled");
	}
}


void Game::checkZoomEnabled()
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	if (player->getZoomFOV() < 0.001f || player->getFov().fov > 0.0f)
		m_game_ui->showTranslatedStatusText("Zoom currently disabled by game or mod");
}

void Game::updateCameraDirection(CameraOrientation *cam, float dtime)
{
	auto *cur_control = device->getCursorControl();

	/* On Linux and Windows, enabling relative mouse mode somehow results
	in simulated mouse events being generated from touch events, even though
	SDL_HINT_MOUSE_TOUCH_EVENTS and SDL_HINT_TOUCH_MOUSE_EVENTS are set to 0.
	Since we have our own code to synthesize mouse events from touch events,
	this results in duplicated input. To avoid that, we don't enable relative
	mouse mode if we're in touchscreen mode. */
	if (cur_control)
		cur_control->setRelativeMode(!g_touchcontrols && !primaryLocalInputBlockedByMenus());

	if ((device->isWindowActive() && device->isWindowFocused()
			&& !primaryLocalInputBlockedByMenus()) || input->isRandom()) {

		if (cur_control && !input->isRandom()) {
			// Mac OSX gets upset if this is set every frame
			if (cur_control->isVisible())
				cur_control->setVisible(false);
		}

		if (m_first_loop_after_window_activation && !g_touchcontrols) {
			m_first_loop_after_window_activation = false;

			input->setMousePos(driver->getScreenSize().Width / 2,
				driver->getScreenSize().Height / 2);
		} else {
			updateCameraOrientation(cam, dtime);
		}

	} else {
		// Mac OSX gets upset if this is set every frame
		if (cur_control && !cur_control->isVisible())
			cur_control->setVisible(true);

		m_first_loop_after_window_activation = true;
	}
	if (g_touchcontrols)
		m_first_loop_after_window_activation = true;
}

// Get the factor to multiply with sensitivity to get the same mouse/joystick
// responsiveness independently of FOV.
f32 Game::getSensitivityScaleFactor() const
{
	f32 fov_y = client->getCamera()->getFovY();

	// Multiply by a constant such that it becomes 1.0 at 72 degree FOV and
	// 16:9 aspect ratio to minimize disruption of existing sensitivity
	// settings.
	return std::tan(fov_y / 2.0f) * 1.3763819f;
}

bool Game::isTouchShootlineUsed() const
{
	return g_touchcontrols && g_touchcontrols->isShootlineAvailable() &&
			camera->getCameraMode() == CAMERA_MODE_FIRST;
}

void Game::updateCameraOrientation(CameraOrientation *cam, float dtime)
{
	f32 sens_scale = getSensitivityScaleFactor();

	if (g_touchcontrols) {
		// User setting is already applied by TouchControls.
		cam->camera_yaw   += g_touchcontrols->getYawChange()   * sens_scale;
		cam->camera_pitch += g_touchcontrols->getPitchChange() * sens_scale;
	} else {
		v2s32 center(driver->getScreenSize().Width / 2, driver->getScreenSize().Height / 2);
		v2s32 dist = input->getMousePos() - center;

		if (m_invert_mouse || camera->getCameraMode() == CAMERA_MODE_THIRD_FRONT) {
			dist.Y = -dist.Y;
		}

		cam->camera_yaw   -= dist.X * m_cache_mouse_sensitivity * sens_scale;
		cam->camera_pitch += dist.Y * m_cache_mouse_sensitivity * sens_scale;

		if (dist.X != 0 || dist.Y != 0)
			input->setMousePos(center.X, center.Y);
	}

	if (m_cache_enable_joysticks) {
		f32 c = m_cache_joystick_frustum_sensitivity * dtime * sens_scale;
		cam->camera_yaw -= input->joystick.getAxisWithoutDead(JA_FRUSTUM_HORIZONTAL) * c;
		cam->camera_pitch += input->joystick.getAxisWithoutDead(JA_FRUSTUM_VERTICAL) * c;
	}

	// Keyboard look
	const f32 rate = m_cache_keyboard_camera_speed * dtime * sens_scale;

	if (input->isKeyDown(KeyType::CAMERA_YAW_LEFT))
		cam->camera_yaw += rate;
	if (input->isKeyDown(KeyType::CAMERA_YAW_RIGHT))
		cam->camera_yaw -= rate;
	if (input->isKeyDown(KeyType::CAMERA_PITCH_UP))
		cam->camera_pitch -= rate;
	if (input->isKeyDown(KeyType::CAMERA_PITCH_DOWN))
		cam->camera_pitch += rate;

	cam->camera_pitch = rangelim(cam->camera_pitch, -90, 90);
}


// Get the state of an optionally togglable key
bool Game::getTogglableKeyState(GameKeyType key, bool toggling_enabled, bool prev_key_state)
{
	if (!toggling_enabled)
		return isKeyDown(key);
	else
		return prev_key_state ^ wasKeyPressed(key);
}


void Game::applyIdlePlayerControlForOpenMenu(const CameraOrientation &cam)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	// In free move (fly), the "toggle_sneak_key" setting would prevent precise
	// up/down movements. Hence, enable the feature only during 'normal' movement.
	const bool allow_sneak_toggle = m_cache_toggle_sneak_key &&
		!(player->getPlayerSettings().free_move && client->checkPrivilege("fly"));

	// Keyboard/mouse do not drive movement here. Joystick analog movement is
	// encoded separately from key bits (see PlayerControl / setMovementFromKeys);
	// passing zeros for speed/direction keeps the character still even though
	// the stick is deflected — needed while a formspec is open because
	// `InputHandler::clear()` does not zero cached axis values.
	PlayerControl control(
			false,
			false,
			false,
			false,
			false,
			getTogglableKeyState(KeyType::AUX1, m_cache_toggle_aux1_key,
					player->control.aux1),
			getTogglableKeyState(KeyType::SNEAK, allow_sneak_toggle,
					player->control.sneak),
			false,
			false,
			false,
			cam.camera_pitch,
			cam.camera_yaw,
			0.0f,
			0.0f);
	control.setMovementFromKeys();
	client->setPlayerControl(control);
}


void Game::updatePlayerControl(const CameraOrientation &cam)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	// In free move (fly), the "toggle_sneak_key" setting would prevent precise
	// up/down movements. Hence, enable the feature only during 'normal' movement.
	const bool allow_sneak_toggle = m_cache_toggle_sneak_key &&
		!(player->getPlayerSettings().free_move && client->checkPrivilege("fly"));

	//TimeTaker tt("update player control", NULL, PRECISION_NANO);

	const bool ctl_forward = isKeyDown(KeyType::FORWARD);
	const bool ctl_aux1 = getTogglableKeyState(KeyType::AUX1,
			m_cache_toggle_aux1_key, player->control.aux1);
	PlayerControl control(
		ctl_forward,
		isKeyDown(KeyType::BACKWARD),
		isKeyDown(KeyType::LEFT),
		isKeyDown(KeyType::RIGHT),
		isKeyDown(KeyType::JUMP) || player->getAutojump(),
		ctl_aux1,
		getTogglableKeyState(KeyType::SNEAK, allow_sneak_toggle,      player->control.sneak),
		isKeyDown(KeyType::ZOOM),
		isKeyDown(KeyType::DIG),
		isKeyDown(KeyType::PLACE),
		cam.camera_pitch,
		cam.camera_yaw,
		input->getJoystickSpeed(),
		input->getJoystickDirection()
	);
	control.setMovementFromKeys();

	// autoforward if set: move at maximum speed
	if (player->getPlayerSettings().continuous_forward &&
			client->activeObjectsReceived() && !player->isDead()) {
		control.movement_speed = 1.0f;
		// sideways movement only
		float dx = std::sin(control.movement_direction);
		control.movement_direction = std::atan2(dx, 1.0f);
	}

	/* For touch, simulate holding down AUX1 (fast move) if the user has
	 * the fast_move setting toggled on. If there is an aux1 key defined for
	 * touch then its meaning is inverted (i.e. holding aux1 means walk and
	 * not fast)
	 */
	if (g_touchcontrols && m_touch_simulate_aux1) {
		control.aux1 = control.aux1 ^ true;
	}

	client->setPlayerControl(control);

	//tt.stop();
}

void Game::updatePauseState()
{
	bool was_paused = this->m_is_paused;
	this->m_is_paused = this->simple_singleplayer_mode && g_menumgr.pausesGame();

	if (!was_paused && this->m_is_paused) {
		this->pauseAnimation();
		this->sound_manager->pauseAll();
	} else if (was_paused && !this->m_is_paused) {
		this->resumeAnimation();
		this->sound_manager->resumeAll();
	}
}


inline void Game::step(f32 dtime)
{
	ZoneScoped;

	if (server) {
		float fps_max = !device->isWindowFocused() && simple_singleplayer_mode ?
				g_settings->getFloat("fps_max_unfocused") :
				g_settings->getFloat("fps_max");
		fps_max = std::max(fps_max, 1.0f);
		/*
		 * Unless you have a barebones game, running the server at more than 60Hz
		 * is hardly realistic and you're at the point of diminishing returns.
		 * fps_max is also not necessarily anywhere near the FPS actually achieved
		 * (also due to vsync).
		 */
		fps_max = std::min(fps_max, 60.0f);

		server->setStepSettings(Server::StepSettings{
				1.0f / fps_max,
				m_is_paused
			});

		server->step();
	}

	if (!m_is_paused) {
		if (m_splitscreen_seats <= 1) {
			client->step(dtime);
		} else {
			for (u8 i = 0; i < m_splitscreen_seats; i++) {
				if (m_seats[i].client)
					m_seats[i].client->step(dtime);
			}
		}
	}
}

static void pauseNodeAnimation(PausedNodesList &paused, scene::ISceneNode *node) {
	if (!node)
		return;
	for (auto &&child: node->getChildren())
		pauseNodeAnimation(paused, child);
	if (node->getType() != scene::ESNT_ANIMATED_MESH)
		return;
	auto animated_node = static_cast<scene::AnimatedMeshSceneNode *>(node);
	float speed = animated_node->getAnimationSpeed();
	if (!speed)
		return;
	paused.emplace_back(grab(animated_node), speed);
	animated_node->setAnimationSpeed(0.0f);
}

void Game::pauseAnimation()
{
	pauseNodeAnimation(paused_animated_nodes, smgr->getRootSceneNode());
}

void Game::resumeAnimation()
{
	for (auto &&pair: paused_animated_nodes)
		pair.first->setAnimationSpeed(pair.second);
	paused_animated_nodes.clear();
}

const ClientEventHandler Game::clientEventHandler[CLIENTEVENT_MAX] = {
	{&Game::handleClientEvent_None},
	{&Game::handleClientEvent_PlayerDamage},
	{&Game::handleClientEvent_PlayerForceMove},
	{&Game::handleClientEvent_DeathscreenLegacy},
	{&Game::handleClientEvent_ShowFormSpec},
	{&Game::handleClientEvent_ShowCSMFormSpec},
	{&Game::handleClientEvent_ShowPauseMenuFormSpec},
	{&Game::handleClientEvent_HandleParticleEvent},
	{&Game::handleClientEvent_HandleParticleEvent},
	{&Game::handleClientEvent_HandleParticleEvent},
	{&Game::handleClientEvent_HudAdd},
	{&Game::handleClientEvent_HudRemove},
	{&Game::handleClientEvent_HudChange},
	{&Game::handleClientEvent_SetSky},
	{&Game::handleClientEvent_SetSun},
	{&Game::handleClientEvent_SetMoon},
	{&Game::handleClientEvent_SetStars},
	{&Game::handleClientEvent_OverrideDayNightRatio},
	{&Game::handleClientEvent_CloudParams},
	{&Game::handleClientEvent_UpdateCamera},
};

void Game::handleClientEvent_None(ClientEvent *event, CameraOrientation *cam)
{
	FATAL_ERROR("ClientEvent type None received");
}

void Game::handleClientEvent_PlayerDamage(ClientEvent *event, CameraOrientation *cam)
{
	if (client->modsLoaded())
		client->getScript()->on_damage_taken(event->player_damage.amount);

	if (!event->player_damage.effect)
		return;

	// Damage flash and hurt tilt are not used at death
	if (client->getHP() > 0) {
		LocalPlayer *player = client->getEnv().getLocalPlayer();

		f32 hp_max = player->getCAO() ?
			player->getCAO()->getProperties().hp_max : PLAYER_MAX_HP_DEFAULT;
		f32 damage_ratio = event->player_damage.amount / hp_max;

		if (g_settings->getBool("hurt_flash_enabled")) {
			runData.damage_flash += 95.0f + 64.f * damage_ratio;
			runData.damage_flash = MYMIN(runData.damage_flash, 127.0f);
		}

		player->hurt_tilt_timer = 1.5f;
		player->hurt_tilt_strength =
			rangelim(damage_ratio * 5.0f, 1.0f, 4.0f);

		// Gamepad rumble proportional to damage (Minecraft Bedrock style).
		// Light damage ⇒ short, light buzz; heavy damage ⇒ longer, heavy
		// rumble. Skipped when the user has joysticks or rumble disabled.
		// Split-screen: `processClientEvents` swaps `input` with each seat's
		// handler while draining that seat's queue (same as `client`).
		if (m_cache_enable_joysticks
				&& g_settings->getBool("joystick_rumble_enable")) {
			const float user_scale = rangelim(
					g_settings->getFloat("joystick_rumble_strength"),
					0.0f, 1.0f);
			if (user_scale > 0.0f) {
				const float ratio = rangelim(damage_ratio, 0.05f, 1.0f);
				// Heavy (low-frequency) motor scales hardest with damage;
				// the high-frequency motor adds a baseline buzz so even
				// 1-HP nicks feel like something.
				const u16 low  = (u16)(65535.0f *
						rangelim(0.45f + 0.55f * ratio, 0.0f, 1.0f) *
						user_scale);
				const u16 high = (u16)(65535.0f *
						rangelim(0.30f + 0.40f * ratio, 0.0f, 1.0f) *
						user_scale);
				const u32 duration_ms = (u32)(120.0f + 250.0f * ratio);
				device->rumbleJoystick(input->joystick.getJoystickId(),
						low, high, duration_ms);
			}
		}
	}

	// Play damage sound
	client->getEventManager()->put(new SimpleTriggerEvent(MtEvent::PLAYER_DAMAGE));
}

void Game::handleClientEvent_PlayerForceMove(ClientEvent *event, CameraOrientation *cam)
{
	cam->camera_yaw = event->player_force_move.yaw;
	cam->camera_pitch = event->player_force_move.pitch;
}

void Game::handleClientEvent_DeathscreenLegacy(ClientEvent *event, CameraOrientation *cam)
{
	const u8 seat_idx = m_current_event_seat;
	const bool is_split = m_splitscreen_seats > 1;
	auto &seat = m_seats[seat_idx];
	Client *seat_client = seat.client ? seat.client : client;
	JoystickController *seat_joystick = nullptr;
	if (seat_idx == 0) {
		seat_joystick = &input->joystick;
	} else if (seat.input) {
		seat_joystick = &seat.input->joystick;
	}
	const core::rect<s32> seat_viewport =
		is_split ? getSeatViewport(seat_idx) : core::rect<s32>(0, 0, 0, 0);

	m_game_formspec.showDeathFormspecLegacy(
			seat_idx, seat_client, seat_joystick, seat_viewport);
}

void Game::handleClientEvent_ShowFormSpec(ClientEvent *event, CameraOrientation *cam)
{
	auto &fs = event->show_formspec;

	// Server-pushed formspecs (chests, furnaces, custom UIs, ...) are
	// queued on the seat's own Client. Route to that seat's slot so
	// chest-on-seat-2 doesn't replace inventory-on-seat-0.
	const u8 seat_idx = m_current_event_seat;
	const bool is_split = m_splitscreen_seats > 1;
	auto &seat = m_seats[seat_idx];
	Client *seat_client = seat.client ? seat.client : client;
	JoystickController *seat_joystick = nullptr;
	if (seat_idx == 0) {
		seat_joystick = &input->joystick;
	} else if (seat.input) {
		seat_joystick = &seat.input->joystick;
	}
	const core::rect<s32> seat_viewport =
		is_split ? getSeatViewport(seat_idx) : core::rect<s32>(0, 0, 0, 0);

	if (fs.formname->empty() && !fs.formspec->empty()) {
		m_game_formspec.showPlayerInventory(fs.formspec, seat_idx,
				seat_client, seat_joystick, seat_viewport);
	} else if (seat_idx > 0 || is_split) {
		m_game_formspec.showFormSpecForSeat(seat_idx, seat_client,
				seat_joystick, seat_viewport,
				*fs.formspec, *fs.formname);
	} else {
		m_game_formspec.showFormSpec(*fs.formspec, *fs.formname);
	}

	delete fs.formspec;
	delete fs.formname;
}

void Game::handleClientEvent_ShowCSMFormSpec(ClientEvent *event, CameraOrientation *cam)
{
	auto &fs = event->show_formspec;

	const u8 seat_idx = m_current_event_seat;
	const bool is_split = m_splitscreen_seats > 1;
	auto &seat = m_seats[seat_idx];
	Client *seat_client = seat.client ? seat.client : client;
	JoystickController *seat_joystick = nullptr;
	if (seat_idx == 0) {
		seat_joystick = &input->joystick;
	} else if (seat.input) {
		seat_joystick = &seat.input->joystick;
	}
	const core::rect<s32> seat_viewport =
		is_split ? getSeatViewport(seat_idx) : core::rect<s32>(0, 0, 0, 0);

	if (seat_idx > 0 || is_split) {
		m_game_formspec.showCSMFormSpecForSeat(seat_idx, seat_client,
				seat_joystick, seat_viewport,
				*fs.formspec, *fs.formname);
	} else {
		m_game_formspec.showCSMFormSpec(*fs.formspec, *fs.formname);
	}

	delete fs.formspec;
	delete fs.formname;
}

void Game::handleClientEvent_ShowPauseMenuFormSpec(ClientEvent *event, CameraOrientation *cam)
{
	m_game_formspec.showPauseMenuFormSpec(*event->show_formspec.formspec,
		*event->show_formspec.formname);

	delete event->show_formspec.formspec;
	delete event->show_formspec.formname;
}

void Game::handleClientEvent_HandleParticleEvent(ClientEvent *event,
		CameraOrientation *cam)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	client->getParticleManager()->handleParticleEvent(event, client, player);
}

void Game::handleClientEvent_HudAdd(ClientEvent *event, CameraOrientation *cam)
{
	// Route to the seat whose ClientEvent queue we're currently draining
	// (set by processClientEvents). Without this, every seat's HUD events
	// would be applied to seat 0's player, leaving any extra split-screen
	// seats without a HUD at all.
	auto &seat = m_seats[m_current_event_seat];
	LocalPlayer *player = seat.client->getEnv().getLocalPlayer();

	u32 server_id = event->hudadd->server_id;
	// ignore if we already have a HUD with that ID
	auto i = seat.hud_server_to_client.find(server_id);
	if (i != seat.hud_server_to_client.end()) {
		delete event->hudadd;
		return;
	}

	HudElement *e = new HudElement;
	e->type   = static_cast<HudElementType>(event->hudadd->type);
	e->pos    = event->hudadd->pos;
	e->name   = event->hudadd->name;
	e->scale  = event->hudadd->scale;
	e->text   = event->hudadd->text;
	e->number = event->hudadd->number;
	e->item   = event->hudadd->item;
	e->dir    = event->hudadd->dir;
	e->align  = event->hudadd->align;
	e->offset = event->hudadd->offset;
	e->world_pos = event->hudadd->world_pos;
	e->size      = v2f::from(event->hudadd->size);
	e->z_index   = event->hudadd->z_index;
	e->text2     = event->hudadd->text2;
	e->style     = event->hudadd->style;
	seat.hud_server_to_client[server_id] = player->addHud(e);

	delete event->hudadd;
}

void Game::handleClientEvent_HudRemove(ClientEvent *event, CameraOrientation *cam)
{
	auto &seat = m_seats[m_current_event_seat];
	LocalPlayer *player = seat.client->getEnv().getLocalPlayer();

	auto i = seat.hud_server_to_client.find(event->hudrm.id);
	if (i != seat.hud_server_to_client.end()) {
		HudElement *e = player->removeHud(i->second);
		delete e;
		seat.hud_server_to_client.erase(i);
	}

}

void Game::handleClientEvent_HudChange(ClientEvent *event, CameraOrientation *cam)
{
	auto &seat = m_seats[m_current_event_seat];
	LocalPlayer *player = seat.client->getEnv().getLocalPlayer();

	HudElement *e = nullptr;

	auto i = seat.hud_server_to_client.find(event->hudchange->id);
	if (i != seat.hud_server_to_client.end()) {
		e = player->getHud(i->second);
	}

	if (e == nullptr) {
		delete event->hudchange;
		return;
	}

#define CASE_SET(statval, prop, dataprop) \
	case statval: \
		e->prop = event->hudchange->dataprop; \
		break

	switch (event->hudchange->stat) {
		CASE_SET(HUD_STAT_POS, pos, v2fdata);

		CASE_SET(HUD_STAT_NAME, name, sdata);

		CASE_SET(HUD_STAT_SCALE, scale, v2fdata);

		CASE_SET(HUD_STAT_TEXT, text, sdata);

		CASE_SET(HUD_STAT_NUMBER, number, data);

		CASE_SET(HUD_STAT_ITEM, item, data);

		CASE_SET(HUD_STAT_DIR, dir, data);

		CASE_SET(HUD_STAT_ALIGN, align, v2fdata);

		CASE_SET(HUD_STAT_OFFSET, offset, v2fdata);

		CASE_SET(HUD_STAT_WORLD_POS, world_pos, v3fdata);

		CASE_SET(HUD_STAT_SIZE, size, v2fdata);

		CASE_SET(HUD_STAT_Z_INDEX, z_index, data);

		CASE_SET(HUD_STAT_TEXT2, text2, sdata);

		CASE_SET(HUD_STAT_STYLE, style, data);

		case HudElementStat_END:
			break;
	}

#undef CASE_SET

	delete event->hudchange;
}

void Game::handleClientEvent_SetSky(ClientEvent *event, CameraOrientation *cam)
{
	sky->setVisible(false);
	// Whether clouds are visible in front of a custom skybox.
	sky->setCloudsEnabled(event->set_sky->clouds);

	// Clear the old textures out in case we switch rendering type.
	sky->clearSkyboxTextures();
	// Handle according to type
	if (event->set_sky->type == "regular") {
		// Shows the mesh skybox
		sky->setVisible(true);
		// Update mesh based skybox colours if applicable.
		sky->setSkyColors(event->set_sky->sky_color);
		sky->setHorizonTint(
			event->set_sky->fog_sun_tint,
			event->set_sky->fog_moon_tint,
			event->set_sky->fog_tint_type
		);
	} else if (event->set_sky->type == "skybox" &&
			event->set_sky->textures.size() == 6) {
		// Disable the dynamic mesh skybox:
		sky->setVisible(false);
		// Set fog colors:
		sky->setFallbackBgColor(event->set_sky->bgcolor);
		// Set sunrise and sunset fog tinting:
		sky->setHorizonTint(
			event->set_sky->fog_sun_tint,
			event->set_sky->fog_moon_tint,
			event->set_sky->fog_tint_type
		);
		// Add textures to skybox.
		for (int i = 0; i < 6; i++)
			sky->addTextureToSkybox(event->set_sky->textures[i], i, texture_src);
	} else {
		// Handle everything else as plain color.
		if (event->set_sky->type != "plain")
			infostream << "Unknown sky type: "
				<< (event->set_sky->type) << std::endl;
		sky->setVisible(false);
		sky->setFallbackBgColor(event->set_sky->bgcolor);
		// Disable directional sun/moon tinting on plain or invalid skyboxes.
		sky->setHorizonTint(
			event->set_sky->bgcolor,
			event->set_sky->bgcolor,
			"custom"
		);
	}

	// Orbit Tilt:
	sky->setBodyOrbitTilt(event->set_sky->body_orbit_tilt);

	// fog
	// do not override a potentially smaller client setting.
	sky->setFogDistance(event->set_sky->fog_distance);

	// if the fog distance is reset, switch back to the client's viewing_range
	if (event->set_sky->fog_distance < 0)
		draw_control->wanted_range = g_settings->getS16("viewing_range");

	if (event->set_sky->fog_start >= 0)
		sky->setFogStart(rangelim(event->set_sky->fog_start, 0.0f, 0.99f));
	else
		sky->setFogStart(rangelim(g_settings->getFloat("fog_start"), 0.0f, 0.99f));

	sky->setFogColor(event->set_sky->fog_color);

	sky->setAutoCaveBrightness(event->set_sky->auto_dim_skybox);

	delete event->set_sky;
}

void Game::handleClientEvent_SetSun(ClientEvent *event, CameraOrientation *cam)
{
	sky->setSunVisible(event->sun_params->visible);
	sky->setSunTexture(event->sun_params->texture,
		event->sun_params->tonemap, texture_src);
	sky->setSunScale(event->sun_params->scale);
	sky->setSunriseVisible(event->sun_params->sunrise_visible);
	sky->setSunriseTexture(event->sun_params->sunrise, texture_src);
	delete event->sun_params;
}

void Game::handleClientEvent_SetMoon(ClientEvent *event, CameraOrientation *cam)
{
	sky->setMoonVisible(event->moon_params->visible);
	sky->setMoonTexture(event->moon_params->texture,
		event->moon_params->tonemap, texture_src);
	sky->setMoonScale(event->moon_params->scale);
	delete event->moon_params;
}

void Game::handleClientEvent_SetStars(ClientEvent *event, CameraOrientation *cam)
{
	sky->setStarsVisible(event->star_params->visible);
	sky->setStarCount(event->star_params->count);
	sky->setStarColor(event->star_params->starcolor);
	sky->setStarScale(event->star_params->scale);
	sky->setStarDayOpacity(event->star_params->day_opacity);
	sky->setStarSeed(event->star_params->star_seed);
	delete event->star_params;
}

void Game::handleClientEvent_OverrideDayNightRatio(ClientEvent *event,
		CameraOrientation *cam)
{
	client->getEnv().setDayNightRatioOverride(
		event->override_day_night_ratio.do_override,
		event->override_day_night_ratio.ratio_f * 1000.0f);
}

void Game::handleClientEvent_CloudParams(ClientEvent *event, CameraOrientation *cam)
{
	clouds->setDensity(event->cloud_params.density);
	clouds->setColorBright(video::SColor(event->cloud_params.color_bright));
	clouds->setColorAmbient(video::SColor(event->cloud_params.color_ambient));
	clouds->setColorShadow(video::SColor(event->cloud_params.color_shadow));
	clouds->setHeight(event->cloud_params.height);
	clouds->setThickness(event->cloud_params.thickness);
	clouds->setSpeed(v2f(event->cloud_params.speed_x, event->cloud_params.speed_y));
}

void Game::handleClientEvent_UpdateCamera(ClientEvent *event, CameraOrientation *cam)
{
	// no parameters to update here, this just makes sure the camera is in the
	// state it should be after something was changed.
	updateCameraMode();
}

void Game::processClientEvents(CameraOrientation *cam)
{
	// Seat 0: drain the queue against the live `client` / `cam_view_target`.
	// m_current_event_seat tells per-seat handlers (Hud*, ...) which
	// SeatRuntime to mutate; without it every extra seat's HUD events
	// would land in seat 0's HudElement list and the other seats would
	// render with no hearts / hotbar / lua HUDs at all.
	m_current_event_seat = 0;
	while (client->hasClientEvents()) {
		std::unique_ptr<ClientEvent> event(client->getClientEvent());
		FATAL_ERROR_IF(event->type >= CLIENTEVENT_MAX, "Invalid clientevent type");
		const ClientEventHandler& evHandler = clientEventHandler[event->type];
		(this->*evHandler.handler)(event.get(), cam);
	}

	if (m_splitscreen_seats <= 1)
		return;

	// Seats 1..N: each has its own Client (and ClientEvent queue). We
	// temporarily swap the global `client` pointer so handlers that read
	// it (PlayerDamage's modsLoaded() check, particle handler, ...) see
	// the right Client. We also swap `input` so per-seat handlers (damage
	// rumble, death screen, ...) resolve the correct JoystickController.
	// PlayerForceMove writes through the `cam` parameter, so feed each seat
	// its own CameraOrientation too, otherwise a server-side setplayer() on
	// seat 1 would yank seat 0's view.
	Client *saved_client = client;
	InputHandler *saved_input = input;
	for (u8 i = 1; i < m_splitscreen_seats; i++) {
		auto &seat = m_seats[i];
		if (!seat.client)
			continue;

		m_current_event_seat = i;
		client = seat.client;
		input = seat.input ? seat.input.get() : saved_input;

		while (client->hasClientEvents()) {
			std::unique_ptr<ClientEvent> event(client->getClientEvent());
			FATAL_ERROR_IF(event->type >= CLIENTEVENT_MAX,
					"Invalid clientevent type");
			const ClientEventHandler& evHandler =
					clientEventHandler[event->type];
			(this->*evHandler.handler)(event.get(), &seat.cam_view);
		}
	}
	client = saved_client;
	input = saved_input;
	m_current_event_seat = 0;
}

void Game::updateChat(f32 dtime)
{
	auto color_for = [](LogLevel level) -> const char* {
		switch (level) {
		case LL_ERROR  : return "\x1b(c@#F00)"; // red
		case LL_WARNING: return "\x1b(c@#EE0)"; // yellow
		case LL_INFO   : return "\x1b(c@#BBB)"; // grey
		case LL_VERBOSE: return "\x1b(c@#888)"; // dark grey
		case LL_TRACE  : return "\x1b(c@#888)"; // dark grey
		default        : return "";
		}
	};

	// Get new messages from error log buffer
	std::vector<LogEntry> entries = m_chat_log_buf.take();
	for (const auto& entry : entries) {
		std::string line;
		line.append(color_for(entry.level)).append(entry.combined);
		chat_backend->addMessage(L"", utf8_to_wide(line));
	}

	// Get new messages from client
	std::wstring message;
	while (client->getChatMessage(message)) {
		chat_backend->addUnparsedMessage(message);
	}

	// Remove old messages
	chat_backend->step(dtime);

	// Display all messages in a static text element
	auto &buf = chat_backend->getRecentBuffer();
	if (buf.getLinesModified()) {
		buf.resetLinesModified();
		m_game_ui->setChatText(chat_backend->getRecentChat(), buf.getLineCount());
	}

	// Make sure that the size is still correct
	m_game_ui->updateChatSize();
}

void Game::updateCamera(f32 dtime)
{
	auto update_one = [this, dtime](Client *c, Camera *cam) {
		ClientEnvironment &env = c->getEnv();
		LocalPlayer *player = env.getLocalPlayer();

		// For interaction purposes, get info about the held item
		ItemStack playeritem, hand;
		{
			ItemStack selected;
			playeritem = player->getWieldedItem(&selected, &hand);
		}

		ToolCapabilities playeritem_toolcap =
			playeritem.getToolCapabilities(itemdef_manager, &hand);

		float full_punch_interval = playeritem_toolcap.full_punch_interval;
		float tool_reload_ratio = runData.time_from_last_punch / full_punch_interval;

		tool_reload_ratio = std::min(tool_reload_ratio, 1.0f);
		cam->update(player, dtime, tool_reload_ratio);
		cam->step(dtime);

		if (!m_flags.disable_camera_update) {
			c->getEnv().getClientMap().updateCamera(cam->getPosition(),
				cam->getDirection(), cam->getFovMax(), cam->getOffset(),
				player->light_color);
		}
	};

	update_one(client, camera);
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++) {
			if (m_seats[i].client && m_seats[i].camera)
				update_one(m_seats[i].client, m_seats[i].camera);
		}
	}
}

void Game::updateCameraMode()
{
	auto update_one = [](Client *c, Camera *cam) {
		if (!c || !cam)
			return;

		LocalPlayer *player = c->getEnv().getLocalPlayer();

		// Obey server choice
		if (player->allowed_camera_mode != CAMERA_MODE_ANY)
			cam->setCameraMode(player->allowed_camera_mode);

		GenericCAO *playercao = player->getCAO();
		if (playercao) {
			// Make the player visible depending on camera mode.
			playercao->updateMeshCulling();
			playercao->setChildrenVisible(cam->getCameraMode() > CAMERA_MODE_FIRST);
		}
	};

	update_one(client, camera);
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++)
			update_one(m_seats[i].client, m_seats[i].camera);
	}
}

void Game::updateCameraOffset()
{
	auto update_one = [this](u8 idx, bool update_globals) {
		if (!m_seats[idx].client || !m_seats[idx].camera)
			return false;

		ClientEnvironment &env = m_seats[idx].client->getEnv();
		Camera *cam = m_seats[idx].camera;
		v3s16 old_camera_offset = cam->getOffset();

		cam->updateOffset();

		v3s16 camera_offset = cam->getOffset();
		bool changed = camera_offset != old_camera_offset;
		m_seats[idx].camera_offset_changed = changed;
		if (!changed)
			return false;

		if (!m_flags.disable_camera_update) {
			if (update_globals) {
				auto *shadow = RenderingEngine::get_shadow_renderer();
				if (shadow) {
					shadow->getDirectionalLight().updateCameraOffset(cam);
					// FIXME: I bet we can be smarter about this and don't need to redraw
					// the shadow map at all, but this is for someone else to figure out.
					if (!g_settings->getFlag("performance_tradeoffs"))
						shadow->setForceUpdateShadowMap();
				}
			}

			env.getClientMap().updateCamera(cam->getPosition(),
				cam->getDirection(), cam->getFovMax(), camera_offset,
				env.getLocalPlayer()->light_color);

			env.updateCameraOffset(camera_offset);
			if (update_globals)
				clouds->updateCameraOffset(camera_offset);
		}

		return changed;
	};

	m_camera_offset_changed = update_one(0, true);
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++)
			update_one(i, false);
	}
}

void Game::updateSound(f32 dtime)
{
	// Update sound listener
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	ClientActiveObject *parent = player->getParent();
	v3s16 camera_offset = camera->getOffset();
	sound_manager->updateListener(
			(1.0f/BS) * camera->getCameraNode()->getPosition()
					+ intToFloat(camera_offset, 1.0f),
			(1.0f/BS) * (parent ? parent->getVelocity() : player->getSpeed()),
			camera->getDirection(),
			camera->getCameraNode()->getUpVector());

	sound_volume_control(sound_manager.get(), device->isWindowActive());

	// Update sound maker
	ClientMap &map = client->getEnv().getClientMap();
	MapNode n = map.getNode(player->getFootstepNodePos());
	soundmaker->update(dtime, player->makes_footstep_sound,
			nodedef_manager->get(n).sound_footstep);
}


void Game::processPlayerInteraction(f32 dtime, bool show_hud)
{
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	const v3f camera_direction = camera->getDirection();
	const v3s16 camera_offset  = camera->getOffset();

	/*
		Calculate what block is the crosshair pointing to
	*/

	ItemStack selected_item, hand_item;
	const ItemStack &tool_item = player->getWieldedItem(&selected_item, &hand_item);

	const ItemDefinition &selected_def = tool_item.getDefinition(itemdef_manager);
	f32 d = getToolRange(tool_item, hand_item, itemdef_manager);

	core::line3d<f32> shootline;

	switch (camera->getCameraMode()) {
	case CAMERA_MODE_ANY:
	case CameraMode_END:
		assert(false);
		break;
	case CAMERA_MODE_FIRST:
		// Shoot from camera position, with bobbing
		shootline.start = camera->getPosition();
		break;
	case CAMERA_MODE_THIRD:
		// Shoot from player head, no bobbing
		shootline.start = camera->getHeadPosition();
		break;
	case CAMERA_MODE_THIRD_FRONT:
		shootline.start = camera->getHeadPosition();
		// prevent player pointing anything in front-view
		d = 0;
		break;
	}
	shootline.end = shootline.start + camera_direction * BS * d;

	if (isTouchShootlineUsed()) {
		shootline = g_touchcontrols->getShootline();
		// Scale shootline to the acual distance the player can reach
		shootline.end = shootline.start +
				shootline.getVector().normalize() * BS * d;
		shootline.start += intToFloat(camera_offset, BS);
		shootline.end += intToFloat(camera_offset, BS);
	}

	PointedThing pointed = updatePointedThing(shootline,
			selected_def.liquids_pointable,
			selected_def.pointabilities,
			!runData.btn_down_for_dig,
			camera_offset);

	if (pointed != runData.pointed_old)
		infostream << "Pointing at " << pointed.dump() << std::endl;

	if (g_touchcontrols) {
		auto mode = selected_def.touch_interaction.getMode(selected_def, pointed.type);
		g_touchcontrols->applyContextControls(mode);
		// applyContextControls may change dig/place input.
		// Update again so that TOSERVER_INTERACT packets have the correct controls set.
		player->control.dig = isKeyDown(KeyType::DIG);
		player->control.place = isKeyDown(KeyType::PLACE);
	}

	// Note that updating the selection mesh every frame is not particularly efficient,
	// but the halo rendering code is already inefficient so there's no point in optimizing it here
	hud->updateSelectionMesh(camera_offset);

	// Allow digging again if button is not pressed
	if (runData.digging_blocked && !isKeyDown(KeyType::DIG))
		runData.digging_blocked = false;

	/*
		Stop digging when
		- releasing dig button
		- pointing away from node
	*/
	if (runData.digging) {
		if (wasKeyReleased(KeyType::DIG)) {
			infostream << "Dig button released (stopped digging)" << std::endl;
			runData.digging = false;
		} else if (pointed != runData.pointed_old) {
			if (pointed.type == POINTEDTHING_NODE
					&& runData.pointed_old.type == POINTEDTHING_NODE
					&& pointed.node_undersurface
							== runData.pointed_old.node_undersurface) {
				// Still pointing to the same node, but a different face.
				// Don't reset.
			} else {
				infostream << "Pointing away from node (stopped digging)" << std::endl;
				runData.digging = false;
				hud->updateSelectionMesh(camera_offset);
			}
		}

		if (!runData.digging) {
			client->interact(INTERACT_STOP_DIGGING, runData.pointed_old);
			client->setCrack(-1, v3s16(0, 0, 0));
			runData.dig_time = 0.0;
		}
	} else if (runData.dig_instantly && wasKeyReleased(KeyType::DIG)) {
		// Remove e.g. torches faster when clicking instead of holding dig button
		runData.nodig_delay_timer = 0;
		runData.dig_instantly = false;
	}

	if (!runData.digging && runData.btn_down_for_dig && !isKeyDown(KeyType::DIG))
		runData.btn_down_for_dig = false;

	runData.punching = false;

	soundmaker->m_player_leftpunch_sound = SoundSpec();
	soundmaker->m_player_leftpunch_sound2 = pointed.type != POINTEDTHING_NOTHING ?
		selected_def.sound_use : selected_def.sound_use_air;

	// Prepare for repeating, unless we're not supposed to
	if (isKeyDown(KeyType::PLACE) && !g_settings->getBool("safe_dig_and_place"))
		runData.repeat_place_timer += dtime;
	else
		runData.repeat_place_timer = 0;

	if (selected_def.usable && isKeyDown(KeyType::DIG)) {
		if (wasKeyPressed(KeyType::DIG) && (!client->modsLoaded() ||
				!client->getScript()->on_item_use(selected_item, pointed)))
			client->interact(INTERACT_USE, pointed);
	} else if (pointed.type == POINTEDTHING_NODE) {
		handlePointingAtNode(pointed, selected_item, hand_item, dtime);
	} else if (pointed.type == POINTEDTHING_OBJECT) {
		v3f player_position  = player->getPosition();
		bool basic_debug_allowed = client->checkPrivilege("debug") || (player->hud_flags & HUD_FLAG_BASIC_DEBUG);
		handlePointingAtObject(pointed, tool_item, hand_item, player_position,
				m_game_ui->m_flags.show_basic_debug && basic_debug_allowed);
	} else if (isKeyDown(KeyType::DIG)) {
		// When button is held down in air, show continuous animation
		runData.punching = true;
		// Run callback even though item is not usable
		if (wasKeyPressed(KeyType::DIG) && client->modsLoaded())
			client->getScript()->on_item_use(selected_item, pointed);
	} else if (wasKeyPressed(KeyType::PLACE)) {
		handlePointingAtNothing(selected_item);
	}

	runData.pointed_old = pointed;

	if (runData.punching || wasKeyPressed(KeyType::DIG))
		camera->setDigging(0); // dig animation

	input->clearWasKeyPressed();
	input->clearWasKeyReleased();
	// Ensure DIG & PLACE are marked as handled
	wasKeyDown(KeyType::DIG);
	wasKeyDown(KeyType::PLACE);
}


PointedThing Game::updatePointedThing(
	const core::line3d<f32> &shootline,
	bool liquids_pointable,
	const std::optional<Pointabilities> &pointabilities,
	bool look_for_object,
	const v3s16 &camera_offset)
{
	std::vector<aabb3f> *selectionboxes = hud->getSelectionBoxes();
	selectionboxes->clear();
	hud->setSelectedFaceNormal(v3f());
	static thread_local const bool show_entity_selectionbox = g_settings->getBool(
		"show_entity_selectionbox");

	ClientEnvironment &env = client->getEnv();
	ClientMap &map = env.getClientMap();
	const NodeDefManager *nodedef = map.getNodeDefManager();

	runData.selected_object = NULL;
	hud->pointing_at_object = false;

	RaycastState s(shootline, look_for_object, liquids_pointable, pointabilities);
	PointedThing result;
	env.continueRaycast(&s, &result);
	if (result.type == POINTEDTHING_OBJECT) {
		hud->pointing_at_object = true;

		runData.selected_object = client->getEnv().getActiveObject(result.object_id);
		aabb3f selection_box{{0.0f, 0.0f, 0.0f}};
		if (show_entity_selectionbox && runData.selected_object->doShowSelectionBox() &&
				runData.selected_object->getSelectionBox(&selection_box)) {
			v3f pos = runData.selected_object->getPosition();
			selectionboxes->push_back(selection_box);
			hud->setSelectionPos(pos, camera_offset);
			GenericCAO* gcao = dynamic_cast<GenericCAO*>(runData.selected_object);
			if (gcao != nullptr && gcao->getProperties().rotate_selectionbox)
				hud->setSelectionRotationRadians(gcao->getSceneNode()
						->getAbsoluteTransformation().getRotationRadians());
			else
				hud->setSelectionRotationRadians(v3f());
		}
		hud->setSelectedFaceNormal(result.raw_intersection_normal);
	} else if (result.type == POINTEDTHING_NODE) {
		// Update selection boxes
		MapNode n = map.getNode(result.node_undersurface);
		std::vector<aabb3f> boxes;
		n.getSelectionBoxes(nodedef, &boxes,
			n.getNeighbors(result.node_undersurface, &map));

		f32 d = 0.002f * BS;
		for (aabb3f box : boxes) {
			box.MinEdge -= v3f(d, d, d);
			box.MaxEdge += v3f(d, d, d);
			selectionboxes->push_back(box);
		}
		hud->setSelectionPos(intToFloat(result.node_undersurface, BS),
			camera_offset);
		hud->setSelectionRotationRadians(v3f());
		hud->setSelectedFaceNormal(result.intersection_normal);
	}

	// Update selection mesh light level and vertex colors
	if (!selectionboxes->empty()) {
		v3f pf = hud->getSelectionPos();
		v3s16 p = floatToInt(pf, BS);

		// Get selection mesh light level
		MapNode n = map.getNode(p);
		u16 node_light = getInteriorLight(n, -1, nodedef);
		u16 light_level = node_light;

		for (const v3s16 &dir : g_6dirs) {
			n = map.getNode(p + dir);
			node_light = getInteriorLight(n, -1, nodedef);
			if (node_light > light_level)
				light_level = node_light;
		}

		u32 daynight_ratio = client->getEnv().getDayNightRatio();
		video::SColor c;
		final_color_blend(&c, light_level, daynight_ratio);

		// Modify final color a bit with time
		u32 timer = client->getEnv().getFrameTime() % 5000;
		float timerf = (float) (core::PI * ((timer / 2500.0) - 0.5));
		float sin_r = 0.08f * std::sin(timerf);
		float sin_g = 0.08f * std::sin(timerf + core::PI * 0.5f);
		float sin_b = 0.08f * std::sin(timerf + core::PI);
		c.setRed(core::clamp(core::round32(c.getRed() * (0.8 + sin_r)), 0, 255));
		c.setGreen(core::clamp(core::round32(c.getGreen() * (0.8 + sin_g)), 0, 255));
		c.setBlue(core::clamp(core::round32(c.getBlue() * (0.8 + sin_b)), 0, 255));

		// Set mesh final color
		hud->setSelectionMeshColor(c);
	}
	return result;
}


void Game::handlePointingAtNothing(const ItemStack &playerItem)
{
	infostream << "Attempted to place item while pointing at nothing" << std::endl;
	PointedThing fauxPointed;
	fauxPointed.type = POINTEDTHING_NOTHING;
	client->interact(INTERACT_ACTIVATE, fauxPointed);
}


void Game::handlePointingAtNode(const PointedThing &pointed,
	const ItemStack &selected_item, const ItemStack &hand_item, f32 dtime)
{
	v3s16 nodepos = pointed.node_undersurface;
	v3s16 neighborpos = pointed.node_abovesurface;

	/*
		Check information text of node
	*/

	ClientMap &map = client->getEnv().getClientMap();

	if (runData.nodig_delay_timer <= 0.0 && isKeyDown(KeyType::DIG)
			&& !runData.digging_blocked
			&& client->checkPrivilege("interact")) {
		handleDigging(pointed, nodepos, selected_item, hand_item, dtime);
	}

	// This should be done after digging handling
	NodeMetadata *meta = map.getNodeMetadata(nodepos);

	if (meta) {
		m_game_ui->setInfoText(unescape_translate(utf8_to_wide(
			meta->getString("infotext"))));
	} else {
		MapNode n = map.getNode(nodepos);

		if (nodedef_manager->get(n).name == "unknown") {
			m_game_ui->setInfoText(L"Unknown node");
		}
	}

	if ((wasKeyPressed(KeyType::PLACE) ||
			runData.repeat_place_timer >= m_repeat_place_time) &&
			client->checkPrivilege("interact")) {
		runData.repeat_place_timer = 0;
		infostream << "Place button pressed while looking at ground" << std::endl;

		// Placing animation (always shown for feedback)
		camera->setDigging(1);

		soundmaker->m_player_rightpunch_sound = SoundSpec();

		// If the wielded item has node placement prediction,
		// make that happen
		// And also set the sound and send the interact
		// But first check for meta formspec and rightclickable
		auto &def = selected_item.getDefinition(itemdef_manager);
		bool placed = nodePlacement(def, selected_item, nodepos, neighborpos,
			pointed, meta);

		if (placed && client->modsLoaded())
			client->getScript()->on_placenode(pointed, def);
	}
}

bool Game::nodePlacement(const ItemDefinition &selected_def,
	const ItemStack &selected_item, const v3s16 &nodepos, const v3s16 &neighborpos,
	const PointedThing &pointed, const NodeMetadata *meta)
{
	const auto &prediction = selected_def.node_placement_prediction;

	const NodeDefManager *nodedef = client->ndef();
	ClientMap &map = client->getEnv().getClientMap();
	MapNode node;
	bool is_valid_position;

	node = map.getNode(nodepos, &is_valid_position);
	if (!is_valid_position) {
		soundmaker->m_player_rightpunch_sound = selected_def.sound_place_failed;
		return false;
	}

	// formspec in meta
	if (meta && !meta->getString("formspec").empty() && !input->isRandom()
			&& !isKeyDown(KeyType::SNEAK)) {
		// on_rightclick callbacks are called anyway
		if (nodedef_manager->get(map.getNode(nodepos)).rightclickable)
			client->interact(INTERACT_PLACE, pointed);

		m_game_formspec.showNodeFormspec(meta->getString("formspec"), nodepos);
		return false;
	}

	// on_rightclick callback
	if (prediction.empty() || (nodedef->get(node).rightclickable &&
			!isKeyDown(KeyType::SNEAK))) {
		// Report to server
		client->interact(INTERACT_PLACE, pointed);
		return false;
	}

	verbosestream << "Node placement prediction for "
		<< selected_def.name << " is " << prediction << std::endl;
	v3s16 p = neighborpos;

	// Place inside node itself if buildable_to
	MapNode n_under = map.getNode(nodepos, &is_valid_position);
	if (is_valid_position) {
		if (nodedef->get(n_under).buildable_to) {
			p = nodepos;
		} else {
			node = map.getNode(p, &is_valid_position);
			if (is_valid_position && !nodedef->get(node).buildable_to) {
				soundmaker->m_player_rightpunch_sound = selected_def.sound_place_failed;
				// Report to server
				client->interact(INTERACT_PLACE, pointed);
				return false;
			}
		}
	}

	// Find id of predicted node
	content_t id;
	bool found = nodedef->getId(prediction, id);

	if (!found) {
		errorstream << "Node placement prediction failed for "
			<< selected_def.name << " (places " << prediction
			<< ") - Name not known" << std::endl;
		// Handle this as if prediction was empty
		// Report to server
		client->interact(INTERACT_PLACE, pointed);
		return false;
	}

	const ContentFeatures &predicted_f = nodedef->get(id);

	// Compare core.item_place_node() for what the server does with param2
	MapNode predicted_node(id, 0, 0);

	const auto place_param2 = selected_def.place_param2;

	if (place_param2) {
		predicted_node.setParam2(*place_param2);
	} else if (predicted_f.param_type_2 == CPT2_WALLMOUNTED ||
			predicted_f.param_type_2 == CPT2_COLORED_WALLMOUNTED) {
		v3s16 dir = nodepos - neighborpos;

		if (abs(dir.Y) > MYMAX(abs(dir.X), abs(dir.Z))) {
			// If you change this code, also change builtin/game/item.lua
			u8 predicted_param2 = dir.Y < 0 ? 1 : 0;
			if (selected_def.wallmounted_rotate_vertical) {
				bool rotate90 = false;
				v3f ppos = client->getEnv().getLocalPlayer()->getPosition() / BS;
				v3f pdir = v3f::from(neighborpos) - ppos;
				switch (predicted_f.drawtype) {
					case NDT_TORCHLIKE: {
						rotate90 = !((pdir.X < 0 && pdir.Z > 0) ||
								(pdir.X > 0 && pdir.Z < 0));
						if (dir.Y > 0) {
							rotate90 = !rotate90;
						}
						break;
					};
					case NDT_SIGNLIKE: {
						rotate90 = std::abs(pdir.X) < std::abs(pdir.Z);
						break;
					}
					default: {
						rotate90 = std::abs(pdir.X) > std::abs(pdir.Z);
						break;
					}
				}
				if (rotate90) {
					predicted_param2 += 6;
				}
			}
			predicted_node.setParam2(predicted_param2);
		} else if (abs(dir.X) > abs(dir.Z)) {
			predicted_node.setParam2(dir.X < 0 ? 3 : 2);
		} else {
			predicted_node.setParam2(dir.Z < 0 ? 5 : 4);
		}
	} else if (predicted_f.param_type_2 == CPT2_FACEDIR ||
			predicted_f.param_type_2 == CPT2_COLORED_FACEDIR ||
			predicted_f.param_type_2 == CPT2_4DIR ||
			predicted_f.param_type_2 == CPT2_COLORED_4DIR) {
		v3s16 dir = nodepos - floatToInt(client->getEnv().getLocalPlayer()->getPosition(), BS);

		if (abs(dir.X) > abs(dir.Z)) {
			predicted_node.setParam2(dir.X < 0 ? 3 : 1);
		} else {
			predicted_node.setParam2(dir.Z < 0 ? 2 : 0);
		}
	}

	// Check attachment if node is in group attached_node
	int an = itemgroup_get(predicted_f.groups, "attached_node");
	if (an != 0) {
		v3s16 pp;

		if (an == 3) {
			pp = p + v3s16(0, -1, 0);
		} else if (an == 4) {
			pp = p + v3s16(0, 1, 0);
		} else if (an == 2) {
			if (predicted_f.param_type_2 == CPT2_FACEDIR ||
					predicted_f.param_type_2 == CPT2_COLORED_FACEDIR ||
					predicted_f.param_type_2 == CPT2_4DIR ||
					predicted_f.param_type_2 == CPT2_COLORED_4DIR) {
				pp = p + facedir_dirs[predicted_node.getFaceDir(nodedef)];
			} else {
				pp = p;
			}
		} else if (predicted_f.param_type_2 == CPT2_WALLMOUNTED ||
				predicted_f.param_type_2 == CPT2_COLORED_WALLMOUNTED) {
			pp = p + predicted_node.getWallMountedDir(nodedef);
		} else {
			pp = p + v3s16(0, -1, 0);
		}

		if (!nodedef->get(map.getNode(pp)).walkable) {
			soundmaker->m_player_rightpunch_sound = selected_def.sound_place_failed;
			// Report to server
			client->interact(INTERACT_PLACE, pointed);
			return false;
		}
	}

	// Apply color
	if (!place_param2 && (predicted_f.param_type_2 == CPT2_COLOR
			|| predicted_f.param_type_2 == CPT2_COLORED_FACEDIR
			|| predicted_f.param_type_2 == CPT2_COLORED_4DIR
			|| predicted_f.param_type_2 == CPT2_COLORED_WALLMOUNTED)) {
		const auto &indexstr = selected_item.metadata.
			getString("palette_index", 0);
		if (!indexstr.empty()) {
			s32 index = mystoi(indexstr);
			if (predicted_f.param_type_2 == CPT2_COLOR) {
				predicted_node.setParam2(index);
			} else if (predicted_f.param_type_2 == CPT2_COLORED_WALLMOUNTED) {
				// param2 = pure palette index + other
				predicted_node.setParam2((index & 0xf8) | (predicted_node.getParam2() & 0x07));
			} else if (predicted_f.param_type_2 == CPT2_COLORED_FACEDIR) {
				// param2 = pure palette index + other
				predicted_node.setParam2((index & 0xe0) | (predicted_node.getParam2() & 0x1f));
			} else if (predicted_f.param_type_2 == CPT2_COLORED_4DIR) {
				// param2 = pure palette index + other
				predicted_node.setParam2((index & 0xfc) | (predicted_node.getParam2() & 0x03));
			}
		}
	}

	// Add node to client map
	try {
		LocalPlayer *player = client->getEnv().getLocalPlayer();

		// Don't place node when player would be inside new node
		// NOTE: This is to be eventually implemented by a mod as client-side Lua
		if (!predicted_f.walkable ||
				g_settings->getBool("enable_build_where_you_stand") ||
				(client->checkPrivilege("noclip") && g_settings->getBool("noclip")) ||
				(predicted_f.walkable &&
					neighborpos != player->getStandingNodePos() + v3s16(0, 1, 0) &&
					neighborpos != player->getStandingNodePos() + v3s16(0, 2, 0))) {
			// This triggers the required mesh update too
			client->addNode(p, predicted_node);
			// Report to server
			client->interact(INTERACT_PLACE, pointed);
			// A node is predicted, also play a sound
			soundmaker->m_player_rightpunch_sound = selected_def.sound_place;
			return true;
		} else {
			soundmaker->m_player_rightpunch_sound = selected_def.sound_place_failed;
			return false;
		}
	} catch (const InvalidPositionException &e) {
		errorstream << "Node placement prediction failed for "
			<< selected_def.name << " (places "
			<< prediction << ") - Position not loaded" << std::endl;
		soundmaker->m_player_rightpunch_sound = selected_def.sound_place_failed;
		return false;
	}
}

void Game::handlePointingAtObject(const PointedThing &pointed, const ItemStack &tool_item,
		const ItemStack &hand_item, const v3f &player_position, bool show_debug)
{
	std::wstring infotext = unescape_translate(
		utf8_to_wide(runData.selected_object->infoText()));

	if (show_debug) {
		if (!infotext.empty()) {
			infotext += L"\n";
		}
		infotext += utf8_to_wide(runData.selected_object->debugInfoText());
	}

	m_game_ui->setInfoText(infotext);

	if (isKeyDown(KeyType::DIG)) {
		bool do_punch = false;
		bool do_punch_damage = false;

		if (runData.object_hit_delay_timer <= 0.0) {
			do_punch = true;
			do_punch_damage = true;
			runData.object_hit_delay_timer = object_hit_delay;
		}

		if (wasKeyPressed(KeyType::DIG))
			do_punch = true;

		if (do_punch) {
			infostream << "Punched object" << std::endl;
			runData.punching = true;
			runData.nodig_delay_timer = std::max(0.15f, m_repeat_dig_time);
		}

		if (do_punch_damage) {
			// Report direct punch
			v3f objpos = runData.selected_object->getPosition();
			v3f dir = (objpos - player_position).normalize();

			bool disable_send = runData.selected_object->directReportPunch(
					dir, &tool_item, &hand_item, runData.time_from_last_punch);
			runData.time_from_last_punch = 0;

			if (!disable_send)
				client->interact(INTERACT_START_DIGGING, pointed);
		}
	} else if (wasKeyDown(KeyType::PLACE)) {
		infostream << "Pressed place button while pointing at object" << std::endl;
		client->interact(INTERACT_PLACE, pointed);  // place
	}
}


void Game::handleDigging(const PointedThing &pointed, const v3s16 &nodepos,
		const ItemStack &selected_item, const ItemStack &hand_item, f32 dtime)
{
	// See also: serverpackethandle.cpp, action == 2
	LocalPlayer *player = client->getEnv().getLocalPlayer();
	ClientMap &map = client->getEnv().getClientMap();
	MapNode n = map.getNode(nodepos);
	const auto &features = nodedef_manager->get(n);
	const ItemStack &tool_item = selected_item.name.empty() ? hand_item : selected_item;

	// NOTE: Similar piece of code exists on the server side for
	// cheat detection.
	// Get digging parameters
	DigParams params = getDigParams(features.groups,
			&tool_item.getToolCapabilities(itemdef_manager, &hand_item),
			tool_item.wear);

	// If can't dig, try hand
	if (!params.diggable) {
		params = getDigParams(features.groups,
				&hand_item.getToolCapabilities(itemdef_manager));
	}

	if (!params.diggable) {
		// I guess nobody will wait for this long
		runData.dig_time_complete = 10000000.0;
	} else {
		runData.dig_time_complete = params.time;

		client->getParticleManager()->addNodeParticle(player, nodepos, n);
	}

	if (!runData.digging) {
		infostream << "Started digging" << std::endl;
		runData.dig_instantly = runData.dig_time_complete == 0;
		if (client->modsLoaded() && client->getScript()->on_punchnode(nodepos, n))
			return;

		client->interact(INTERACT_START_DIGGING, pointed);
		runData.digging = true;
		runData.btn_down_for_dig = true;
	}

	if (!runData.dig_instantly) {
		runData.dig_index = (float)crack_animation_length
				* runData.dig_time
				/ runData.dig_time_complete;
	} else {
		// This is for e.g. torches
		runData.dig_index = crack_animation_length;
	}

	const auto &sound_dig = features.sound_dig;

	if (sound_dig.exists() && params.diggable) {
		if (sound_dig.name == "__group") {
			if (!params.main_group.empty()) {
				soundmaker->m_player_leftpunch_sound.gain = 0.5;
				soundmaker->m_player_leftpunch_sound.name =
						std::string("default_dig_") +
						params.main_group;
			}
		} else {
			soundmaker->m_player_leftpunch_sound = sound_dig;
		}
	}

	// Don't show cracks if not diggable
	if (runData.dig_time_complete >= 100000.0) {
	} else if (runData.dig_index < crack_animation_length) {
		client->setCrack(runData.dig_index, nodepos);
	} else {
		infostream << "Digging completed" << std::endl;
		client->setCrack(-1, v3s16(0, 0, 0));

		runData.dig_time = 0;
		runData.digging = false;
		// we successfully dug, now block it from repeating if we want to be safe
		if (g_settings->getBool("safe_dig_and_place"))
			runData.digging_blocked = true;

		runData.nodig_delay_timer =
				runData.dig_time_complete / (float)crack_animation_length;

		// We don't want a corresponding delay to very time consuming nodes
		// and nodes without digging time (e.g. torches) get a fixed delay.
		if (runData.nodig_delay_timer > 0.3f)
			runData.nodig_delay_timer = 0.3f;
		else if (runData.dig_instantly)
			runData.nodig_delay_timer = 0.15f;

		// Ensure that the delay between breaking nodes
		// (dig_time_complete + nodig_delay_timer) is at least the
		// value of the repeat_dig_time setting.
		runData.nodig_delay_timer = std::max(runData.nodig_delay_timer,
				m_repeat_dig_time - runData.dig_time_complete);

		if (client->modsLoaded() &&
				client->getScript()->on_dignode(nodepos, n)) {
			return;
		}

		if (features.node_dig_prediction == "air") {
			client->removeNode(nodepos);
		} else if (!features.node_dig_prediction.empty()) {
			content_t id;
			bool found = nodedef_manager->getId(features.node_dig_prediction, id);
			if (found)
				client->addNode(nodepos, id, true);
		}
		// implicit else: no prediction

		client->interact(INTERACT_DIGGING_COMPLETED, pointed);

		client->getParticleManager()->addDiggingParticles(player, nodepos, n);

		// Send event to trigger sound
		client->getEventManager()->put(new NodeDugEvent(nodepos, n));
	}

	if (runData.dig_time_complete < 100000.0) {
		runData.dig_time += dtime;
	} else {
		runData.dig_time = 0;
		client->setCrack(-1, nodepos);
	}

	camera->setDigging(0);  // Dig animation
}

void Game::updateFrame(ProfilerGraph *graph, RunStats *stats, f32 dtime,
		const CameraOrientation &cam)
{
	ZoneScoped;
	TimeTaker tt_update("Game::updateFrame()");
	LocalPlayer *player = client->getEnv().getLocalPlayer();

	/*
		Frame time
	*/

	client->getEnv().updateFrameTime(m_is_paused);

	/*
		Fog range
	*/

	if (sky->getFogDistance() >= 0) {
		draw_control->wanted_range = MYMIN(draw_control->wanted_range, sky->getFogDistance());
	}
	if (draw_control->range_all && sky->getFogDistance() < 0) {
		runData.fog_range = FOG_RANGE_ALL;
	} else {
		runData.fog_range = draw_control->wanted_range * BS;
	}

	/*
		Calculate general brightness
	*/
	u32 daynight_ratio = client->getEnv().getDayNightRatio();
	float time_brightness = decode_light_f((float)daynight_ratio / 1000.0f);
	float direct_brightness;
	bool sunlight_seen;

	// When in noclip mode force same sky brightness as above ground so you
	// can see properly
	bool noclip_fly = draw_control->allow_noclip &&
			m_cache_enable_free_move &&
			client->checkPrivilege("fly");
	if (!sky->getAutoCaveBrightness() || noclip_fly) {
		direct_brightness = time_brightness;
		sunlight_seen = true;
	} else {
		float old_brightness = sky->getBrightness();
		direct_brightness = client->getEnv().getClientMap()
				.getBackgroundBrightness(MYMIN(runData.fog_range * 1.2, 60 * BS),
						daynight_ratio, (int)(old_brightness * 255.5), &sunlight_seen)
				/ 255.0;
	}

	float time_of_day_smooth = runData.time_of_day_smooth;
	float time_of_day = client->getEnv().getTimeOfDayF();

	static const float maxsm = 0.05f;
	static const float todsm = 0.05f;

	if (std::fabs(time_of_day - time_of_day_smooth) > maxsm &&
			std::fabs(time_of_day - time_of_day_smooth + 1.0) > maxsm &&
			std::fabs(time_of_day - time_of_day_smooth - 1.0) > maxsm)
		time_of_day_smooth = time_of_day;

	if (time_of_day_smooth > 0.8 && time_of_day < 0.2)
		time_of_day_smooth = time_of_day_smooth * (1.0 - todsm)
				+ (time_of_day + 1.0) * todsm;
	else
		time_of_day_smooth = time_of_day_smooth * (1.0 - todsm)
				+ time_of_day * todsm;

	runData.time_of_day_smooth = time_of_day_smooth;

	sky->update(time_of_day_smooth, time_brightness, direct_brightness,
			sunlight_seen, camera->getCameraMode(), player->getYaw(),
			player->getPitch());

	/*
		Update clouds
	*/
	updateClouds(dtime);

	/*
		Update particles
	*/
	client->getParticleManager()->step(dtime);

	/*
		Damage camera tilt
	*/
	if (player->hurt_tilt_timer > 0.0f) {
		player->hurt_tilt_timer -= dtime * 6.0f;

		if (player->hurt_tilt_timer < 0.0f)
			player->hurt_tilt_strength = 0.0f;
	}

	/*
		Update minimap pos and rotation
	*/
	if (mapper && m_game_ui->m_flags.show_hud) {
		mapper->setPos(floatToInt(player->getPosition(), BS));
		mapper->setAngle(player->getYaw());
	}

	/*
		Get chat messages from client
	*/

	updateChat(dtime);

	/*
		Inventory
	*/

	if (player->getWieldIndex() != runData.new_playeritem)
		client->setPlayerItem(runData.new_playeritem);

	if (client->updateWieldedItem()) {
		// Update wielded tool
		ItemStack selected_item, hand_item;
		ItemStack &tool_item = player->getWieldedItem(&selected_item, &hand_item);

		bool skip_anim = client->consumeSkipNextWieldAnimation();
		camera->wield(tool_item, !skip_anim);
	}

	// Split-screen: each extra seat has its own Camera/wieldnode that needs
	// to be initialized with the player's actual held item, otherwise the
	// wield mesh stays as the empty default ItemStack and the arm/tool
	// never appears in their first-person view.
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++) {
			Client *sc = m_seats[i].client;
			Camera *scam = m_seats[i].camera;
			if (!sc || !scam)
				continue;
			LocalPlayer *sp = sc->getEnv().getLocalPlayer();
			if (!sp)
				continue;
			// First-time priming: force the wield mesh once even if the
			// inventory hasn't been "modified" since the seat connected.
			// We track this with a flag on SeatRuntime so we only do the
			// initial prime, then rely on updateWieldedItem() afterwards.
			bool needs_update = sc->updateWieldedItem();
			if (!m_seats[i].wield_primed) {
				needs_update = true;
				m_seats[i].wield_primed = true;
			}
			if (needs_update) {
				ItemStack selected_item, hand_item;
				ItemStack &tool_item = sp->getWieldedItem(&selected_item, &hand_item);
				bool skip_anim = sc->consumeSkipNextWieldAnimation();
				scam->wield(tool_item, !skip_anim);
			}
		}
	}

	/*
		Update block draw list every 200ms or when camera direction has
		changed much
	*/
	runData.update_draw_list_timer += dtime;
	runData.touch_blocks_timer += dtime;

	constexpr float update_draw_list_delta = 0.2f;
	constexpr float touch_mapblock_delta = 4.0f;

	v3f camera_direction = camera->getDirection();

	// call only one of updateDrawList, touchMapBlocks, or updateShadow per frame
	// (the else-ifs below are intentional)
	if (runData.update_draw_list_timer >= update_draw_list_delta
			|| runData.update_draw_list_last_cam_dir.getDistanceFrom(camera_direction) > 0.2
			|| m_camera_offset_changed
			|| client->getEnv().getClientMap().needsUpdateDrawList()) {
		runData.update_draw_list_timer = 0;
		client->getEnv().getClientMap().updateDrawList();
		runData.update_draw_list_last_cam_dir = camera_direction;
	} else if (runData.touch_blocks_timer > touch_mapblock_delta) {
		client->getEnv().getClientMap().touchMapBlocks();
		runData.touch_blocks_timer = 0;
	} else if (RenderingEngine::get_shadow_renderer()) {
		updateShadows();
	}

	// Split-screen: the main draw-list/touch logic above only drives the
	// primary global `client`. Each additional in-process client has its own
	// ClientMap and draw list, so keep those updated too; otherwise seat 1+
	// can draw CAOs and sky but no map blocks.
	if (m_splitscreen_seats > 1) {
		for (u8 i = 1; i < m_splitscreen_seats; i++) {
			if (!m_seats[i].client || !m_seats[i].camera)
				continue;

			ClientMap &seat_map = m_seats[i].client->getEnv().getClientMap();
			v3f seat_camera_direction = m_seats[i].camera->getDirection();
			m_seats[i].update_draw_list_timer += dtime;
			m_seats[i].touch_blocks_timer += dtime;

			if (m_seats[i].update_draw_list_timer >= update_draw_list_delta
					|| m_seats[i].update_draw_list_last_cam_dir
							.getDistanceFrom(seat_camera_direction) > 0.2f
					|| m_seats[i].camera_offset_changed
					|| seat_map.needsUpdateDrawList()) {
				m_seats[i].update_draw_list_timer = 0.0f;
				seat_map.updateDrawList();
				m_seats[i].update_draw_list_last_cam_dir = seat_camera_direction;
			} else if (m_seats[i].touch_blocks_timer > touch_mapblock_delta) {
				seat_map.touchMapBlocks();
				m_seats[i].touch_blocks_timer = 0.0f;
			}
		}
	}

	m_game_ui->update(*stats, client, draw_control, cam, runData.pointed_old,
			gui_chat_console.get(), dtime);

	m_game_formspec.update();

	/*
		==================== Drawing begins ====================
	*/
	if (device->isWindowVisible())
		drawScene(graph, stats);
	/*
		==================== End scene ====================
	*/

	// Damage flash is drawn in drawScene, but the timing update is done here to
	// keep dtime out of the drawing code.
	if (runData.damage_flash > 0.0f) {
		runData.damage_flash -= 384.0f * dtime;
	}

	g_profiler->avg("Game::updateFrame(): update frame [ms]", tt_update.stop(true));
}

void Game::updateClouds(float dtime)
{
	if (this->sky->getCloudsVisible()) {
		this->clouds->setVisible(true);
		this->clouds->step(dtime);
		// this->camera->getPosition is not enough for third-person camera.
		v3f camera_node_position = this->camera->getCameraNode()->getPosition();
		v3s16 camera_offset      = this->camera->getOffset();
		camera_node_position.X   = camera_node_position.X + camera_offset.X * BS;
		camera_node_position.Y   = camera_node_position.Y + camera_offset.Y * BS;
		camera_node_position.Z   = camera_node_position.Z + camera_offset.Z * BS;
		this->clouds->update(camera_node_position, this->sky->getCloudColor());
		if (this->clouds->isCameraInsideCloud() && this->fogEnabled()) {
			// If camera is inside cloud and fog is enabled, use cloud's colors as sky colors.
			video::SColor clouds_dark = this->clouds->getColor().getInterpolated(
					video::SColor(255, 0, 0, 0), 0.9);
			this->sky->overrideColors(clouds_dark, this->clouds->getColor());
			this->sky->setInClouds(true);
			this->runData.fog_range = std::fmin(this->runData.fog_range * 0.5f, 32.0f * BS);
			// Clouds are not drawn in this case.
			this->clouds->setVisible(false);
		}
	} else {
		this->clouds->setVisible(false);
	}
}

/* Log times and stuff for visualization */
inline void Game::updateProfilerGraphs(ProfilerGraph *graph)
{
	Profiler::GraphValues values;
	g_profiler->graphPop(values);
	graph->put(values);
}

/****************************************************************************
 * Shadows
 *****************************************************************************/
void Game::updateShadows()
{
	ShadowRenderer *shadow = RenderingEngine::get_shadow_renderer();
	if (!shadow)
		return;

	float in_timeofday = std::fmod(runData.time_of_day_smooth, 1.0f);

	const auto &lighting = client->getEnv().getLocalPlayer()->getLighting();
	shadow->setShadowTint(lighting.shadow_tint);

	const float offset_constant = 10000.0f;

	v3f light;
	if (lighting.shadow_direction.getLengthSQ() > 0.0f) {
		// Custom shadow direction: bypass sun/moon visibility check
		shadow->setShadowIntensity(lighting.shadow_intensity);
		light = lighting.shadow_direction;
	} else {
		float timeoftheday = getWickedTimeOfDay(in_timeofday);
		bool is_day = timeoftheday > 0.25f && timeoftheday < 0.75f;
		bool is_shadow_visible = is_day ? sky->getSunVisible() : sky->getMoonVisible();
		shadow->setShadowIntensity(is_shadow_visible ? lighting.shadow_intensity : 0.0f);
		light = is_day ? sky->getSunDirection() : sky->getMoonDirection();
	}

	v3f sun_pos = light * offset_constant;
	shadow->getDirectionalLight().setDirection(sun_pos);
	shadow->setTimeOfDay(in_timeofday);

	shadow->getDirectionalLight().updateFrustum(camera, client);
}

void Game::drawScene(ProfilerGraph *graph, RunStats *stats)
{
	ZoneScoped;

	const video::SColor fog_color = this->sky->getFogColor();
	const video::SColor sky_color = this->sky->getSkyColor();

	/*
		Fog
	*/
	if (this->fogEnabled()) {
		this->driver->setFog(
				fog_color,
				video::EFT_FOG_LINEAR,
				this->runData.fog_range * this->sky->getFogStart(),
				this->runData.fog_range * 1.0f,
				0.f, // unused
				false, // pixel fog
				true // range fog
		);
	} else {
		this->driver->setFog(
				fog_color,
				video::EFT_FOG_LINEAR,
				FOG_RANGE_ALL,
				FOG_RANGE_ALL + 100 * BS,
				0.f, // unused
				false, // pixel fog
				false // range fog
		);
	}

	/*
		Drawing
	*/
	TimeTaker tt_draw("Draw scene", nullptr, PRECISION_MICRO);
	this->driver->beginScene(true, true, sky_color);

	const v2u32 screensize = this->driver->getScreenSize();
	const core::rect<s32> fullvp(0, 0, screensize.X, screensize.Y);
	const core::rect<s32> vp_top(0, 0, screensize.X, (s32)(screensize.Y / 2));
	const core::rect<s32> vp_bottom(0, (s32)(screensize.Y / 2), screensize.X, (s32)screensize.Y);

	// Couch co-op rendering.
	//
	// Single-seat path: just call the normal pipeline-based draw.
	// Split-screen path: render every seat into its own off-screen texture
	// (so each seat can draw scene + wield + HUD without any other seat's
	// content polluting it), then 2D-blit each texture into its viewport
	// rectangle on screen.
	//
	// Each seat's Client owns an independent scene::ISceneManager (created
	// in Game::initializeSeats() via smgr->createNewSceneManager(false)).
	// That means seat N's CAOs / ClientMap / camera helper nodes live in a
	// scene tree that doesn't contain any other seat's content - so drawing
	// a seat is just "activate its smgr's camera and call drawAll() on its
	// smgr". No cross-seat visibility juggling is needed; the local-player
	// mesh hide already works correctly in single-player mode and the same
	// path now applies to every seat.
	//
	// This bypasses the post-processing pipeline (ScreenTarget always clears
	// the entire framebuffer, which would wipe each previous seat's render),
	// so split-screen mode currently lacks features like FXAA / bloom. It
	// keeps shadows-off, plain-pipeline rendering for each seat. Adding the
	// post-processing pipeline back would mean swapping the pipeline's
	// ScreenTarget for a per-seat TextureBufferOutput - a follow-up.
	m_seats[0].scene_root = client->getSceneRoot();

	auto draw_one_fullscreen = [&](u8 idx, const core::rect<s32> &vp, bool show_hud) {
		if (!m_seats[idx].client || !m_seats[idx].camera || !m_seats[idx].hud)
			return;

		this->driver->setViewPort(vp);
		// Activate this seat's camera in its own scene manager (= the
		// engine's main smgr for seat 0; an independent sub-manager for
		// any seat created by initializeSeats()).
		m_seats[idx].client->getSceneManager()->setActiveCamera(
				m_seats[idx].camera->getCameraNode());

		const LocalPlayer *player = m_seats[idx].client->getEnv().getLocalPlayer();
		bool draw_wield_tool = (show_hud &&
				(player->hud_flags & HUD_FLAG_WIELDITEM_VISIBLE) &&
				(m_seats[idx].camera->getCameraMode() == CAMERA_MODE_FIRST));
		bool draw_crosshair = (show_hud &&
				(player->hud_flags & HUD_FLAG_CROSSHAIR_VISIBLE) &&
				(m_seats[idx].camera->getCameraMode() != CAMERA_MODE_THIRD_FRONT));

		this->m_rendering_engine->draw_scene_for(
				m_seats[idx].client,
				m_seats[idx].hud,
				sky_color,
				show_hud,
				draw_wield_tool,
				draw_crosshair);
	};

	// Pick the viewport rectangle for each seat based on the configured
	// number of seats. Shared with menu/formspec routing via
	// Game::getSeatViewport() so that e.g. a per-seat inventory opens
	// inside exactly the rectangle that seat is being rendered into.
	auto get_viewport = [&](u8 idx) -> core::rect<s32> {
		return getSeatViewport(idx);
	};

	// Make sure each seat has an off-screen render target sized to its
	// viewport. Recreate when the viewport size changes (e.g. window resize).
	auto ensure_seat_tex = [&](u8 idx, const core::rect<s32> &vp) -> video::ITexture * {
		const u32 w = (u32)std::max<s32>(1, vp.getWidth());
		const u32 h = (u32)std::max<s32>(1, vp.getHeight());
		auto target_size = core::dimension2du(w, h);
		if (m_seats[idx].render_tex) {
			if (m_seats[idx].render_tex->getSize() != target_size) {
				this->driver->removeTexture(m_seats[idx].render_tex);
				m_seats[idx].render_tex = nullptr;
			}
		}
		if (!m_seats[idx].render_tex) {
			std::string name = "seat" + itos(idx) + "_color";
			m_seats[idx].render_tex = this->driver->addRenderTargetTexture(
					target_size, name.c_str(), video::ECF_A8R8G8B8);
		}
		return m_seats[idx].render_tex;
	};

	// Render one seat into its off-screen texture using the simple
	// (non-pipeline) path: scene -> map post-fx -> wield -> hud.
	//
	// Each seat's Client has its own scene::ISceneManager (created in
	// initializeSeats()). That scene manager contains *only* this seat's
	// world: its ClientMap, its CAOs, its camera helper nodes. So we
	// don't need to touch visibility on anything - we just point the
	// seat's smgr at the seat's camera and call drawAll() on it.
	auto draw_one_to_texture = [&](u8 idx, const core::rect<s32> &vp) {
		if (!m_seats[idx].client || !m_seats[idx].camera || !m_seats[idx].hud)
			return;
		auto *tex = ensure_seat_tex(idx, vp);
		if (!tex)
			return;

		// Bind and clear the per-seat texture.
		this->driver->setRenderTarget(tex,
				/*clearBackBuffer=*/true,
				/*clearZBuffer=*/true,
				sky_color);
		const auto tex_size = tex->getSize();
		this->driver->setViewPort(core::rect<s32>(
				0, 0, (s32)tex_size.Width, (s32)tex_size.Height));

		// Use the seat's camera and pin its aspect to the viewport so the
		// world isn't horizontally squashed when we render to a smaller
		// rectangle.
		auto *seat_smgr = m_seats[idx].client->getSceneManager();
		auto *cam_node = m_seats[idx].camera->getCameraNode();
		seat_smgr->setActiveCamera(cam_node);
		const f32 vp_aspect = (f32)tex_size.Width / (f32)tex_size.Height;

		// Camera::update() set the camera's vertical FOV based on the
		// full window aspect. If we just swap in the (different) viewport
		// aspect here, Irrlicht keeps the vertical FOV and recomputes the
		// horizontal FOV as fov_x = 2*atan(aspect * tan(fov_y/2)). For a
		// 2-up top/bottom split that doubles the viewport width-to-height
		// ratio, so the horizontal FOV ends up around 140deg and every
		// seat looks like it's wearing a fisheye lens.
		//
		// Step 1: recover the intended horizontal FOV from the
		// full-window aspect.
		// Step 2: cap the per-seat horizontal FOV. Even at the player's
		// "normal" 72deg vertical setting, a 16:10 monitor already
		// projects ~108deg horizontally, and that same 108deg in a 1920
		// x501 split-pane reads as a fisheye because the wide-and-short
		// shape exaggerates the perspective stretch at the edges. A
		// ~80deg horizontal feels natural in a split pane.
		// Step 3: derive a new vertical FOV from the (possibly capped)
		// horizontal FOV and the seat's viewport aspect.
		const v2u32 &full_window = RenderingEngine::getWindowSize();
		const f32 win_aspect = full_window.Y > 0 ?
				(f32)full_window.X / (f32)full_window.Y : vp_aspect;
		const f32 fov_y_full = cam_node->getFOV();
		f32 fov_x = 2.0f * std::atan(win_aspect *
				std::tan(0.5f * fov_y_full));

		constexpr f32 SPLITSCREEN_MAX_FOV_X_DEG = 90.0f;
		const f32 max_fov_x = SPLITSCREEN_MAX_FOV_X_DEG *
				(f32)(M_PI / 180.0);
		// Vertical FOV (radians) that yields exactly max_fov_x horizontally
		// at the full-window aspect — the point where we start clamping.
		const f32 fov_y_lim = 2.0f * std::atan(
				std::tan(0.5f * max_fov_x) / win_aspect);
		const bool horiz_clamped = fov_x > max_fov_x;
		if (horiz_clamped)
			fov_x = max_fov_x;

		f32 fov_y_seat = 2.0f * std::atan(
				std::tan(0.5f * fov_x) / vp_aspect);

		// When horiz_clamped, fov_x is pinned to max_fov_x, so any further
		// increase in the camera's vertical FOV (e.g. MineClone2 sprint's
		// ~10% widen) is lost and sprint feels like it does nothing in
		// split-screen. Scale the seat vertical FOV so those changes still
		// apply relative to the clamp threshold.
		if (horiz_clamped && fov_y_lim > 0.0001f)
			fov_y_seat *= fov_y_full / fov_y_lim;
		cam_node->setAspectRatio(vp_aspect);
		cam_node->setFOV(fov_y_seat);
		cam_node->updateMatrices();

		// Main 3D scene - draw THIS seat's scene manager (which contains
		// only this seat's ClientMap + CAOs + camera helper nodes). The
		// usual local-player-mesh culling inside GenericCAO works the
		// same way it does in single-player: there's exactly one local
		// player CAO in this scene, and its material-flag hide is
		// already correct.
		seat_smgr->drawAll();
		this->driver->setTransform(video::ETS_WORLD, core::IdentityMatrix);

		// Underwater / lava overlay etc.
		m_seats[idx].client->getEnv().getClientMap().renderPostFx(
				m_seats[idx].camera->getCameraMode());

		// Wielded item.
		const LocalPlayer *player = m_seats[idx].client->getEnv().getLocalPlayer();
		const bool draw_wield_tool =
				(player->hud_flags & HUD_FLAG_WIELDITEM_VISIBLE) &&
				(m_seats[idx].camera->getCameraMode() == CAMERA_MODE_FIRST);
		if (draw_wield_tool)
			m_seats[idx].camera->drawWieldedTool();

		// Per-seat HUD: temporarily tell the HUD to lay out for this
		// viewport instead of the full window so hearts / hotbar / crosshair
		// land where you expect inside the seat's panel.
		const v2u32 vp_size((u32)tex_size.Width, (u32)tex_size.Height);
		m_seats[idx].hud->setScreensizeOverride(vp_size);
		m_seats[idx].hud->resizeHotbar();

		const bool draw_crosshair =
				(player->hud_flags & HUD_FLAG_CROSSHAIR_VISIBLE) &&
				(m_seats[idx].camera->getCameraMode() != CAMERA_MODE_THIRD_FRONT);

		m_seats[idx].hud->drawBlockBounds();
		m_seats[idx].hud->drawSelectionMesh();
		if (draw_crosshair)
			m_seats[idx].hud->drawCrosshair();
		m_seats[idx].hud->drawLuaElements(m_seats[idx].camera->getOffset());
		m_seats[idx].camera->drawNametags();

		// Drop the override so anything else (debug overlay, GUI environment,
		// etc.) drawn after the per-seat block uses the real window size.
		m_seats[idx].hud->setScreensizeOverride(v2u32(0, 0));
		m_seats[idx].hud->resizeHotbar();
	};

	if (m_splitscreen_seats <= 1) {
		// Single seat: keep the original full pipeline (post-FX, etc.).
		draw_one_fullscreen(0, fullvp, this->m_game_ui->m_flags.show_hud);
	} else {
		// Render every seat into its own off-screen texture.
		for (u8 i = 0; i < m_splitscreen_seats; i++)
			draw_one_to_texture(i, get_viewport(i));

		// Bind back to the real backbuffer for the 2D blit.
		this->driver->setRenderTarget(nullptr, false, false, sky_color);
		this->driver->setViewPort(fullvp);

		// 2D-blit each seat's texture into its viewport region.
		for (u8 i = 0; i < m_splitscreen_seats; i++) {
			if (!m_seats[i].render_tex)
				continue;
			const auto vp = get_viewport(i);
			const auto sz = m_seats[i].render_tex->getSize();
			this->driver->draw2DImage(
					m_seats[i].render_tex,
					vp,
					core::rect<s32>(0, 0, (s32)sz.Width, (s32)sz.Height));
		}

		// Thin separator lines between viewports for readability.
		const video::SColor sep(255, 0, 0, 0);
		const s32 W = (s32)screensize.X;
		const s32 H = (s32)screensize.Y;
		const s32 HW = W / 2;
		const s32 HH = H / 2;
		// Horizontal split at H/2 (always present in the 2/3/4 layouts).
		this->driver->draw2DRectangle(sep, core::rect<s32>(0, HH - 1, W, HH + 1));
		if (m_splitscreen_seats >= 3) {
			// Vertical split across the top half (2-up / 3-up / 4-up).
			const s32 v_top_max = (m_splitscreen_seats == 3) ? HH : H;
			this->driver->draw2DRectangle(sep,
					core::rect<s32>(HW - 1, 0, HW + 1, v_top_max));
		}

		// The single-seat path runs through the rendering pipeline, whose
		// final DrawHUD step calls guienv->drawAll(). The split-screen
		// path bypasses that pipeline entirely, which means menus
		// (pause/exit menu, formspecs), the chat console and any GameUI
		// text (debug overlay, status text, ...) wouldn't be drawn at
		// all - so e.g. pressing Escape would seem to do nothing because
		// the pause menu is created in the GUI env but never rendered.
		// Draw the GUI env once across the full window so menus and HUD
		// text overlay every seat.
		guienv->drawAll();
	}

	this->driver->setViewPort(fullvp);

	/*
		Profiler graph
	*/
	// screensize already computed

	if (this->m_game_ui->m_flags.show_profiler_graph) {
		auto font = g_fontengine->getFont(
			g_fontengine->getDefaultFontSize() * 0.9f, FM_Mono);
		graph->draw(10, screensize.Y - 10, driver, font);
	}

	/*
		Damage flash
	*/
	if (this->runData.damage_flash > 0.0f) {
		video::SColor color(this->runData.damage_flash, 180, 0, 0);
		this->driver->draw2DRectangle(color,
					core::rect<s32>(0, 0, screensize.X, screensize.Y),
					NULL);
	}

	this->driver->endScene();

	stats->drawtime = tt_draw.stop(true);
	g_profiler->graphAdd("Draw scene [us]", stats->drawtime);

}

/****************************************************************************
 Misc
 ****************************************************************************/

void Game::showOverlayMessage(const char *msg, float dtime, int percent, float *indef_pos)
{
	m_rendering_engine->draw_load_screen(wstrgettext(msg), guienv, texture_src,
			dtime, percent, indef_pos);
}

void Game::settingChangedCallback(const std::string &setting_name, void *data)
{
	((Game *)data)->readSettings();
}

void Game::readSettings()
{
	LogLevel chat_log_level = Logger::stringToLevel(g_settings->get("chat_log_level"));
	if (chat_log_level == LL_MAX) {
		warningstream << "Supplied unrecognized chat_log_level; showing none." << std::endl;
		chat_log_level = LL_NONE;
	}
	m_chat_log_buf.setLogLevel(chat_log_level);

	m_cache_doubletap_jump               = g_settings->getBool("doubletap_jump");
	m_cache_toggle_sneak_key             = g_settings->getBool("toggle_sneak_key");
	m_cache_toggle_aux1_key              = g_settings->getBool("toggle_aux1_key");
	m_cache_enable_joysticks             = g_settings->getBool("enable_joysticks");
	m_cache_enable_fog                   = g_settings->getBool("enable_fog");
	m_cache_mouse_sensitivity            = g_settings->getFloat("mouse_sensitivity", 0.001f, 10.0f);
	m_cache_keyboard_camera_speed        = g_settings->getFloat("keyboard_camera_speed", 0.001f, 720.0f);
	m_cache_joystick_frustum_sensitivity = std::max(g_settings->getFloat("joystick_frustum_sensitivity"), 0.001f);
	m_repeat_place_time                  = g_settings->getFloat("repeat_place_time", 0.16f, 2.0f);
	m_repeat_dig_time                    = g_settings->getFloat("repeat_dig_time", 0.0f, 2.0f);

	m_cache_enable_noclip                = g_settings->getBool("noclip");
	m_cache_enable_free_move             = g_settings->getBool("free_move");

	m_cache_cam_smoothing = 0;
	if (g_settings->getBool("cinematic"))
		m_cache_cam_smoothing = g_settings->getFloat("cinematic_camera_smoothing");
	else
		m_cache_cam_smoothing = g_settings->getFloat("camera_smoothing");

	m_cache_cam_smoothing = std::max(0.0f, m_cache_cam_smoothing);
	m_cache_mouse_sensitivity = rangelim(m_cache_mouse_sensitivity, 0.001, 100.0);

	m_invert_mouse = g_settings->getBool("invert_mouse");
	m_enable_hotbar_mouse_wheel = g_settings->getBool("enable_hotbar_mouse_wheel");
	m_invert_hotbar_mouse_wheel = g_settings->getBool("invert_hotbar_mouse_wheel");

	m_does_lost_focus_pause_game = g_settings->getBool("pause_on_lost_focus");
}

/****************************************************************************/
/****************************************************************************
 extern function for launching the game
 ****************************************************************************/
/****************************************************************************/

void the_game(volatile std::sig_atomic_t *kill,
		InputHandler *input,
		RenderingEngine *rendering_engine,
		const GameStartData &start_data,
		std::string &error_message,
		ChatBackend &chat_backend,
		bool *reconnect_requested) // Used for local game
{
	Game game;

	try {

		if (game.startup(kill, input, rendering_engine, start_data,
				error_message, reconnect_requested, &chat_backend)) {
			game.run();
		}

	} catch (SerializationError &e) {
		const std::string ver_err = fmtgettext("The server is probably running a different version of %s.", PROJECT_NAME_C);
		error_message = strgettext("A serialization error occurred:") +"\n"
				+ e.what() + "\n\n" + ver_err;
		errorstream << error_message << std::endl;
	} catch (ServerError &e) {
		error_message = e.what();
		errorstream << "ServerError: " << error_message << std::endl;
	} catch (ModError &e) {
		// DO NOT TRANSLATE the `ModError`, it's used by `ui.lua`
		error_message = std::string("ModError: ") + e.what() +
				strgettext("\nCheck debug.txt for details.");
		errorstream << error_message << std::endl;
	} catch (con::PeerNotFoundException &e) {
		error_message = gettext("Connection error (timed out?)");
		errorstream << error_message << std::endl;
	} catch (ShaderException &e) {
		error_message = e.what();
		errorstream << error_message << std::endl;
	}

	game.shutdown();
}
