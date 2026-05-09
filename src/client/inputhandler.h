// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2010-2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

#include "irrlichttypes.h"
#include "irr_v2d.h"
#include "joystick_controller.h"
#include <array>
#include <algorithm>
#include <set>
#include <unordered_map>
#include "keycode.h"
#include "settings.h"
#include "util/string.h"

class InputHandler;

enum class PointerType {
	Mouse,
	Touch,
};

class MyEventReceiver : public IEventReceiver
{
public:
	// This is the one method that we have to implement
	virtual bool OnEvent(const SEvent &event);

	bool IsKeyDown(GameKeyType key) const { return keyIsDown[key]; }

	// Checks whether a key was down and resets the state
	bool WasKeyDown(GameKeyType key)
	{
		bool b = keyWasDown[key];
		if (b)
			keyWasDown.reset(key);
		return b;
	}

	// Checks whether a key was just pressed. State will be cleared
	// in the subsequent iteration of Game::processPlayerInteraction
	bool WasKeyPressed(GameKeyType key) const { return keyWasPressed[key]; }

	// Checks whether a key was just released. State will be cleared
	// in the subsequent iteration of Game::processPlayerInteraction
	bool WasKeyReleased(GameKeyType key) const { return keyWasReleased[key]; }

	void reloadKeybindings();

	s32 getMouseWheel()
	{
		s32 a = mouse_wheel;
		mouse_wheel = 0;
		return a;
	}

	void clearInput()
	{
		physicalKeyDown.clear();
		keyIsDown.reset();
		keyWasDown.reset();
		keyWasPressed.reset();
		keyWasReleased.reset();

		mouse_wheel = 0;
	}

	void releaseAllKeys()
	{
		physicalKeyDown.clear();
		keyWasReleased |= keyIsDown;
		keyIsDown.reset();
	}

	void clearWasKeyPressed()
	{
		keyWasPressed.reset();
	}

	void clearWasKeyReleased()
	{
		keyWasReleased.reset();
	}

	// Legacy single-joystick pointer (still supported).
	JoystickController *joystick = nullptr;
	// Optional multi-joystick dispatch (for split-screen).
	void registerJoystick(JoystickController *j) { if (j) m_joysticks.push_back(j); }
	void unregisterJoystick(JoystickController *j)
	{
		m_joysticks.erase(std::remove(m_joysticks.begin(), m_joysticks.end(), j),
				m_joysticks.end());
	}

	PointerType getLastPointerType() { return last_pointer_type; }

private:
	void listenForKey(KeyPress keyCode, GameKeyType action)
	{
		if (keyCode)
			keysListenedFor[keyCode] = action;
	}

	bool setKeyDown(KeyPress keyCode, bool is_down);
	void setKeyDown(GameKeyType action, bool is_down);
	bool checkKeyDown(GameKeyType action) const;

	/* This is faster than using getKeySetting with the tradeoff that functions
	 * using it must make sure that it's initialised before using it and there is
	 * no error handling (for example bounds checking). This is useful here as the
	 * faster (up to 10x faster) key lookup is an asset.
	 */
	std::array<std::vector<KeyPress>, KeyType::INTERNAL_ENUM_COUNT> keybindings;

	s32 mouse_wheel = 0;

	// The current state of physical keys.
	std::set<KeyPress> physicalKeyDown;

	// The current state of keys
	std::bitset<GameKeyType::INTERNAL_ENUM_COUNT> keyIsDown;

	// Like keyIsDown but only reset when that key is read
	std::bitset<GameKeyType::INTERNAL_ENUM_COUNT> keyWasDown;

	// Whether a key has just been pressed
	std::bitset<GameKeyType::INTERNAL_ENUM_COUNT> keyWasPressed;

	// Whether a key has just been released
	std::bitset<GameKeyType::INTERNAL_ENUM_COUNT> keyWasReleased;

	// List of keys we listen for
	std::unordered_map<KeyPress, GameKeyType> keysListenedFor;

	// Intentionally not reset by clearInput/releaseAllKeys.
	bool fullscreen_is_down = false;

	bool close_world_down = false;
	bool esc_down = false;

	PointerType last_pointer_type = PointerType::Mouse;

	std::vector<JoystickController *> m_joysticks;
};

class InputHandler
{
public:
	InputHandler()
	{
		for (const auto &name: Settings::getLayer(SL_DEFAULTS)->getNames())
			if (str_starts_with(name, "keymap_"))
				g_settings->registerChangedCallback(name, &settingChangedCallback, this);
	}

	virtual ~InputHandler() = default;

	virtual bool isRandom() const
	{
		return false;
	}

	virtual bool isKeyDown(GameKeyType k) = 0;
	virtual bool wasKeyDown(GameKeyType k) = 0;
	virtual bool wasKeyPressed(GameKeyType k) = 0;
	virtual bool wasKeyReleased(GameKeyType k) = 0;
	virtual bool cancelPressed() = 0;

	virtual float getJoystickSpeed() = 0;
	virtual float getJoystickDirection() = 0;

	virtual void clearWasKeyPressed() {}
	virtual void clearWasKeyReleased() {}

	virtual void reloadKeybindings() {}

	virtual v2s32 getMousePos() = 0;
	virtual void setMousePos(s32 x, s32 y) = 0;

	virtual s32 getMouseWheel() = 0;

	virtual void step(float dtime) {}

	virtual void clear() {}
	virtual void releaseAllKeys() {}

