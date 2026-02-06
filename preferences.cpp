#include "stdafx.h"
#include "latinize.h"

#ifdef _WIN32
#include "resource.h"
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>
#include <atlctrls.h>
#endif // _WIN32

using namespace foo_latinize;

#ifdef _WIN32
static void trim_ascii_local(pfc::string8& s) {
	const char* p = s.c_str();
	size_t start = 0;
	size_t end = strlen(p);
	while (start < end && (unsigned char)p[start] <= 0x20) ++start;
	while (end > start && (unsigned char)p[end - 1] <= 0x20) --end;
	if (start == 0 && end == strlen(p)) return;
	pfc::string8 tmp;
	tmp.add_string(p + start, end - start);
	s = tmp;
}

class CPrefsMain : public CDialogImpl<CPrefsMain>, public preferences_page_instance {
public:
	CPrefsMain(preferences_page_callback::ptr callback) : m_callback(callback) {}

	enum { IDD = IDD_PREFS_MAIN };

	t_uint32 get_state();
	void apply();
	void reset();

	BEGIN_MSG_MAP_EX(CPrefsMain)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_HANDLER_EX(IDC_API_URL, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_API_KEY, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_MODEL, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_PROMPT, EN_CHANGE, OnEditChange)
		COMMAND_HANDLER_EX(IDC_DB_PATH, EN_CHANGE, OnEditChange)
	END_MSG_MAP()
private:
	BOOL OnInitDialog(CWindow, LPARAM);
	void OnEditChange(UINT, int, CWindow);
	bool HasChanged();
	void OnChanged();

	const preferences_page_callback::ptr m_callback;
	fb2k::CDarkModeHooks m_dark;
};

BOOL CPrefsMain::OnInitDialog(CWindow, LPARAM) {
	m_dark.AddDialogWithControls(*this);

	uSetDlgItemText(*this, IDC_API_URL, cfg_api_url.get().c_str());
	uSetDlgItemText(*this, IDC_API_KEY, cfg_api_key.get().c_str());
	uSetDlgItemText(*this, IDC_MODEL, cfg_api_model.get().c_str());
	uSetDlgItemText(*this, IDC_PROMPT, cfg_prompt.get().c_str());
	uSetDlgItemText(*this, IDC_DB_PATH, cfg_db_path.get().c_str());
	return FALSE;
}

void CPrefsMain::OnEditChange(UINT, int, CWindow) {
	OnChanged();
}

t_uint32 CPrefsMain::get_state() {
	t_uint32 state = preferences_state::resettable | preferences_state::dark_mode_supported;
	if (HasChanged()) state |= preferences_state::changed;
	return state;
}

void CPrefsMain::reset() {
	uSetDlgItemText(*this, IDC_API_URL, default_api_url());
	uSetDlgItemText(*this, IDC_API_KEY, "");
	uSetDlgItemText(*this, IDC_MODEL, default_api_model());
	uSetDlgItemText(*this, IDC_PROMPT, default_prompt());
	uSetDlgItemText(*this, IDC_DB_PATH, "");
	OnChanged();
}

void CPrefsMain::apply() {
	cfg_api_url = uGetDlgItemText(*this, IDC_API_URL);
	cfg_api_key = uGetDlgItemText(*this, IDC_API_KEY);
	cfg_api_model = uGetDlgItemText(*this, IDC_MODEL);
	cfg_prompt = uGetDlgItemText(*this, IDC_PROMPT);
	cfg_db_path = uGetDlgItemText(*this, IDC_DB_PATH);
	OnChanged();
}

bool CPrefsMain::HasChanged() {
	if (uGetDlgItemText(*this, IDC_API_URL) != cfg_api_url.get()) return true;
	if (uGetDlgItemText(*this, IDC_API_KEY) != cfg_api_key.get()) return true;
	if (uGetDlgItemText(*this, IDC_MODEL) != cfg_api_model.get()) return true;
	if (uGetDlgItemText(*this, IDC_PROMPT) != cfg_prompt.get()) return true;
	if (uGetDlgItemText(*this, IDC_DB_PATH) != cfg_db_path.get()) return true;
	return false;
}

