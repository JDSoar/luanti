// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#pragma once

/*
	All kinds of stuff that needs to be exposed from main.cpp
*/
#include "modalMenu.h"
#include "touchcontrols.h" // g_touchcontrols
#include <algorithm>
#include <cassert>
#include <list>
#include <vector>

#include <IGUIEnvironment.h>

namespace gui {
	class IGUIStaticText;
}

class IGameCallback
{
public:
	virtual void exitToOS() = 0;
	virtual void openSettings() = 0;
	virtual void disconnect() = 0;
	virtual void changePassword() = 0;
	virtual void changeVolume() = 0;
	virtual void showOpenURLDialog(const std::string &url) = 0;
	virtual void touchscreenLayout() = 0;
};

extern gui::IGUIEnvironment *guienv;
extern gui::IGUIStaticText *guiroot;

// Handler for the modal menus

class MainMenuManager : public IMenuManager
{
public:
	virtual void createdMenu(gui::IGUIElement *menu)
	{
		for (gui::IGUIElement *e : m_stack) {
			if (e == menu)
				return;
		}

		// Split-screen friendly: a "viewport menu" is constrained to a
		// sub-rectangle of the screen (one player's panel). Multiple
		// viewport menus can be visible at once because they paint into
		// disjoint regions, so we don't hide previously-stacked viewport
		// menus when adding another viewport menu. Adding a *full-screen*
		// menu (e.g. pause menu) on top hides every existing menu.
		GUIModalMenu *new_mm = dynamic_cast<GUIModalMenu *>(menu);
		bool new_is_viewport = new_mm && new_mm->getViewport().getWidth() > 0;

		if (!m_stack.empty()) {
			if (!new_is_viewport) {
				for (gui::IGUIElement *e : m_stack)
					e->setVisible(false);
			} else {
				// Adding a viewport menu only hides a fullscreen menu
				// that may already be on top; existing viewport menus
				// stay visible because they live in disjoint panels.
				GUIModalMenu *top_mm =
					dynamic_cast<GUIModalMenu *>(m_stack.back());
				bool top_is_viewport = top_mm &&
					top_mm->getViewport().getWidth() > 0;
				if (!top_is_viewport)
					m_stack.back()->setVisible(false);
			}
		}

		m_stack.push_back(menu);
		guienv->setFocus(m_stack.back());
	}

	/// Note that it may be called multiple times on GUIModalMenu (or GUIFormSpecMenu):
	///   1x Explicit close request
	///   1x Destructor
	virtual void deletingMenu(gui::IGUIElement *menu)
	{
		// Remove all entries if there are duplicates
		m_stack.remove(menu);

		// Reference count reduction (-1) due to focus loss
		if (m_stack.empty()) {
			guienv->removeFocus(menu);
			if (g_touchcontrols)
				g_touchcontrols->show();
			return;
		}

		// If the new top is a viewport menu (split-screen), make sure
		// every viewport menu beneath it is visible too - they were
		// kept visible while siblings were pushed, but a full-screen
		// menu on top will have hidden them. Otherwise just restore the
		// single top menu (legacy behaviour).
		GUIModalMenu *top_mm = dynamic_cast<GUIModalMenu *>(m_stack.back());
		bool top_is_viewport = top_mm && top_mm->getViewport().getWidth() > 0;
		if (top_is_viewport) {
			for (gui::IGUIElement *e : m_stack) {
				GUIModalMenu *mm = dynamic_cast<GUIModalMenu *>(e);
				if (mm && mm->getViewport().getWidth() > 0)
					e->setVisible(true);
			}
		} else {
			m_stack.back()->setVisible(true);
		}
		guienv->setFocus(m_stack.back());
	}

