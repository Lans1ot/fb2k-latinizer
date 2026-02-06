#include "stdafx.h"
#include "latinize.h"

// Context menu integration for the component.
// This file registers a popup menu group and several commands that call into
// the latinize feature (see latinize.cpp).

// Menu group GUID: groups our commands under "Latinize Sort" in the context menu.
// Use a stable GUID so foobar2000 can remember menu customizations.
static const GUID guid_latinize_group = { 0x572de7f4, 0xcbdf, 0x479a, { 0x97, 0x26, 0x0a, 0xb0, 0x97, 0x47, 0x69, 0xe3 } };
static contextmenu_group_popup_factory g_latinize_group(
	guid_latinize_group, contextmenu_groups::root, "Latinize Sort", 0
);

class latinize_context_item : public contextmenu_item_simple {
public:
	// Command indices used by the contextmenu_item_simple API.
	enum { cmd_latinize = 0, cmd_clear_all, cmd_clear_title, cmd_clear_album, cmd_total };

	// Put commands under our popup group.
	GUID get_parent() { return guid_latinize_group; }
	unsigned get_num_items() { return cmd_total; }

	// Visible names for each menu entry.
	void get_item_name(unsigned index, pfc::string_base& out) {
		switch (index) {
		case cmd_latinize:
			out = "Latinize title/album (LLM)";
			return;
		case cmd_clear_all:
			out = "Clear latinized title/album (all)";
			return;
		case cmd_clear_title:
			out = "Clear latinized title only";
			return;
		case cmd_clear_album:
			out = "Clear latinized album only";
			return;
		default:
			out = "";
			return;
		}
	}

	// When a menu item is clicked, call into latinize.cpp entry points.
	void context_command(unsigned index, metadb_handle_list_cref data, const GUID&) {
		switch (index) {
		case cmd_latinize:
			foo_latinize::RunLatinize(data, core_api::get_main_window());
			return;
		case cmd_clear_all:
			foo_latinize::ClearLatinizeAll(data, core_api::get_main_window());
			return;
		case cmd_clear_title:
			foo_latinize::ClearLatinizeTitleOnly(data, core_api::get_main_window());
			return;
		case cmd_clear_album:
			foo_latinize::ClearLatinizeAlbumOnly(data, core_api::get_main_window());
			return;
		default:
			return;
		}
	}

	// Stable GUID per command (required by the API).
	GUID get_item_guid(unsigned index) {
		switch (index) {
		case cmd_latinize:
			return GUID{ 0x3f2b6d1b, 0x0e7b, 0x4a7b, { 0x8a, 0x0f, 0x5f, 0xd2, 0x01, 0x7c, 0x9b, 0x46 } };
		case cmd_clear_all:
			return GUID{ 0x0a4bd19d, 0x9c32, 0x4e6a, { 0xa2, 0x1c, 0x2f, 0x5f, 0x78, 0x3a, 0x4f, 0x5d } };
		case cmd_clear_title:
			return GUID{ 0x1a90f70a, 0x6b7f, 0x4a1c, { 0x9b, 0x2f, 0x7a, 0x43, 0x7b, 0x35, 0x0e, 0x2a } };
		case cmd_clear_album:
			return GUID{ 0x9a6e7c55, 0x1d2a, 0x4b54, { 0xa7, 0x07, 0xf2, 0xa2, 0xb1, 0x6c, 0x2b, 0x18 } };
		default:
			return pfc::guid_null;
		}
	}

	// Tooltip/help text shown in menu customization UI.
	bool get_item_description(unsigned index, pfc::string_base& out) {
		switch (index) {
		case cmd_latinize:
			out = "Calls your configured LLM API to compute latinized title/album names and stores them in the component database.";
			return true;
		case cmd_clear_all:
			out = "Clears cached latinized title/album names for the selected tracks.";
			return true;
		case cmd_clear_title:
			out = "Clears cached latinized title for the selected tracks (album cache unchanged).";
			return true;
		case cmd_clear_album:
			out = "Clears cached latinized album for the selected tracks and album cache.";
			return true;
		default:
			return false;
		}
	}
};

// Factory registers the item with foobar2000.
static contextmenu_item_factory_t<latinize_context_item> g_latinize_context_factory;
