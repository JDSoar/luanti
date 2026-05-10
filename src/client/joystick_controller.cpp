// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2016 est31, <MTest31@outlook.com>

#include "joystick_controller.h"
#include "keys.h"
#include "settings.h"
#include "porting.h"
#include "util/string.h"
#include "util/numeric.h"
#include "log.h"

bool JoystickButtonCmb::isTriggered(const SEvent::SJoystickEvent &ev) const
{
	u32 buttons = ev.ButtonStates;

	buttons &= filter_mask;
	return buttons == compare_mask;
}

bool JoystickAxisCmb::isTriggered(const SEvent::SJoystickEvent &ev) const
{
	s16 ax_val = ev.Axis[axis_to_compare];

	return (ax_val * direction < -thresh);
}

// spares many characters
#define JLO_B_PB(A, B, C)    jlo.button_keys.emplace_back(A, B, C)
#define JLO_A_PB(A, B, C, D) jlo.axis_keys.emplace_back(A, B, C, D)

JoystickLayout create_default_layout()
{
	JoystickLayout jlo;

	jlo.axes_deadzone = g_settings->getU16("joystick_deadzone");

	const JoystickAxisLayout axes[JA_COUNT] = {
		{0, 1}, // JA_SIDEWARD_MOVE
		{1, 1}, // JA_FORWARD_MOVE
		{3, 1}, // JA_FRUSTUM_HORIZONTAL
		{4, 1}, // JA_FRUSTUM_VERTICAL
	};
	memcpy(jlo.axes, axes, sizeof(jlo.axes));

	u32 sb = 1 << 7; // START button mask
	u32 fb = 1 << 3; // FOUR button mask
	u32 bm = sb | fb; // Mask for Both Modifiers

	// The back button means "ESC".
	JLO_B_PB(KeyType::ESC,        1 << 6,      1 << 6);

	// The start button counts as modifier as well as use key.
	// JLO_B_PB(KeyType::USE,        sb,          sb));

	// Accessible without start modifier button pressed
	// regardless whether four is pressed or not
	JLO_B_PB(KeyType::SNEAK,      sb | 1 << 2, 1 << 2);

	// Accessible without four modifier button pressed
	// regardless whether start is pressed or not
	JLO_B_PB(KeyType::DIG,        fb | 1 << 4, 1 << 4);
	JLO_B_PB(KeyType::PLACE,      fb | 1 << 5, 1 << 5);

	// Accessible without any modifier pressed
	JLO_B_PB(KeyType::JUMP,       bm | 1 << 0, 1 << 0);
	JLO_B_PB(KeyType::AUX1,       bm | 1 << 1, 1 << 1);
	// Inventory: "four" face button alone (same role as Y on Xbox layouts).
	JLO_B_PB(KeyType::INVENTORY,  bm | fb, fb);

	// Accessible with start button not pressed, but four pressed
	// TODO find usage for button 0
	JLO_B_PB(KeyType::DROP,        bm | 1 << 1, fb | 1 << 1);
	JLO_B_PB(KeyType::HOTBAR_PREV, bm | 1 << 4, fb | 1 << 4);
	JLO_B_PB(KeyType::HOTBAR_NEXT, bm | 1 << 5, fb | 1 << 5);

	// Accessible with start button and four pressed
	// TODO find usage for buttons 0, 1 and 4, 5

	// Now about the buttons simulated by the axes

	// Movement buttons, important for vessels
	JLO_A_PB(KeyType::FORWARD,  1,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::BACKWARD, 1, -1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::LEFT,     0,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::RIGHT,    0, -1, jlo.axes_deadzone);

	// Scroll buttons
	JLO_A_PB(KeyType::HOTBAR_PREV, 2, -1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::HOTBAR_NEXT, 5, -1, jlo.axes_deadzone);

	return jlo;
}

