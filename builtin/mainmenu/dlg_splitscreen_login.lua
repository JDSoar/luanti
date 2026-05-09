-- Luanti
-- SPDX-License-Identifier: LGPL-2.1-or-later

-- Layout constants (formspec units). One row contains both the name and
-- password fields side-by-side so the dialog stays compact even with 4 seats.
local FORM_W      = 8.6
local TITLE_Y     = 0.4
local HEADER_Y    = 1.05   -- "Name" / "Password" column headers
local FIRST_ROW_Y = 1.45   -- first per-player row
local ROW_H       = 1.05
local LABEL_W     = 1.3
local FIELD_W     = 3.3
local FIELD_H     = 0.75
local X_LABEL     = 0.3
local X_NAME      = X_LABEL + LABEL_W
local X_PWD       = X_NAME + FIELD_W + 0.2
local BTN_H       = 0.8
local BTN_GAP     = 0.4    -- vertical gap between last row and buttons
local ERR_H       = 0.6    -- height reserved for an inline error message
local BTN_PAD     = 0.4    -- bottom padding under the buttons

local function get_seats()
	local seats = tonumber(core.settings:get("splitscreen.seats") or "2") or 2
	return math.max(2, math.min(4, seats))
end

local function make_row(y, label, name_field, name_value, pwd_field)
	-- Label Y in formspec_version[6] is the label's vertical center, so
	-- placing it at y + FIELD_H/2 visually centers it with the field.
	return ("label[%f,%f;%s]"):format(X_LABEL, y + FIELD_H * 0.5,
			core.formspec_escape(label)) ..
		("field[%f,%f;%f,%f;%s;;%s]"):format(
			X_NAME, y, FIELD_W, FIELD_H, name_field,
			core.formspec_escape(name_value or "")) ..
		("pwdfield[%f,%f;%f,%f;%s;]"):format(
			X_PWD, y, FIELD_W, FIELD_H, pwd_field)
end

local function build_formspec(dialogdata)
	local seats = get_seats()
	local mode = dialogdata.mode -- "online" or "local"

	-- Compute form height so the engine's outline wraps the content.
	-- All `seats` rows are always shown (Player 1 included).
	local last_row_y = FIRST_ROW_Y + (seats - 1) * ROW_H
	local err_y      = last_row_y + FIELD_H + 0.2
	local has_error  = dialogdata.error and dialogdata.error ~= ""
	local btn_y      = last_row_y + FIELD_H + BTN_GAP +
			(has_error and ERR_H or 0)
	local form_h     = btn_y + BTN_H + BTN_PAD

	local title = fgettext("Split-screen players")
	local btn_label = mode == "online" and fgettext("Connect") or fgettext("Play")

	-- Default name shown in the Player 1 row. For online we use the name
	-- typed on the Join Game tab; for local couch we prefer the persisted
	-- "name" setting (the engine writes it back when the user customizes
	-- Player 1) and fall back to the engine default "singleplayer".
	local seat0_default
	if mode == "online" then
		seat0_default = dialogdata.seat0_name or ""
	else
		local saved = core.settings:get("name") or ""
		seat0_default = (saved ~= "") and saved or "singleplayer"
	end

	local fs = "formspec_version[6]" ..
		("size[%f,%f]"):format(FORM_W, form_h) ..
		("label[%f,%f;%s]"):format(X_LABEL, TITLE_Y, core.formspec_escape(title))

	-- Column headers.
	fs = fs ..
		("label[%f,%f;%s]"):format(X_NAME, HEADER_Y, core.formspec_escape(fgettext("Name"))) ..
		("label[%f,%f;%s]"):format(X_PWD, HEADER_Y, core.formspec_escape(fgettext("Password")))

	-- Per-player rows. Player 1 (seat 0) is always editable.
	local y = FIRST_ROW_Y
	fs = fs .. make_row(y, fgettext("Player 1"),
		"ss_name0",
		dialogdata.values.ss_name0 or seat0_default,
		"ss_pwd0")
	y = y + ROW_H

	for i = 1, seats - 1 do
		local default_name = dialogdata.values["ss_name" .. i]
				or core.settings:get("splitscreen.name" .. i) or ""
		fs = fs .. make_row(y, fgettext("Player $1", i + 1),
			"ss_name" .. i, default_name, "ss_pwd" .. i)
		y = y + ROW_H
	end

	-- Inline error message (e.g. "Name required") shown above the buttons.
	if has_error then
		fs = fs ..
			("box[%f,%f;%f,%f;darkred]"):format(
				X_LABEL, err_y, FORM_W - 2 * X_LABEL, ERR_H - 0.05) ..
			("label[%f,%f;%s]"):format(
				X_LABEL + 0.2, err_y + (ERR_H - 0.05) * 0.5,
				core.formspec_escape(dialogdata.error))
	end

	fs = fs ..
		("button[%f,%f;1.8,%f;btn_cancel;%s]"):format(
			FORM_W * 0.5 - 2.0, btn_y, BTN_H, fgettext("Cancel")) ..
		("button[%f,%f;3.6,%f;btn_connect;%s]"):format(
			FORM_W * 0.5 + 0.0, btn_y, BTN_H, btn_label)

	return fs
