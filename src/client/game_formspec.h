// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2024 cx384

#pragma once

#include <array>
#include <memory>
#include <string>
#include "irr_v3d.h"
#include "rect.h"
#include "scripting_pause_menu.h"

class Client;
class RenderingEngine;
class InputHandler;
class GUIFormSpecMenu;
class JoystickController;

/// Number of split-screen seats supported. Keep in sync with
/// `Game::m_seats`'s array size in `game_internal.h`.
constexpr u8 GAMEFORMSPEC_MAX_SEATS = 4;

/*
This object intend to contain the core fromspec functionality.
It includes:
  - methods to show specific formspec menus
  - storing the opened fromspec
  - handling fromspec related callbacks
 */
struct GameFormSpec
{
	void init(Client *client, RenderingEngine *rendering_engine, InputHandler *input);

	~GameFormSpec() { reset(); }

	void showFormSpec(const std::string &formspec, const std::string &formname);
	/// Split-screen variant: routes the formspec into `seat_idx`'s slot so
	/// it can coexist with formspecs already open for other seats.
	void showFormSpecForSeat(u8 seat_idx, Client *seat_client,
		JoystickController *seat_joystick,
		const core::rect<s32> &seat_viewport,
		const std::string &formspec, const std::string &formname);
	void showCSMFormSpec(const std::string &formspec, const std::string &formname);
	/// Split-screen: CSM `show_formspec` must use the originating seat's slot,
	/// script instance, joystick, and viewport — otherwise it would always
	/// open on seat 0 with a fullscreen layout and cover every player's panel.
	void showCSMFormSpecForSeat(u8 seat_idx, Client *seat_client,
		JoystickController *seat_joystick,
		const core::rect<s32> &seat_viewport,
		const std::string &formspec, const std::string &formname);
	// Used by the Lua pause menu environment to show formspecs.
	// Currently only used for the in-game settings menu.
	void showPauseMenuFormSpec(const std::string &formspec, const std::string &formname);
	void showNodeFormspec(const std::string &formspec, const v3s16 &nodepos);
	/// If `!fs_override`: Uses `player->inventory_formspec`.
	/// If ` fs_override`: Uses a temporary formspec until an update is received.
	///
	/// Split-screen: pass `seat_client` / `seat_joystick` to show the inventory
	/// for an additional seat's player instead of the primary one. When both
	/// are null the primary `m_client` / `m_input` is used (legacy behavior).
	/// `seat_viewport` (when non-empty) restricts the rendered formspec to a
	/// sub-rectangle of the screen so it only appears in that seat's panel.
	/// `seat_idx` selects which per-seat formspec slot to use; allows several
	/// players to have inventories open simultaneously.
	void showPlayerInventory(const std::string *fs_override,
		u8 seat_idx = 0,
		Client *seat_client = nullptr,
		JoystickController *seat_joystick = nullptr,
		const core::rect<s32> &seat_viewport = core::rect<s32>(0, 0, 0, 0));
	/// Network packet `TOCLIENT_DEATHSCREEN_LEGACY`. In split-screen, `seat_*`
	/// must match the client whose queue received the event so the overlay is
	/// confined to that seat (see `Game::handleClientEvent_DeathscreenLegacy`).
	void showDeathFormspecLegacy(u8 seat_idx, Client *seat_client,
		JoystickController *seat_joystick,
		const core::rect<s32> &seat_viewport);
	// Shows the hardcoded "main" pause menu.
	void showPauseMenu();

	/// Returns true if the formspec slot for `seat_idx` currently has a
	/// menu open (its GUIFormSpecMenu is alive and parented to guiroot).
	/// Used by per-seat key handlers so opening one seat's inventory does
	/// not get blocked by another seat already having an inventory open.
	bool isSeatMenuActive(u8 seat_idx) const;

	void update();
	void disableDebugView();

	bool handleCallbacks();
	void reset();

#ifdef __ANDROID__
	// Returns false if no formspec open
	bool handleAndroidUIInput();
#endif

private:
	Client *m_client;
	RenderingEngine *m_rendering_engine;
	InputHandler *m_input;
	std::unique_ptr<PauseMenuScripting> m_pause_script;

	/// Per-seat formspec slots. Slot 0 is the legacy / single-player /
	/// seat-0 slot - all non-split-screen code paths read and write
	/// `m_seat_formspec[0]`. Slots 1..N hold the inventory / node /
	/// custom formspec each additional split-screen seat has open.
	/// Multiple slots can be non-null simultaneously; each menu is
	/// constrained to its seat's viewport (see GUIModalMenu::setViewport)
	/// and routed gamepad input via its own JoystickController so they
	/// don't fight for focus.
	/// FIXME: Layering is already managed by `GUIModalMenu` (`g_menumgr`),
	/// hence these slots should be unified with the menu manager long-term.
	std::array<GUIFormSpecMenu *, GAMEFORMSPEC_MAX_SEATS> m_seat_formspec{};

	bool handleEmptyFormspec(u8 seat_idx, const std::string &formspec,
			const std::string &formname);

	void deleteFormspec(u8 seat_idx);
};