	// Returns true to prevent further processing
	virtual bool preprocessEvent(const SEvent& event)
	{
		if (m_stack.empty())
			return false;
		// Joystick events: each split-screen seat owns a separate
		// GUIFormSpecMenu bound to that seat's joystick controller, and
		// the menu filters by joystick id internally. Broadcasting the
		// event lets seat 1's gamepad scroll seat 1's inventory even
		// when seat 2's inventory is on top of the stack.
		//
		// SAFETY: any `mm->preprocessEvent(event)` below can synchronously
		// destroy `mm` (e.g. the gamepad's mapped INVENTORY / ESC button
		// closes the menu, which calls ~GUIModalMenu → deletingMenu →
		// m_stack.remove(menu)). That would invalidate a live iterator
		// over `m_stack` and the next access dereferences a freed
		// IGUIElement, crashing in __dynamic_cast on the stale vtable.
		// Snapshot the stack first and re-check membership before each
		// dispatch so a self-removing menu can't pull the rug out.
		if (event.EventType == EET_JOYSTICK_INPUT_EVENT) {
			std::vector<gui::IGUIElement *> snapshot(
					m_stack.begin(), m_stack.end());
			bool handled = false;
			for (gui::IGUIElement *e : snapshot) {
				if (!isStillInStack(e))
					continue;
				GUIModalMenu *mm = dynamic_cast<GUIModalMenu *>(e);
				if (mm && mm->preprocessEvent(event))
					handled = true;
			}
			return handled;
		}
		// Mouse / touch: with multiple split-screen viewport menus open
		// the topmost one would otherwise eat clicks meant for a
		// different seat's panel. Prefer a viewport menu whose region
		// actually contains the pointer; fall through to the legacy
		// "top menu" path when none does (e.g. fullscreen pause menu).
		if (event.EventType == EET_MOUSE_INPUT_EVENT ||
				event.EventType == EET_TOUCH_INPUT_EVENT) {
			s32 px, py;
			if (event.EventType == EET_MOUSE_INPUT_EVENT) {
				px = event.MouseInput.X;
				py = event.MouseInput.Y;
			} else {
				px = event.TouchInput.X;
				py = event.TouchInput.Y;
			}
			const core::position2d<s32> pt(px, py);
			std::vector<gui::IGUIElement *> snapshot(
					m_stack.begin(), m_stack.end());
			for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
				if (!isStillInStack(*it))
					continue;
				GUIModalMenu *mm = dynamic_cast<GUIModalMenu *>(*it);
				if (!mm)
					continue;
				const core::rect<s32> &vp = mm->getViewport();
				if (vp.getWidth() > 0 && vp.getHeight() > 0 &&
						vp.isPointInside(pt))
					return mm->preprocessEvent(event);
			}
		}
		GUIModalMenu *mm = dynamic_cast<GUIModalMenu*>(m_stack.back());
		return mm && mm->preprocessEvent(event);
	}

	size_t menuCount() const
	{
		return m_stack.size();
	}

	GUIModalMenu *tryGetTopMenu() const
	{
		if (m_stack.empty())
			return nullptr;
		return dynamic_cast<GUIModalMenu *>(m_stack.back());
	}

	void deleteFront()
	{
		assert(!m_stack.empty());
		gui::IGUIElement *e = m_stack.front();
		e->setVisible(false);
		deletingMenu(e);
		e->remove();
	}

	bool pausesGame()
	{
		for (gui::IGUIElement *i : m_stack) {
			GUIModalMenu *mm = dynamic_cast<GUIModalMenu*>(i);
			if (mm && mm->pausesGame())
				return true;
		}
		return false;
	}

private:
	// True iff `e` is currently in `m_stack`. Used to skip snapshot entries
	// that were destroyed (and thus removed from m_stack via deletingMenu)
	// while we were dispatching events to other entries.
	bool isStillInStack(gui::IGUIElement *e) const
	{
		return std::find(m_stack.begin(), m_stack.end(), e) != m_stack.end();
	}

	std::list<gui::IGUIElement*> m_stack;
};

extern MainMenuManager g_menumgr;

static inline bool isMenuActive()
{
	return g_menumgr.menuCount() != 0;
}

class MainGameCallback : public IGameCallback
{
public:
	MainGameCallback() = default;
	virtual ~MainGameCallback() = default;

	void exitToOS() override
	{
		shutdown_requested = true;
	}

	void openSettings() override
	{
		settings_requested = true;
	}

	void disconnect() override
	{
		disconnect_requested = true;
	}

	void changePassword() override
	{
		changepassword_requested = true;
	}

	void changeVolume() override
	{
		changevolume_requested = true;
	}

	void touchscreenLayout() override
	{
		touchscreenlayout_requested = true;
	}

	void showOpenURLDialog(const std::string &url) override
	{
		show_open_url_dialog = url;
	}

	bool disconnect_requested = false;
	bool settings_requested = false;
	bool changepassword_requested = false;
	bool changevolume_requested = false;
	bool touchscreenlayout_requested = false;
	bool shutdown_requested = false;
	std::string show_open_url_dialog = "";
};

extern MainGameCallback *g_gamecallback;