end

-- Snapshot the user-typed names so an error re-render or any other
-- intervening submission doesn't wipe them. Passwords are intentionally not
-- stored (pwdfield never echoes its value back into the formspec).
local function snapshot_values(dialogdata, fields)
	dialogdata.values = dialogdata.values or {}
	for _, k in ipairs({ "ss_name0", "ss_name1", "ss_name2", "ss_name3" }) do
		if fields[k] ~= nil then
			dialogdata.values[k] = fields[k]
		end
	end
end

-- Returns the trimmed Player 1 name typed in the dialog, or an error string
-- describing what's missing. Player 1 is always required (we no longer fall
-- back to "singleplayer" or the Join Game tab name silently).
local function validate_seat0(fields)
	local n = (fields.ss_name0 or ""):trim()
	if n == "" then
		return nil, fgettext("Please enter a name for Player 1.")
	end
	return n, nil
end

local function handle_buttons_online(this, fields)
	if fields.quit then
		return true
	end

	snapshot_values(this.data, fields)

	if fields.btn_cancel then
		this:delete()
		return true
	end

	if fields.btn_connect then
		local name0, err = validate_seat0(fields)
		if err then
			this.data.error = err
			return true
		end
		this.data.error = nil

		local seats = get_seats()

		gamedata.splitscreen_enable = true
		gamedata.splitscreen_seats = seats
		gamedata.splitscreen_layout = core.settings:get("splitscreen.layout") or ""

		gamedata.playername = name0
		gamedata.password = fields.ss_pwd0 or ""
		gamedata.address = this.data.address
		gamedata.port = this.data.port
		gamedata.selected_world = 0

		for i = 1, seats - 1 do
			local n = (fields["ss_name" .. i] or ""):trim()
			gamedata["splitscreen_name" .. i] = n
			gamedata["splitscreen_password" .. i] = fields["ss_pwd" .. i] or ""
			if n ~= "" then
				core.settings:set("splitscreen.name" .. i, n)
			end
		end

		core.start()
		this:delete()
		return true
	end

	return false
end

local function handle_buttons_local(this, fields)
	if fields.quit then
		return true
	end

	snapshot_values(this.data, fields)

	if fields.btn_cancel then
		this:delete()
		return true
	end

	if fields.btn_connect then
		local name0, err = validate_seat0(fields)
		if err then
			this.data.error = err
			return true
		end
		this.data.error = nil

		local seats = get_seats()

		gamedata.splitscreen_enable = true
		gamedata.splitscreen_seats = seats
		gamedata.splitscreen_layout = core.settings:get("splitscreen.layout") or ""

		-- Always override Player 1's name; clientlauncher.cpp's singleplayer
		-- override is suppressed whenever splitscreen is on and a non-empty
		-- name is provided.
		gamedata.playername = name0

		for i = 1, seats - 1 do
			local n = (fields["ss_name" .. i] or ""):trim()
			gamedata["splitscreen_name" .. i] = n
			gamedata["splitscreen_password" .. i] = fields["ss_pwd" .. i] or ""
			if n ~= "" then
				core.settings:set("splitscreen.name" .. i, n)
			end
		end

		core.start()
		this:delete()
		return true
	end

	return false
end

--[[ Online: opened after the user clicks Login on the Join Game tab when
     split-screen is enabled. Player 1 is pre-filled with the Name typed on
     the Join Game tab but is always editable, and a name is required. ]]
function create_splitscreen_login_dialog(address, port, seat0_name, seat0_pwd)
	local dlg = dialog_create("splitscreen_login",
		build_formspec, handle_buttons_online, nil)
	dlg.data.mode        = "online"
	dlg.data.address     = address
	dlg.data.port        = port
	dlg.data.seat0_name  = seat0_name or ""
	dlg.data.seat0_pwd   = seat0_pwd or ""
	dlg.data.values      = {}
	dlg.data.error       = nil
	return dlg
end

--[[ Local couch co-op (after choosing a world on the Start Game tab):
     do not overwrite the world selection, address, or singleplayer flag.
     Player 1 is pre-filled with "singleplayer" but is always editable, and
     a name is required. ]]
function create_splitscreen_local_login_dialog()
	local dlg = dialog_create("splitscreen_login_local",
		build_formspec, handle_buttons_local, nil)
	dlg.data.mode    = "local"
	dlg.data.values  = {}
	dlg.data.error   = nil
	return dlg
end