JoystickLayout create_xbox_layout(bool swap_analog_sticks)
{
	JoystickLayout jlo;

	jlo.axes_deadzone = 7000;

	// Default (swap_analog_sticks == false): Linux xpad / common SDL raw mapping:
	//   0 = Left  stick X      3 = Right stick X
	//   1 = Left  stick Y      4 = Right stick Y
	//   2 = Left  trigger      5 = Right trigger
	//
	// Some Bluetooth / hid-microsoft stacks report the two analog sticks in the
	// opposite order (movement on 3,4, look on 0,1). That makes the physical
	// *left* stick control the camera and the *right* stick walk — use
	// `joystick_type = xbox_swapped` or pass swap_analog_sticks = true.
	u16 move_x, move_y, look_x, look_y;
	if (!swap_analog_sticks) {
		move_x = 0;
		move_y = 1;
		look_x = 3;
		look_y = 4;
	} else {
		move_x = 3;
		move_y = 4;
		look_x = 0;
		look_y = 1;
	}

	const JoystickAxisLayout axes[JA_COUNT] = {
		{move_x, 1}, // JA_SIDEWARD_MOVE
		{move_y, 1}, // JA_FORWARD_MOVE
		{look_x, 1}, // JA_FRUSTUM_HORIZONTAL
		{look_y, 1}, // JA_FRUSTUM_VERTICAL
	};
	memcpy(jlo.axes, axes, sizeof(jlo.axes));

	// Face buttons - Minecraft-style (Bedrock controller layout):
	//   A = Jump       B = Sneak (hold)
	//   X = Drop item  Y = Open inventory
	JLO_B_PB(KeyType::JUMP,        1 << 0,  1 << 0); // A
	JLO_B_PB(KeyType::SNEAK,       1 << 1,  1 << 1); // B
	JLO_B_PB(KeyType::DROP,        1 << 2,  1 << 2); // X
	JLO_B_PB(KeyType::INVENTORY,   1 << 3,  1 << 3); // Y

	// Bumpers cycle the hotbar (matches Minecraft's LB/RB).
	JLO_B_PB(KeyType::HOTBAR_PREV, 1 << 4,  1 << 4); // LB
	JLO_B_PB(KeyType::HOTBAR_NEXT, 1 << 5,  1 << 5); // RB

	// Back / Start — both open the game menu, matching Minecraft Bedrock
	// (View and Menu both pause / show the game menu).
	JLO_B_PB(KeyType::ESC,         1 << 6,  1 << 6); // Back / View  → game menu
	JLO_B_PB(KeyType::ESC,         1 << 7,  1 << 7); // Start / Menu → game menu

	// Stick clicks — SDL's raw Xbox / X-input ordering uses **0-based** indices:
	//   8 = left stick (L3), 9 = right stick (R3).  (Bits are 1<<8, 1<<9.)
	// We previously used 9 and 10 here, which map to **R3** and a nonexistent
	// button on standard Xbox pads — sprint (AUX1) felt broken.
	// Linux xpad (Xbox 360 / Xbox One wired) button ordering — verified by
	// dumping `ButtonStates` from a real "Xbox 360 Controller" pad, which
	// reports 11 buttons (bits 0..10):
	//   bit 8  = Guide / Xbox button   (intentionally unmapped)
	//   bit 9  = L3   (left stick click)
	//   bit 10 = R3   (right stick click)
	// This is NOT the Windows XInput ordering (where L3=8, R3=9). SDL on
	// Linux exposes the raw joydev/xpad numbering, not the normalized
	// SDL_GameController layout, so we must use the kernel ordering here.
	JLO_B_PB(KeyType::AUX1,        1 << 9,  1 << 9);  // L3 → sprint (held)
	JLO_B_PB(KeyType::CAMERA_MODE, 1 << 10, 1 << 10); // R3 → camera mode

	// Bedrock-style sprint: a single L3 tap latches AUX1 on while the
	// player is moving forward, releasing automatically when the stick
	// returns to neutral. A second L3 tap cancels the latch. Holding L3
	// keeps AUX1 on continuously through the binding above.
	jlo.sprint_toggle_button_bit = 9;

	// D-pad — Minecraft Bedrock-flavoured mapping:
	//   Up    = toggle minimap   (MC: emote menu — closest analogue in Luanti)
	//   Down  = open chat        (MC: chat)
	//   Left  = hotbar previous  (MC extended / Java mods)
	//   Right = hotbar next      (MC extended / Java mods)
	// Optional fallback: some pads expose the D-pad as buttons instead of a hat.
	// Primary path is SDL hat → SJoystickEvent.POV in JoystickController::handleEvent.
	JLO_B_PB(KeyType::MINIMAP,     1 << 11, 1 << 11); // D-pad up
	JLO_B_PB(KeyType::CHAT,        1 << 12, 1 << 12); // D-pad down
	JLO_B_PB(KeyType::HOTBAR_PREV, 1 << 13, 1 << 13); // D-pad left
	JLO_B_PB(KeyType::HOTBAR_NEXT, 1 << 14, 1 << 14); // D-pad right

	// Triggers are analog axes on Linux (-32768 released → +32767 pressed),
	// not button bits as on Windows XInput. Treat them as digital "pressed"
	// once they cross the deadzone in the positive direction.
	// Minecraft mapping: LT = use/place, RT = attack/break.
	// (Trigger axis indices are unchanged when swapping look/move sticks.)
	JLO_A_PB(KeyType::PLACE, 2, -1, jlo.axes_deadzone); // LT → Place / Use
	JLO_A_PB(KeyType::DIG,   5, -1, jlo.axes_deadzone); // RT → Attack / Mine

	// Movement stick → direction keys (axis indices follow move_* pair).
	JLO_A_PB(KeyType::FORWARD,  move_y,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::BACKWARD, move_y, -1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::LEFT,     move_x,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::RIGHT,    move_x, -1, jlo.axes_deadzone);

	return jlo;
}

