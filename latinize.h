#pragma once

#include "stdafx.h"

namespace foo_latinize {
	struct cache_entry {
		bool is_track = false;
		metadb_index_hash hash = 0;
		pfc::string8 title;
		pfc::string8 album;
	};

	// Config variables (stored in foobar2000 config)
	extern cfg_string cfg_api_url;
	extern cfg_string cfg_api_key;
	extern cfg_string cfg_api_model;
	extern cfg_string cfg_prompt;
	extern cfg_string cfg_db_path;

	// Defaults (used by preferences reset)
	const char* default_api_url();
	const char* default_api_model();
	const char* default_prompt();

	// Effective values
	pfc::string8 get_db_path();

	// Cache access (preferences UI)
	void get_cache_snapshot(pfc::list_t<cache_entry>& out);
	bool update_cache_entry(const cache_entry& entry);
	bool delete_cache_entry(bool is_track, metadb_index_hash hash);
	void clear_cache();

	// Manual test helper
	bool test_latinize(const char* title, const char* album, pfc::string8& outTitle, pfc::string8& outAlbum, pfc::string8& outError, pfc::string8& outRaw);

	// Command entry point
	void RunLatinize(metadb_handle_list_cref data, fb2k::hwnd_t parent);
	void ClearLatinizeAll(metadb_handle_list_cref data, fb2k::hwnd_t parent);
	void ClearLatinizeTitleOnly(metadb_handle_list_cref data, fb2k::hwnd_t parent);
	void ClearLatinizeAlbumOnly(metadb_handle_list_cref data, fb2k::hwnd_t parent);
}