void CPrefsMain::OnChanged() {
	m_callback->on_state_changed();
}

class preferences_page_main : public preferences_page_impl<CPrefsMain> {
public:
	const char* get_name() { return "Latinize Sort"; }
	GUID get_guid() {
		return GUID{ 0x8f0ed4f1, 0x88a6, 0x4e0f, { 0x91, 0x20, 0x9a, 0x3d, 0x55, 0x9c, 0x18, 0xe2 } };
	}
	GUID get_parent_guid() { return guid_tools; }
};

static preferences_page_factory_t<preferences_page_main> g_preferences_page_main_factory;

class CPrefsCache : public CDialogImpl<CPrefsCache>, public preferences_page_instance {
public:
	CPrefsCache(preferences_page_callback::ptr) {}

	enum { IDD = IDD_PREFS_CACHE };

	t_uint32 get_state() { return preferences_state::dark_mode_supported; }
	void apply() {}
	void reset() {}

	BEGIN_MSG_MAP_EX(CPrefsCache)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_HANDLER_EX(IDC_CACHE_SEARCH, EN_CHANGE, OnSearchChange)
		COMMAND_ID_HANDLER_EX(IDC_CACHE_FIND, OnFind)
		COMMAND_ID_HANDLER_EX(IDC_CACHE_REFRESH, OnRefresh)
		COMMAND_ID_HANDLER_EX(IDC_CACHE_SAVE, OnSave)
		COMMAND_ID_HANDLER_EX(IDC_CACHE_DELETE, OnDelete)
		COMMAND_ID_HANDLER_EX(IDC_CACHE_CLEAR, OnClear)
		NOTIFY_HANDLER_EX(IDC_CACHE_LIST, LVN_ITEMCHANGED, OnListChanged)
	END_MSG_MAP()
private:
	BOOL OnInitDialog(CWindow, LPARAM);
	void OnSearchChange(UINT, int, CWindow);
	void OnFind(UINT, int, CWindow);
	void OnRefresh(UINT, int, CWindow);
	void OnSave(UINT, int, CWindow);
	void OnDelete(UINT, int, CWindow);
	void OnClear(UINT, int, CWindow);
	LRESULT OnListChanged(LPNMHDR hdr);

	void RefreshList();
	void UpdateSelection();
	void UpdateFilterFromUI();
	bool MatchesFilter(const cache_entry& e) const;
	bool IsAsciiOnly(const pfc::string8& s) const;
	pfc::string8 AsciiLower(const pfc::string8& s) const;
	bool ContainsAsciiCI(const pfc::string8& haystack, const pfc::string8& needle) const;

	CListViewCtrl m_list;
	pfc::list_t<cache_entry> m_cache;
	int m_selIndex = -1;
	pfc::string8 m_filter;
	fb2k::CDarkModeHooks m_dark;
};