JoystickLayout create_ps5_layout()
{
	JoystickLayout jlo;
	jlo.axes_deadzone = 7000;

	// Analog sticks
	const JoystickAxisLayout axes[JA_COUNT] = {
		{0, 1},  // JA_SIDEWARD_MOVE (left stick X)
		{1, 1},  // JA_FORWARD_MOVE (left stick Y)
		{2, 1},  // JA_FRUSTUM_HORIZONTAL (right stick X - look)
		{3, 1},  // JA_FRUSTUM_VERTICAL (right stick Y - look)
	};
	memcpy(jlo.axes, axes, sizeof(jlo.axes));

	// Options button
	JLO_B_PB(KeyType::ESC,         1 << 6,  1 << 6);  // Options - Pause menu

	// Face buttons
	JLO_B_PB(KeyType::JUMP,        1 << 0,  1 << 0);  // Cross
	JLO_B_PB(KeyType::SNEAK,       1 << 1,  1 << 1);  // Circle
	JLO_B_PB(KeyType::CAMERA_MODE, 1 << 2,  1 << 2);  // Square
	JLO_B_PB(KeyType::DROP,        1 << 3,  1 << 3);  // Triangle

	// Touchpad
	JLO_B_PB(KeyType::INVENTORY,   1 << 15, 1 << 15); // Touchpad

	// Stick clicks L3/R3
	JLO_B_PB(KeyType::AUX1,        1 << 7,  1 << 7);  // L3
	JLO_B_PB(KeyType::ZOOM,        1 << 8,  1 << 8);  // R3

	// Bumpers L1/R1
	JLO_B_PB(KeyType::HOTBAR_PREV, 1 << 9,  1 << 9);  // L1
	JLO_B_PB(KeyType::HOTBAR_NEXT, 1 << 10, 1 << 10); // R1

	// Triggers L2/R2 (analog axes used as buttons)
	JLO_A_PB(KeyType::DIG,   4, -1, jlo.axes_deadzone); // L2
	JLO_A_PB(KeyType::PLACE, 5, -1, jlo.axes_deadzone); // R2

	// D-pad
	JLO_B_PB(KeyType::FREEMOVE,    1 << 11, 1 << 11); // D-pad up
	JLO_B_PB(KeyType::AUTOFORWARD, 1 << 12, 1 << 12); // D-pad down
	JLO_B_PB(KeyType::MINIMAP,     1 << 13, 1 << 13); // D-pad left
	JLO_B_PB(KeyType::FASTMOVE,    1 << 14, 1 << 14); // D-pad right

	return jlo;
}