	static void settingChangedCallback(const std::string &name, void *data)
	{
		static_cast<InputHandler *>(data)->reloadKeybindings();
	}

	JoystickController joystick;
};

/*
	Separated input handler implementations
*/

class RealInputHandler final : public InputHandler
{
public:
	RealInputHandler(MyEventReceiver *receiver) : m_receiver(receiver)
	{
		m_receiver->joystick = &joystick;
		m_receiver->registerJoystick(&joystick);
		m_receiver->reloadKeybindings();
	}

	virtual ~RealInputHandler()
	{
		m_receiver->unregisterJoystick(&joystick);
		m_receiver->joystick = nullptr;
	}

	virtual bool isKeyDown(GameKeyType k)
	{
		return m_receiver->IsKeyDown(k) || joystick.isKeyDown(k);
	}
	virtual bool wasKeyDown(GameKeyType k)
	{
		return m_receiver->WasKeyDown(k) || joystick.wasKeyDown(k);
	}
	virtual bool wasKeyPressed(GameKeyType k)
	{
		return m_receiver->WasKeyPressed(k) || joystick.wasKeyPressed(k);
	}
	virtual bool wasKeyReleased(GameKeyType k)
	{
		return m_receiver->WasKeyReleased(k) || joystick.wasKeyReleased(k);
	}

	virtual float getJoystickSpeed();

	virtual float getJoystickDirection();

	virtual bool cancelPressed()
	{
		return wasKeyDown(KeyType::ESC);
	}

	virtual void clearWasKeyPressed()
	{
		m_receiver->clearWasKeyPressed();
		// Match keyboard: batch-clear at end of processPlayerInteraction.
		// Without this, JoystickController::m_keys_pressed stays latched until
		// InputHandler::clear() (e.g. menus only), so `wasKeyPressed` is true
		// on every frame while a face button is held — e.g. R3 toggles camera
		// mode continuously and appears to "flash" between first/third person.
		joystick.clearAllWasKeyPressed();
	}
	virtual void clearWasKeyReleased()
	{
		m_receiver->clearWasKeyReleased();
		joystick.clearAllWasKeyReleased();
	}

	virtual void reloadKeybindings()
	{
		m_receiver->reloadKeybindings();
	}

	virtual v2s32 getMousePos();
	virtual void setMousePos(s32 x, s32 y);

	virtual s32 getMouseWheel()
	{
		return m_receiver->getMouseWheel();
	}

	MyEventReceiver *getReceiver() const { return m_receiver; }

	void clear()
	{
		joystick.clear();
		m_receiver->clearInput();
	}

	void releaseAllKeys()
	{
		joystick.releaseAllKeys();
		m_receiver->releaseAllKeys();
	}

private:
	MyEventReceiver *m_receiver = nullptr;
	v2s32 m_mousepos;
};

// Joystick-only handler used for extra split-screen seats.
class GamepadInputHandler final : public InputHandler
{
public:
	GamepadInputHandler(MyEventReceiver *receiver, u8 joystick_id) : m_receiver(receiver)
	{
		joystick.setJoystickId(joystick_id);
		m_receiver->registerJoystick(&joystick);
	}

	~GamepadInputHandler()
	{
		m_receiver->unregisterJoystick(&joystick);
	}

	bool isKeyDown(GameKeyType k) override { return joystick.isKeyDown(k); }
	bool wasKeyDown(GameKeyType k) override { return joystick.wasKeyDown(k); }
	bool wasKeyPressed(GameKeyType k) override { return joystick.wasKeyPressed(k); }
	bool wasKeyReleased(GameKeyType k) override { return joystick.wasKeyReleased(k); }

	bool cancelPressed() override { return false; }

	float getJoystickSpeed() override { return joystick.getMovementSpeed(); }
	float getJoystickDirection() override { return joystick.getMovementDirection(); }

	v2s32 getMousePos() override { return v2s32(0, 0); }
	void setMousePos(s32, s32) override {}
	s32 getMouseWheel() override { return 0; }

	void clear() override { joystick.clear(); }
	void releaseAllKeys() override { joystick.releaseAllKeys(); }

	void clearWasKeyPressed() override
	{
		joystick.clearAllWasKeyPressed();
	}

	void clearWasKeyReleased() override
	{
		joystick.clearAllWasKeyReleased();
	}

private:
	MyEventReceiver *m_receiver = nullptr;
};

class RandomInputHandler final : public InputHandler
{
public:
	RandomInputHandler() = default;

	bool isRandom() const
	{
		return true;
	}

	virtual bool isKeyDown(GameKeyType k) { return keydown[k]; }
	virtual bool wasKeyDown(GameKeyType k) { return false; }
	virtual bool wasKeyPressed(GameKeyType k) { return false; }
	virtual bool wasKeyReleased(GameKeyType k) { return false; }
	virtual bool cancelPressed() { return false; }
	virtual float getJoystickSpeed() { return joystickSpeed; }
	virtual float getJoystickDirection() { return joystickDirection; }
	virtual v2s32 getMousePos() { return mousepos; }
	virtual void setMousePos(s32 x, s32 y) { mousepos = v2s32(x, y); }

	virtual s32 getMouseWheel() { return 0; }

	virtual void step(float dtime);

	s32 Rand(s32 min, s32 max);

private:
	std::bitset<GameKeyType::INTERNAL_ENUM_COUNT> keydown;
	v2s32 mousepos;
	v2s32 mousespeed;
	float joystickSpeed;
	float joystickDirection;
};
