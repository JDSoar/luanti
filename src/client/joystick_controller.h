// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2016 est31, <MTest31@outlook.com>

#pragma once

#include <IEventReceiver.h>
#include "irrlichttypes.h"

#include "keys.h"
#include <bitset>
#include <vector>

enum JoystickAxis {
	JA_SIDEWARD_MOVE,
	JA_FORWARD_MOVE,

	JA_FRUSTUM_HORIZONTAL,
	JA_FRUSTUM_VERTICAL,

	// To know the count of enum values
	JA_COUNT,
};

struct JoystickAxisLayout {
	u16 axis_id;
	// -1 if to invert, 1 if to keep it.
	int invert;
};


struct JoystickCombination {

	virtual bool isTriggered(const SEvent::SJoystickEvent &ev) const=0;

	GameKeyType key;
};

struct JoystickButtonCmb : public JoystickCombination {

	JoystickButtonCmb() = default;

	JoystickButtonCmb(GameKeyType key, u32 filter_mask, u32 compare_mask) :
		filter_mask(filter_mask),
		compare_mask(compare_mask)
	{
		this->key = key;
	}

	virtual ~JoystickButtonCmb() = default;

	virtual bool isTriggered(const SEvent::SJoystickEvent &ev) const;

	u32 filter_mask;
	u32 compare_mask;
};

struct JoystickAxisCmb : public JoystickCombination {

	JoystickAxisCmb() = default;

	JoystickAxisCmb(GameKeyType key, u16 axis_to_compare, int direction, s16 thresh) :
		axis_to_compare(axis_to_compare),
		direction(direction),
		thresh(thresh)
	{
		this->key = key;
	}

	virtual ~JoystickAxisCmb() = default;

	bool isTriggered(const SEvent::SJoystickEvent &ev) const override;

	u16 axis_to_compare;

	// if -1, thresh must be smaller than the axis value in order to trigger
	// if  1, thresh must be bigger  than the axis value in order to trigger
	int direction;
	s16 thresh;
};

struct JoystickLayout {
	std::vector<JoystickButtonCmb> button_keys;
	std::vector<JoystickAxisCmb> axis_keys;
	JoystickAxisLayout axes[JA_COUNT];
	s16 axes_deadzone;
	// Bit index of the button that toggles "sprint mode" (latched AUX1 while
	// the player is moving forward). -1 disables the latch. Used to mirror
	// Minecraft Bedrock's L3-tap-to-sprint behaviour without forcing the
	// global `toggle_aux1_key` setting on (which would also toggle the
	// keyboard E key).
	int sprint_toggle_button_bit = -1;
};

class JoystickController {

public:
	JoystickController();

	void onJoystickConnect(const std::vector<SJoystickInfo> &joystick_infos);

	bool handleEvent(const SEvent::SJoystickEvent &ev);
	void clear();

	void releaseAllKeys()
	{
		m_keys_released |= m_keys_down;
		m_keys_down.reset();
		// Intentionally leave m_sprint_latched / m_sprint_button_was_down
		// alone. Game::processUserInput calls this on the first frame *any*
		// menu opens (isMenuActive() is global), which in split-screen
		// includes the OTHER seat's inventory — wiping this seat's sprint
		// here killed sprint immediately after entering split-screen.
		// The latch is purely user-driven: handleEvent auto-cancels it the
		// moment the forward stick returns to neutral, and a second L3 tap
		// toggles it off, which covers every legitimate "stop sprinting"
		// situation. Alt-tabbing with the stick released auto-cancels on
		// the next event; alt-tabbing with the stick held is harmless
		// because no AUX1 is reported until you push forward again anyway.
	}

	bool wasKeyDown(GameKeyType b)
	{
		bool r = m_past_keys_pressed[b];
		m_past_keys_pressed[b] = false;
		return r;
	}

	bool wasKeyReleased(GameKeyType b)
	{
		return m_keys_released[b];
	}
	void clearWasKeyReleased(GameKeyType b)
	{
		m_keys_released[b] = false;
	}

	void clearAllWasKeyReleased()
	{
		m_keys_released.reset();
	}

	bool wasKeyPressed(GameKeyType b)
	{
		return m_keys_pressed[b];
	}
	void clearWasKeyPressed(GameKeyType b)
	{
		m_keys_pressed[b] = false;
	}

	/// Batch-reset (same frame as MyEventReceiver::clearWasKeyPressed): joystick
	/// "was pressed" latches must not persist across frames; otherwise actions
	/// that use `wasKeyPressed` (e.g. camera mode) fire every frame while the
	/// button is held.
	void clearAllWasKeyPressed()
	{
		m_keys_pressed.reset();
	}

	bool isKeyDown(GameKeyType b)
	{
		return m_keys_down[b];
	}

	s16 getAxis(JoystickAxis axis)
	{
		return m_axes_vals[axis];
	}

	float getAxisWithoutDead(JoystickAxis axis);

	float getMovementDirection();
	float getMovementSpeed();

	u8 getJoystickId() const
	{
		return m_joystick_id;
	}

	void setJoystickId(u8 id)
	{
		m_joystick_id = id;
	}

	/** Button/axis map only (used after split-screen GamepadInputHandler construction). */
	void copyLayoutFrom(const JoystickController &other)
	{
		m_layout = other.m_layout;
	}

	f32 doubling_dtime;

private:
	/** Applies m_layout from a device name or `joystick_type` setting string; returns preset id for logging. */
	const char *setLayoutFromControllerName(const std::string &name);

	JoystickLayout m_layout;

	s16 m_axes_vals[JA_COUNT];

	u8 m_joystick_id = 0;

	std::bitset<KeyType::INTERNAL_ENUM_COUNT> m_keys_down;
	std::bitset<KeyType::INTERNAL_ENUM_COUNT> m_keys_pressed;

	f32 m_internal_time;

	f32 m_past_pressed_time[KeyType::INTERNAL_ENUM_COUNT];

	std::bitset<KeyType::INTERNAL_ENUM_COUNT> m_past_keys_pressed;
	std::bitset<KeyType::INTERNAL_ENUM_COUNT> m_keys_released;

	// Bedrock-style L3-tap-to-sprint state (see JoystickLayout::sprint_toggle_button_bit)
	bool m_sprint_latched = false;
	bool m_sprint_button_was_down = false;
};