JoystickLayout create_dragonrise_gamecube_layout()
{
	JoystickLayout jlo;

	jlo.axes_deadzone = 7000;

	const JoystickAxisLayout axes[JA_COUNT] = {
		// Control Stick
		{0, 1}, // JA_SIDEWARD_MOVE
		{1, 1}, // JA_FORWARD_MOVE

		// C-Stick
		{3, 1}, // JA_FRUSTUM_HORIZONTAL
		{4, 1}, // JA_FRUSTUM_VERTICAL
	};
	memcpy(jlo.axes, axes, sizeof(jlo.axes));

	// The center button
	JLO_B_PB(KeyType::ESC, 1 << 9, 1 << 9); // Start/Pause Button

	// Front right buttons
	JLO_B_PB(KeyType::JUMP,  1 << 2, 1 << 2); // A Button
	JLO_B_PB(KeyType::SNEAK, 1 << 3, 1 << 3); // B Button
	JLO_B_PB(KeyType::DROP,  1 << 0, 1 << 0); // Y Button
	JLO_B_PB(KeyType::AUX1,  1 << 1, 1 << 1); // X Button

	// Triggers
	JLO_B_PB(KeyType::DIG,       1 << 4, 1 << 4); // L Trigger
	JLO_B_PB(KeyType::PLACE,     1 << 5, 1 << 5); // R Trigger
	JLO_B_PB(KeyType::INVENTORY, 1 << 6, 1 << 6); // Z Button

	// D-Pad
	JLO_A_PB(KeyType::HOTBAR_PREV, 5,  1, jlo.axes_deadzone); // left
	JLO_A_PB(KeyType::HOTBAR_NEXT, 5, -1, jlo.axes_deadzone); // right
	// Axis are hard to actuate independently, best to leave up and down unused.
	//JLO_A_PB(0, 6,  1, jlo.axes_deadzone); // up
	//JLO_A_PB(0, 6, -1, jlo.axes_deadzone); // down

	// Movements tied to Control Stick, important for vessels
	JLO_A_PB(KeyType::LEFT,     0,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::RIGHT,    0, -1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::FORWARD,  1,  1, jlo.axes_deadzone);
	JLO_A_PB(KeyType::BACKWARD, 1, -1, jlo.axes_deadzone);

	return jlo;
}


JoystickController::JoystickController()
{
	doubling_dtime = std::max(g_settings->getFloat("repeat_joystick_button_time"), 0.001f);
	for (float &i : m_past_pressed_time) {
		i = 0;
	}
	m_layout.axes_deadzone = 0;
	clear();
}

void JoystickController::onJoystickConnect(const std::vector<SJoystickInfo> &joystick_infos)
{
	s32         id     = g_settings->getS32("joystick_id");
	std::string layout = g_settings->get("joystick_type");

	if (id < 0 || id >= (s32)joystick_infos.size()) {
		// TODO: auto detection
		id = 0;
	}

	const char *preset_used = "generic";
	if (id >= 0 && id < (s32)joystick_infos.size()) {
		if (layout.empty() || layout == "auto")
			preset_used = setLayoutFromControllerName(joystick_infos[id].Name.c_str());
		else
			preset_used = setLayoutFromControllerName(layout);

		infostream << "Joystick [" << id << "] name=\"" << joystick_infos[id].Name.c_str()
				<< "\" preset=\"" << preset_used << "\". "
				"If buttons are wrong, set joystick_type to xbox, xbox_swapped, "
				"ps5, or generic (requires restart)." << std::endl;
	}

	// Irrlicht restriction.
	m_joystick_id = rangelim(id, 0, UINT8_MAX);
}