BOOL CPrefsCache::OnInitDialog(CWindow, LPARAM) {
	m_dark.AddDialogWithControls(*this);
	m_list.Attach(GetDlgItem(IDC_CACHE_LIST));
	m_list.SetExtendedListViewStyle(LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
	m_list.InsertColumn(0, L"Kind", LVCFMT_LEFT, 60, 0);
	m_list.InsertColumn(1, L"Hash", LVCFMT_LEFT, 140, 1);
	m_list.InsertColumn(2, L"Title Latin", LVCFMT_LEFT, 150, 2);
	m_list.InsertColumn(3, L"Album Latin", LVCFMT_LEFT, 150, 3);
	RefreshList();
	return FALSE;
}

void CPrefsCache::OnSearchChange(UINT, int, CWindow) {
	UpdateFilterFromUI();
	RefreshList();
}

void CPrefsCache::OnFind(UINT, int, CWindow) {
	UpdateFilterFromUI();
	RefreshList();
}

void CPrefsCache::OnRefresh(UINT, int, CWindow) {
	UpdateFilterFromUI();
	RefreshList();
}

void CPrefsCache::OnSave(UINT, int, CWindow) {
	if (m_selIndex < 0 || m_selIndex >= (int)m_cache.get_count()) return;
	cache_entry entry = m_cache[m_selIndex];
	entry.title = uGetDlgItemText(*this, IDC_CACHE_TITLE);
	entry.album = uGetDlgItemText(*this, IDC_CACHE_ALBUM);
	if (update_cache_entry(entry)) {
		RefreshList();
	}
}

void CPrefsCache::OnDelete(UINT, int, CWindow) {
	if (m_selIndex < 0 || m_selIndex >= (int)m_cache.get_count()) return;
	const cache_entry& entry = m_cache[m_selIndex];
	if (delete_cache_entry(entry.is_track, entry.hash)) {
		RefreshList();
	}
}

void CPrefsCache::OnClear(UINT, int, CWindow) {
	clear_cache();
	RefreshList();
}

LRESULT CPrefsCache::OnListChanged(LPNMHDR hdr) {
	auto* p = reinterpret_cast<LPNMLISTVIEW>(hdr);
	if ((p->uChanged & LVIF_STATE) && (p->uNewState & LVIS_SELECTED)) {
		m_selIndex = (int)m_list.GetItemData(p->iItem);
		UpdateSelection();
	}
	return 0;
}

void CPrefsCache::RefreshList() {
	get_cache_snapshot(m_cache);
	m_list.DeleteAllItems();
	for (t_size i = 0; i < m_cache.get_count(); ++i) {
		const cache_entry& e = m_cache[i];
		if (!MatchesFilter(e)) continue;
		const wchar_t* kind = e.is_track ? L"Track" : L"Album";
		pfc::string8 hashStr = pfc::format_hex(e.hash, 16);
		int idx = m_list.InsertItem((int)i, kind);
		m_list.SetItemText(idx, 1, pfc::stringcvt::string_wide_from_utf8(hashStr));
		m_list.SetItemText(idx, 2, pfc::stringcvt::string_wide_from_utf8(e.title));
		m_list.SetItemText(idx, 3, pfc::stringcvt::string_wide_from_utf8(e.album));
		m_list.SetItemData(idx, (DWORD_PTR)i);
	}
	m_selIndex = -1;
	uSetDlgItemText(*this, IDC_CACHE_KIND, "");
	uSetDlgItemText(*this, IDC_CACHE_HASH, "");
	uSetDlgItemText(*this, IDC_CACHE_TITLE, "");
	uSetDlgItemText(*this, IDC_CACHE_ALBUM, "");
}

void CPrefsCache::UpdateFilterFromUI() {
	m_filter = uGetDlgItemText(*this, IDC_CACHE_SEARCH);
	trim_ascii_local(m_filter);
}

bool CPrefsCache::MatchesFilter(const cache_entry& e) const {
	if (m_filter.length() == 0) return true;
	if (IsAsciiOnly(m_filter)) {
		if (ContainsAsciiCI(e.title, m_filter)) return true;
		if (ContainsAsciiCI(e.album, m_filter)) return true;
		return false;
	}
	if (strstr(e.title.c_str(), m_filter.c_str()) != nullptr) return true;
	if (strstr(e.album.c_str(), m_filter.c_str()) != nullptr) return true;
	return false;
}

bool CPrefsCache::IsAsciiOnly(const pfc::string8& s) const {
	const char* p = s.c_str();
	for (; *p; ++p) {
		if ((unsigned char)*p >= 0x80) return false;
	}
	return true;
}

pfc::string8 CPrefsCache::AsciiLower(const pfc::string8& s) const {
	pfc::string8 out;
	for (const char* p = s.c_str(); *p; ++p) {
		unsigned char c = (unsigned char)*p;
		out.add_char((char)tolower(c));
	}
	return out;
}

bool CPrefsCache::ContainsAsciiCI(const pfc::string8& haystack, const pfc::string8& needle) const {
	const pfc::string8 h = AsciiLower(haystack);
	const pfc::string8 n = AsciiLower(needle);
	return strstr(h.c_str(), n.c_str()) != nullptr;
}

void CPrefsCache::UpdateSelection() {
	if (m_selIndex < 0 || m_selIndex >= (int)m_cache.get_count()) return;
	const cache_entry& e = m_cache[m_selIndex];
	uSetDlgItemText(*this, IDC_CACHE_KIND, e.is_track ? "Track" : "Album");
	uSetDlgItemText(*this, IDC_CACHE_HASH, pfc::format_hex(e.hash, 16));
	uSetDlgItemText(*this, IDC_CACHE_TITLE, e.title);
	uSetDlgItemText(*this, IDC_CACHE_ALBUM, e.album);
}

class preferences_page_cache : public preferences_page_impl<CPrefsCache> {
public:
	const char* get_name() { return "Latinize Cache"; }
	GUID get_guid() {
		return GUID{ 0x0e33c11f, 0x46cd, 0x4a6f, { 0x9c, 0x4a, 0x35, 0x6c, 0x70, 0x9d, 0x9e, 0x0c } };
	}
	GUID get_parent_guid() { return guid_tools; }
};

static preferences_page_factory_t<preferences_page_cache> g_preferences_page_cache_factory;

class CPrefsTest : public CDialogImpl<CPrefsTest>, public preferences_page_instance {
public:
	CPrefsTest(preferences_page_callback::ptr) {}

	enum { IDD = IDD_PREFS_TEST };

	t_uint32 get_state() { return preferences_state::dark_mode_supported; }
	void apply() {}
	void reset() {}

	BEGIN_MSG_MAP_EX(CPrefsTest)
		MSG_WM_INITDIALOG(OnInitDialog)
		COMMAND_ID_HANDLER_EX(IDC_TEST_RUN, OnRunTest)
	END_MSG_MAP()
private:
	BOOL OnInitDialog(CWindow, LPARAM);
	void OnRunTest(UINT, int, CWindow);

	fb2k::CDarkModeHooks m_dark;
};

BOOL CPrefsTest::OnInitDialog(CWindow, LPARAM) {
	m_dark.AddDialogWithControls(*this);
	return FALSE;
}

void CPrefsTest::OnRunTest(UINT, int, CWindow) {
	const pfc::string8 title = uGetDlgItemText(*this, IDC_TEST_TITLE);
	const pfc::string8 album = uGetDlgItemText(*this, IDC_TEST_ALBUM);
	uSetDlgItemText(*this, IDC_TEST_OUTPUT, "Testing...");
	uSetDlgItemText(*this, IDC_TEST_RAW, "");

	struct result_t {
		pfc::string8 title;
		pfc::string8 album;
		pfc::string8 error;
		pfc::string8 raw;
		bool ok = false;
	};
	auto result = std::make_shared<result_t>();

	auto task = threaded_process_callback_lambda::create(
		[](threaded_process_callback::ctx_t) {},
		[title, album, result](threaded_process_status&, abort_callback&) {
			result->ok = test_latinize(title.c_str(), album.c_str(), result->title, result->album, result->error, result->raw);
		},
		[this, result](threaded_process_callback::ctx_t, bool) {
			pfc::string8 out;
			if (result->ok) {
				out << "title_latin: " << result->title << "\r\n";
				out << "album_latin: " << result->album << "\r\n";
			} else {
				out << "FAILED: " << result->error;
			}
			uSetDlgItemText(*this, IDC_TEST_OUTPUT, out);
			uSetDlgItemText(*this, IDC_TEST_RAW, result->raw);
		}
	);

	threaded_process::g_run_modal(task, threaded_process::flag_show_abort, *this, "Test LLM");
}

class preferences_page_test : public preferences_page_impl<CPrefsTest> {
public:
	const char* get_name() { return "Latinize Test"; }
	GUID get_guid() {
		return GUID{ 0x2d9a6a0b, 0xb742, 0x4f5c, { 0x8b, 0x6f, 0xf5, 0x23, 0x4a, 0x14, 0x6c, 0x7a } };
	}
	GUID get_parent_guid() { return guid_tools; }
};

static preferences_page_factory_t<preferences_page_test> g_preferences_page_test_factory;
#endif // _WIN32