const char *JoystickController::setLayoutFromControllerName(const std::string &name)
{
	const std::string n = lowercase(name);

	// Settings menu presets (builtin/settingtypes.txt `joystick_type` enum), and
	// substring matches for wired-only or modpack overrides.
	if (n == "xbox") {
		m_layout = create_xbox_layout(false);
		return "xbox";
	} else if (n == "xbox_swapped") {
		// Left/right stick axes reversed in SDL vs xpad (Bluetooth / some drivers)
		m_layout = create_xbox_layout(true);
		return "xbox_swapped";
	} else if (n == "ps5") {
		m_layout = create_ps5_layout();
		return "ps5";
	} else if (n == "generic") {
		m_layout = create_default_layout();
		return "generic";
	} else if (n.find("dragonrise_gamecube") != std::string::npos) {
		m_layout = create_dragonrise_gamecube_layout();
		return "dragonrise_gamecube";
	} else if (n.find("ps5") != std::string::npos ||
			n.find("dualsense") != std::string::npos) {
		m_layout = create_ps5_layout();
		return "ps5";
	// Linux: SDL/udev sometimes expose only USB vendor ids or generic text on
	// Ubuntu while Fedora shows a full product name — widen matching so "auto"
	// still picks the Bedrock-style Xbox/PS maps instead of `generic`.
	} else if (n.find("dualshock") != std::string::npos ||
			n.find("sixaxis") != std::string::npos ||
			n.find("playstation") != std::string::npos ||
			n.find("sony") != std::string::npos ||
			n.find("054c") != std::string::npos) { // Sony USB vendor id
		m_layout = create_ps5_layout();
		return "ps5";
	} else if (n.find("045e") != std::string::npos ||
			n.find("microsoft") != std::string::npos) { // Microsoft USB vendor id
		m_layout = create_xbox_layout(false);
		return "xbox";
	} else if (n.find("8bitdo") != std::string::npos) {
		// Most 8BitDo modes on Linux follow Xbox-like SDL button numbering.
		m_layout = create_xbox_layout(false);
		return "xbox";
	} else if (n.find("xbox") != std::string::npos ||
			// Linux evdev: "Microsoft X-Box 360 pad" — hyphen breaks plain `xbox`
			n.find("x-box") != std::string::npos ||
			n.find("xinput") != std::string::npos) {
		m_layout = create_xbox_layout(false);
		return "xbox";
	}
	m_layout = create_default_layout();
	return "generic";
}

bool JoystickController::handleEvent(const SEvent::SJoystickEvent &ev)
{
	if (ev.Joystick != m_joystick_id)
		return false;

	m_internal_time = porting::getTimeMs() / 1000.f;

	// Diagnostic: log every button-bit change so users can identify which
	// hardware bit corresponds to L3 / R3 / etc. on their specific pad.
	// Enable by setting `debug_joystick_buttons = true` in minetest.conf.
	// Uses warningstream so it always lands in debug.txt regardless of the
	// configured log level. The setting is re-read on every event (cheap)
	// to avoid first-call caching pitfalls. POV (d-pad hat) changes are
	// logged too so we can verify hat reporting on this pad.
	{
		static thread_local u32 last_button_states = 0;
		static thread_local u16 last_pov = 65535;
		const bool debug_enabled = g_settings->getBool("debug_joystick_buttons");
		if (debug_enabled) {
			if (ev.ButtonStates != last_button_states) {
				u32 changed = ev.ButtonStates ^ last_button_states;
				std::string desc;
				for (int i = 0; i < 32; i++) {
					if (changed & (1u << i)) {
						desc += " bit";
						desc += std::to_string(i);
						desc += (ev.ButtonStates & (1u << i)) ? "_DOWN" : "_UP";
					}
				}
				warningstream << "[joystick " << (int)ev.Joystick
						<< "] buttons=0x" << std::hex << ev.ButtonStates
						<< std::dec << " changed:" << desc << std::endl;
				last_button_states = ev.ButtonStates;
			}
			if (ev.POV != last_pov) {
				warningstream << "[joystick " << (int)ev.Joystick
						<< "] POV=" << ev.POV
						<< (ev.POV == 65535 ? " (centered)" : "")
						<< std::endl;
				last_pov = ev.POV;
			}
		}
	}

	std::bitset<KeyType::INTERNAL_ENUM_COUNT> keys_pressed;

	// First generate a list of keys pressed

	for (const auto &button_key : m_layout.button_keys) {
		if (button_key.isTriggered(ev)) {
			keys_pressed.set(button_key.key);
		}
	}

	for (const auto &axis_key : m_layout.axis_keys) {
		if (axis_key.isTriggered(ev)) {
			keys_pressed.set(axis_key.key);
		}
	}

	// SDL reports the Xbox D-pad as a **hat** (see CIrrDeviceSDL.cpp), which
	// Irrlicht exposes as SJoystickEvent.POV in hundredths of a degree — not
	// as ButtonStates bits. Without this, d-pad does nothing on Linux.
	//
	// Mapping mirrors Minecraft Bedrock's controller scheme:
	//   Up    = toggle minimap (MC's emote/menu slot)
	//   Down  = open chat
	//   Left  = hotbar previous
	//   Right = hotbar next
	// The formspec inventory menu re-reads these same keys for D-pad cursor
	// navigation — see GUIFormSpecMenu::handleJoystickInput.
	static constexpr u16 POV_CENTERED = 65535;
	if (ev.POV != POV_CENTERED) {
		auto press = [&](GameKeyType k) {
			keys_pressed.set(k);
		};
		switch (ev.POV) {
		case 0: // Up
			press(KeyType::MINIMAP);
			break;
		case 4500: // Up-Right
			press(KeyType::MINIMAP);
			press(KeyType::HOTBAR_NEXT);
			break;
		case 9000: // Right
			press(KeyType::HOTBAR_NEXT);
			break;
		case 13500: // Down-Right
			press(KeyType::CHAT);
			press(KeyType::HOTBAR_NEXT);
			break;
		case 18000: // Down
			press(KeyType::CHAT);
			break;
		case 22500: // Down-Left
			press(KeyType::CHAT);
			press(KeyType::HOTBAR_PREV);
			break;
		case 27000: // Left
			press(KeyType::HOTBAR_PREV);
			break;
		case 31500: // Up-Left
			press(KeyType::MINIMAP);
			press(KeyType::HOTBAR_PREV);
			break;
		default:
			break;
		}
	}

	// Bedrock-style L3-tap-to-sprint latch.
	//
	// Holding L3 already presses AUX1 directly through the button binding,
	// but you can't physically keep L3 held while pushing the left stick
	// forward. So we add a latch: a single tap of the configured button
	// keeps AUX1 pressed as long as the player is also moving forward.
	// Releasing the stick clears the latch (sprint stops); a second tap
	// while the latch is on cancels it immediately.
	if (m_layout.sprint_toggle_button_bit >= 0) {
		const u32 mask = 1u << m_layout.sprint_toggle_button_bit;
		const bool sprint_btn_down = (ev.ButtonStates & mask) != 0;
		const bool sprint_btn_edge = sprint_btn_down && !m_sprint_button_was_down;
		m_sprint_button_was_down = sprint_btn_down;

		if (sprint_btn_edge)
			m_sprint_latched = !m_sprint_latched;

		// Auto-cancel when the player isn't pushing forward. The
		// FORWARD bit was just set above by the axis_keys loop, so it
		// reflects the current stick state (SDL events carry all axis
		// values, not just the one that changed).
		if (!keys_pressed[KeyType::FORWARD])
			m_sprint_latched = false;

		if (m_sprint_latched)
			keys_pressed.set(KeyType::AUX1);
	}

	// Then update the values

	for (size_t i = 0; i < KeyType::INTERNAL_ENUM_COUNT; i++) {
		if (keys_pressed[i]) {
			if (!m_past_keys_pressed[i] &&
					m_past_pressed_time[i] < m_internal_time - doubling_dtime) {
				m_past_keys_pressed[i] = true;
				m_past_pressed_time[i] = m_internal_time;
			}
		} else if (m_keys_down[i]) {
			m_keys_released[i] = true;
		}

		if (keys_pressed[i] && !(m_keys_down[i]))
			m_keys_pressed[i] = true;

		m_keys_down[i] = keys_pressed[i];
	}

	for (size_t i = 0; i < JA_COUNT; i++) {
		const JoystickAxisLayout &ax_la = m_layout.axes[i];
		m_axes_vals[i] = ax_la.invert * ev.Axis[ax_la.axis_id];
	}

	return true;
}

void JoystickController::clear()
{
	m_keys_pressed.reset();
	m_past_keys_pressed.reset();
	m_keys_released.reset();
	// Intentionally do NOT touch m_sprint_latched / m_sprint_button_was_down
	// here. clear() runs every frame while *any* menu is active anywhere
	// (Game::processUserInput calls input->clear() on isMenuActive()), so in
	// split-screen another seat opening their inventory would otherwise wipe
	// this seat's sprint state. The latch already self-cancels in
	// handleEvent (release the forward stick or tap L3 again) and in
	// releaseAllKeys() (window focus loss), which covers every case where
	// stopping sprint is actually wanted.

	// Intentionally do NOT zero m_axes_vals or m_keys_down here. clear()
	// is called every frame while a menu is open (Game::processUserInput),
	// but SDL only emits joystick events when state *changes* — there is
	// no continuous "still held" event. Wiping these would cause:
	//   * axes: holding the right stick steady to drag the inventory
	//     cursor would read 0 every subsequent frame (no fresh event).
	//   * keys: holding the D-pad in one direction (which arrives as a
	//     POV/hat event, not a per-frame button bit) would only register
	//     for the single frame the event landed, so the inventory cursor
	//     wouldn't move. Same applies to held face buttons in menus.
	// Both are self-correcting: SDL emits a release event (axis=0 / POV
	// centered / button up) when the user lets go, so leaving the last
	// reported value here is safe.
	//
	// The "was pressed / released" latches above are still wiped so that
	// one-shot menu actions (e.g. Y to close) don't fire repeatedly.
}

float JoystickController::getAxisWithoutDead(JoystickAxis axis)
{
	s16 v = m_axes_vals[axis];

	if (abs(v) < m_layout.axes_deadzone)
		return 0.0f;

	v += (v < 0 ? m_layout.axes_deadzone : -m_layout.axes_deadzone);

	return (float)v / ((float)(INT16_MAX - m_layout.axes_deadzone));
}

float JoystickController::getMovementDirection()
{
	return std::atan2(getAxisWithoutDead(JA_SIDEWARD_MOVE),
			-getAxisWithoutDead(JA_FORWARD_MOVE));
}

float JoystickController::getMovementSpeed()
{
	float speed = std::sqrt(std::pow(getAxisWithoutDead(JA_FORWARD_MOVE), 2) +
			std::pow(getAxisWithoutDead(JA_SIDEWARD_MOVE), 2));
	if (speed > 1.0f)
		speed = 1.0f;
	return speed;
}
