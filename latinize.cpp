#include "stdafx.h"
#include "latinize.h"

#include <SDK/cfg_var.h>

#include <cctype>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace foo_latinize {
	// Configuration keys live in foobar2000 config storage. GUIDs must be
	// stable across versions to avoid losing user settings.
	// Config GUIDs - replace with your own for production use
	static constexpr GUID guid_cfg_api_url = { 0xa2d7b0a1, 0x8c5a, 0x4e4f, { 0x9c, 0x6a, 0x6b, 0x8a, 0x1d, 0x2e, 0x11, 0xf0 } };
	static constexpr GUID guid_cfg_api_key = { 0x7f44f1b3, 0x1b2f, 0x4a5b, { 0x9a, 0x2c, 0x91, 0x03, 0x6a, 0x3f, 0x67, 0x1a } };
	static constexpr GUID guid_cfg_api_model = { 0x1f566c64, 0xf2a9, 0x4c0a, { 0x86, 0x13, 0x2a, 0x78, 0x5f, 0x88, 0x5d, 0x42 } };
	static constexpr GUID guid_cfg_prompt = { 0x2d7b06e1, 0xf728, 0x4f49, { 0x90, 0x0a, 0x6d, 0xb7, 0x62, 0x5a, 0xf0, 0x18 } };
	static constexpr GUID guid_cfg_db_path = { 0x9d5c8d7e, 0x7e2a, 0x4c39, { 0x9a, 0x27, 0xa7, 0x3f, 0x5a, 0x31, 0x06, 0x92 } };
	// Defaults used when the user clicks "Reset" in Preferences.
	static constexpr char default_api_url_value[] = "https://api.deepseek.com/chat/completions";
	static constexpr char default_api_model_value[] = "deepseek-chat";
	static constexpr char default_prompt_value[] =
		"Task: Convert song title and album name to Latin letters and digits (A-Z, 0-9 only).\n"
		"Rules:\n"
		"- Ignore all symbols and punctuation.\n"
		"- For Chinese, output pinyin without tone marks.\n"
		"- For Japanese, output romaji.\n"
		"- If any Japanese characters appear (Hiragana, Katakana, or Kanji used with Japanese), treat the whole title/album as Japanese for romanization.\n"
		"- For Chinese (Simplified/Traditional), treat the whole title/album as Chinese for pinyin.\n"
		"- Treat each title/album as a single language by default; do not mix languages inside one title.\n"
		"- Example: \"\xE5\xBF\x83\xE3\x81\xAE\xE5\xA3\xB0\" should be \"kokoro no koe\" (Japanese), NOT \"xin no sheng(Chinese)\".\n"
		"- For English/Latin script, keep the letters as-is.\n"
		"- Output only letters A-Z and digits 0-9 (case-insensitive) and single spaces between words.\n"
		"Output exactly two lines:\n"
		"title_latin: <latinized title>\n"
		"album_latin: <latinized album>\n"
		"Title: {title}\n"
		"Album: {album}\n";
	// Config variables (persisted by foobar2000).
	cfg_string cfg_api_url(guid_cfg_api_url, default_api_url_value);
	cfg_string cfg_api_key(guid_cfg_api_key, "");
	cfg_string cfg_api_model(guid_cfg_api_model, default_api_model_value);
	cfg_string cfg_prompt(guid_cfg_prompt, default_prompt_value);
	cfg_string cfg_db_path(guid_cfg_db_path, "");

	const char* default_api_url() { return default_api_url_value; }
	const char* default_api_model() { return default_api_model_value; }
	const char* default_prompt() { return default_prompt_value; }

	// Default DB location inside the foobar2000 profile directory.
	static pfc::string8 get_db_path_fallback() {
		return core_api::pathInProfile("foo_sample_latin.db");
	}

	// Normalize path to a URL; http_client expects URL-like strings.
	static pfc::string8 normalize_db_path(const pfc::string8& in) {
		if (strstr(in.c_str(), "://") != nullptr) return in;
		pfc::string8 out = "file://";
		out << in;
		return out;
	}

	// Effective database path (user override or default).
	pfc::string8 get_db_path() {
		const auto& path = cfg_db_path.get();
		if (path.length() == 0) return normalize_db_path(get_db_path_fallback());
		return normalize_db_path(path);
	}
}

namespace {
	static void trim_ascii(pfc::string8& s);
	static pfc::string8 sanitize_latin(const char* in);

	// In-memory record for cached latinized values.
	struct latin_record {
		pfc::string8 title;
		pfc::string8 album;
	};

	// Helper to build deterministic hashes from metadata. This lets us cache
	// results per track and per album consistently.
	class latin_keyer {
	public:
	latin_keyer() {
		auto compiler = static_api_ptr_t<titleformat_compiler>();
		compiler->compile_safe_ex(m_track, "%artist% - %title% - %album%");
		// Use album title only to keep album latinization consistent across tracks.
		compiler->compile_safe_ex(m_album, "%album%");
	}

		metadb_index_hash hash_track(const file_info& info, const playable_location& location) {
			pfc::string_formatter s;
			m_track->run_simple(location, &info, s);
			return static_api_ptr_t<hasher_md5>()->process_single_string(s).xorHalve();
		}

		metadb_index_hash hash_album(const file_info& info, const playable_location& location) {
			pfc::string_formatter s;
			m_album->run_simple(location, &info, s);
			return static_api_ptr_t<hasher_md5>()->process_single_string(s).xorHalve();
		}
	private:
		titleformat_object::ptr m_track;
		titleformat_object::ptr m_album;
	};

	// Simple persistent cache:
	// - tracks: keyed by hash of artist/title/album
	// - albums: keyed by hash of album
	// This cache is saved to a local DB file in the profile directory.
	class latin_db {
	public:
		void ensure_loaded() {
			std::lock_guard<std::mutex> lock(m_mutex);
			const auto path = foo_latinize::get_db_path();
			if (m_loaded && m_path == path) return;
			m_path = path;
			m_loaded = true;
			m_dirty = false;
			m_tracks.clear();
			m_albums.clear();
			load_locked();
		}

		bool get_track(metadb_index_hash hash, latin_record& out) {
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_tracks.find(hash);
			if (it == m_tracks.end()) return false;
			out = it->second;
			return true;
		}

		bool get_album(metadb_index_hash hash, pfc::string8& out) {
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_albums.find(hash);
			if (it == m_albums.end()) return false;
			out = it->second;
			return true;
		}

		void set_track(metadb_index_hash hash, const latin_record& rec) {
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_tracks.find(hash);
			if (it != m_tracks.end() && it->second.title == rec.title && it->second.album == rec.album) return;
			m_tracks[hash] = rec;
			m_dirty = true;
		}

		void set_album(metadb_index_hash hash, const pfc::string8& album) {
			std::lock_guard<std::mutex> lock(m_mutex);
			auto it = m_albums.find(hash);
			if (it != m_albums.end() && it->second == album) return;
			m_albums[hash] = album;
			m_dirty = true;
		}

		void save_if_dirty() {
			std::lock_guard<std::mutex> lock(m_mutex);
			if (!m_dirty) return;
			save_locked();
			m_dirty = false;
		}

		void snapshot(pfc::list_t<foo_latinize::cache_entry>& out) {
			std::lock_guard<std::mutex> lock(m_mutex);
			out.remove_all();
			out.prealloc((t_size)(m_tracks.size() + m_albums.size()));
			for (auto const& kv : m_tracks) {
				foo_latinize::cache_entry e;
				e.is_track = true;
				e.hash = kv.first;
				e.title = kv.second.title;
				e.album = kv.second.album;
				out.add_item(e);
			}
			for (auto const& kv : m_albums) {
				foo_latinize::cache_entry e;
				e.is_track = false;
				e.hash = kv.first;
				e.album = kv.second;
				out.add_item(e);
			}
		}

		bool update_entry(const foo_latinize::cache_entry& entry) {
			std::lock_guard<std::mutex> lock(m_mutex);
			if (entry.is_track) {
				latin_record rec;
				rec.title = sanitize_latin(entry.title.c_str());
				rec.album = sanitize_latin(entry.album.c_str());
				auto it = m_tracks.find(entry.hash);
				if (it != m_tracks.end() && it->second.title == rec.title && it->second.album == rec.album) return false;
				m_tracks[entry.hash] = rec;
				m_dirty = true;
				return true;
			} else {
				pfc::string8 album = sanitize_latin(entry.album.c_str());
				auto it = m_albums.find(entry.hash);
				if (it != m_albums.end() && it->second == album) return false;
				m_albums[entry.hash] = album;
				m_dirty = true;
				return true;
			}
		}

		bool delete_entry(bool is_track, metadb_index_hash hash) {
			std::lock_guard<std::mutex> lock(m_mutex);
			if (is_track) {
				auto it = m_tracks.find(hash);
				if (it == m_tracks.end()) return false;
				m_tracks.erase(it);
			} else {
				auto it = m_albums.find(hash);
				if (it == m_albums.end()) return false;
				m_albums.erase(it);
			}
			m_dirty = true;
			return true;
		}

		void clear_all() {
			std::lock_guard<std::mutex> lock(m_mutex);
			if (m_tracks.empty() && m_albums.empty()) return;
			m_tracks.clear();
			m_albums.clear();
			m_dirty = true;
		}

	private:
		void load_locked() {
			abort_callback_dummy abort;
			file::ptr f;
			try {
				filesystem::g_open_read(f, m_path, abort);
			} catch (exception_io_not_found const&) {
				FB2K_console_formatter() << "[latinize] DB not found: " << m_path;
				return;
			} catch (exception_io const&) {
				FB2K_console_formatter() << "[latinize] Failed to open DB for read: " << m_path;
				return;
			}

			try {
				const t_uint32 magic = f->read_lendian_t<t_uint32>(abort);
				const t_uint32 version = f->read_lendian_t<t_uint32>(abort);
				if (magic != 0x544C4246 || version != 1) return; // "FBLT"

				const t_uint32 trackCount = f->read_lendian_t<t_uint32>(abort);
				const t_uint32 albumCount = f->read_lendian_t<t_uint32>(abort);

				for (t_uint32 i = 0; i < trackCount; ++i) {
					const metadb_index_hash hash = f->read_lendian_t<metadb_index_hash>(abort);
					latin_record rec;
					f->read_string(rec.title, abort);
					f->read_string(rec.album, abort);
					m_tracks.emplace(hash, rec);
				}
				for (t_uint32 i = 0; i < albumCount; ++i) {
					const metadb_index_hash hash = f->read_lendian_t<metadb_index_hash>(abort);
					pfc::string8 album;
					f->read_string(album, abort);
					m_albums.emplace(hash, album);
				}
			} catch (exception_io const&) {
				m_tracks.clear();
				m_albums.clear();
				FB2K_console_formatter() << "[latinize] Failed to read DB (corrupt?): " << m_path;
			}
		}

		void save_locked() {
			abort_callback_dummy abort;
			try {
				pfc::string8 dir = m_path;
				dir.truncate(dir.scan_filename());
				if (dir.length() > 0) {
					try {
						filesystem::g_create_directory(dir, abort);
					} catch (exception_io const&) {
						// Directory may already exist or be provided by user; try to write anyway.
					}
				}

				file::ptr f;
				try {
					filesystem::g_open_write_new(f, m_path, abort);
				} catch (exception_io const&) {
					// Fallback: attempt using native path rewrapped as file://
					pfc::string8 native;
					if (filesystem::g_get_native_path(m_path, native, abort)) {
						pfc::string8 alt = "file://";
						alt << native;
						if (alt != m_path) {
							filesystem::g_open_write_new(f, alt, abort);
							FB2K_console_formatter() << "[latinize] DB save path fallback: " << alt;
							m_path = alt;
						}
					} else {
						throw;
					}
				}
				f->write_lendian_t<t_uint32>(0x544C4246, abort); // "FBLT"
				f->write_lendian_t<t_uint32>(1, abort);
				f->write_lendian_t<t_uint32>((t_uint32)m_tracks.size(), abort);
				f->write_lendian_t<t_uint32>((t_uint32)m_albums.size(), abort);

				for (auto const& kv : m_tracks) {
					f->write_lendian_t<metadb_index_hash>(kv.first, abort);
					f->write_string(kv.second.title, abort);
					f->write_string(kv.second.album, abort);
				}
				for (auto const& kv : m_albums) {
					f->write_lendian_t<metadb_index_hash>(kv.first, abort);
					f->write_string(kv.second, abort);
				}
				f->commit(abort);
				FB2K_console_formatter() << "[latinize] DB saved: " << m_path
					<< " (tracks=" << (t_uint32)m_tracks.size() << ", albums=" << (t_uint32)m_albums.size() << ")";
			} catch (exception_io const&) {
				// swallow write errors
				FB2K_console_formatter() << "[latinize] Failed to save DB: " << m_path;
			}
		}

		std::mutex m_mutex;
		pfc::string8 m_path;
		bool m_loaded = false;
		bool m_dirty = false;
		std::unordered_map<metadb_index_hash, latin_record> m_tracks;
		std::unordered_map<metadb_index_hash, pfc::string8> m_albums;
	};

	static latin_db g_db;

	static latin_keyer& get_keyer() {
		static latin_keyer g_keyer;
		return g_keyer;
	}

	static void append_utf8(pfc::string8& out, uint32_t cp) {
		char buf[5] = {};
		size_t len = 0;
		if (cp <= 0x7F) {
			buf[0] = (char)cp; len = 1;
		} else if (cp <= 0x7FF) {
			buf[0] = (char)(0xC0 | (cp >> 6));
			buf[1] = (char)(0x80 | (cp & 0x3F));
			len = 2;
		} else if (cp <= 0xFFFF) {
			buf[0] = (char)(0xE0 | (cp >> 12));
			buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
			buf[2] = (char)(0x80 | (cp & 0x3F));
			len = 3;
		} else {
			buf[0] = (char)(0xF0 | (cp >> 18));
			buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
			buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
			buf[3] = (char)(0x80 | (cp & 0x3F));
			len = 4;
		}
		out.add_string(buf, len);
	}

	static bool parse_json_string(const char*& p, pfc::string8& out) {
		if (*p != '"') return false;
		++p;
		out.reset();
		while (*p) {
			char c = *p++;
			if (c == '"') return true;
			if (c == '\\') {
				char e = *p++;
				switch (e) {
				case '"': out.add_char('"'); break;
				case '\\': out.add_char('\\'); break;
				case '/': out.add_char('/'); break;
				case 'b': out.add_char('\b'); break;
				case 'f': out.add_char('\f'); break;
				case 'n': out.add_char('\n'); break;
				case 'r': out.add_char('\r'); break;
				case 't': out.add_char('\t'); break;
				case 'u': {
					uint32_t cp = 0;
					for (int i = 0; i < 4; ++i) {
						char h = *p++;
						cp <<= 4;
						if (h >= '0' && h <= '9') cp |= (h - '0');
						else if (h >= 'a' && h <= 'f') cp |= (h - 'a' + 10);
						else if (h >= 'A' && h <= 'F') cp |= (h - 'A' + 10);
						else return false;
					}
					append_utf8(out, cp);
					break;
				}
				default:
					out.add_char(e);
					break;
				}
			} else {
				out.add_char(c);
			}
		}
		return false;
	}

	static bool json_find_string_value(const char* json, const char* key, pfc::string8& out) {
		pfc::string8 pattern;
		pattern << '"' << key << '"';
		const char* search = json;
		while (true) {
			const char* p = strstr(search, pattern);
			if (!p) return false;
			p += pattern.length();
			while (*p && *p != ':') ++p;
			if (*p != ':') {
				search = p;
				continue;
			}
			++p;
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
			if (*p == '"') {
				const char* pTry = p;
				if (parse_json_string(pTry, out)) return true;
			}
			search = p + 1;
		}
	}

	static bool json_find_string_value_from(const char* start, const char* key, pfc::string8& out) {
		if (!start) return false;
		return json_find_string_value(start, key, out);
	}

	static bool extract_assistant_content(const char* json, pfc::string8& out) {
		if (!json) return false;
		const char* role = strstr(json, "\"role\":\"assistant\"");
		if (!role) role = strstr(json, "\"role\": \"assistant\"");
		if (!role) return false;
		const char* p = role;
		const char* key = "\"content\"";
		while (true) {
			const char* hit = strstr(p, key);
			if (!hit) return false;
			p = hit + strlen(key);
			while (*p && *p != ':') ++p;
			if (*p != ':') return false;
			++p;
			while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
			if (*p == '"') {
				const char* pTry = p;
				if (parse_json_string(pTry, out)) return true;
			}
			p = hit + 1;
		}
	}

	static bool match_key_ci(const pfc::string8& line, const char* key, size_t& consumed) {
		if (!key) return false;
		const char* s = line.c_str();
		size_t i = 0;
		while (s[i] == ' ' || s[i] == '\t') ++i;
		size_t j = 0;
		while (key[j] && s[i]) {
			char c = s[i];
			if (c == ' ' || c == '\t' || c == '_' || c == '-') { ++i; continue; }
			char a = (char)tolower((unsigned char)c);
			char b = (char)tolower((unsigned char)key[j]);
			if (a != b) return false;
			++i;
			++j;
		}
		if (key[j] != 0) return false;
		consumed = i;
		return true;
	}

	static const char* skip_separators(const char* p) {
		while (*p == ' ' || *p == '\t' || *p == ':' || *p == '-' ) ++p;
		return p;
	}

	static bool parse_latin_lines(const pfc::string8& text, latin_record& out) {
		const char* p = text.c_str();
		pfc::string8 t, a;
		while (*p) {
			while (*p == '\r' || *p == '\n') ++p;
			const char* lineStart = p;
			while (*p && *p != '\r' && *p != '\n') ++p;
			pfc::string8 line;
			line.add_string(lineStart, p - lineStart);
			size_t consumed = 0;
			if (match_key_ci(line, "titlelatin", consumed)) {
				const char* q = line.c_str() + consumed;
				q = skip_separators(q);
				t = q;
				trim_ascii(t);
			} else if (match_key_ci(line, "albumlatin", consumed)) {
				const char* q = line.c_str() + consumed;
				q = skip_separators(q);
				a = q;
				trim_ascii(a);
			}
		}
		out.title = sanitize_latin(t.c_str());
		out.album = sanitize_latin(a.c_str());
		return out.title.length() > 0 || out.album.length() > 0;
	}

	static void trim_ascii(pfc::string8& s) {
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

	static pfc::string8 sanitize_latin(const char* in) {
		pfc::string8 out;
		bool prev_space = false;
		for (const char* p = in; *p; ++p) {
			const char c = *p;
			if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
				out.add_char((char)tolower((unsigned char)c));
				prev_space = false;
			} else if (c >= '0' && c <= '9') {
				out.add_char(c);
				prev_space = false;
			} else if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '.') {
				if (!prev_space && out.length() > 0) {
					out.add_char(' ');
					prev_space = true;
				}
			}
		}
		trim_ascii(out);
		return out;
	}

	static pfc::string8 replace_token(pfc::string8 src, const char* token, const char* value) {
		pfc::string8 out;
		const char* p = src.c_str();
		const size_t tokenLen = strlen(token);
		while (*p) {
			const char* hit = strstr(p, token);
			if (!hit) {
				out.add_string(p);
				break;
			}
			out.add_string(p, hit - p);
			out.add_string(value);
			p = hit + tokenLen;
		}
		return out;
	}

	static pfc::string8 json_escape(const char* in) {
		pfc::string8 out;
		for (const char* p = in; *p; ++p) {
			const unsigned char b = (unsigned char)*p;
			switch (b) {
			case '\\': out.add_string("\\\\"); break;
			case '"': out.add_string("\\\""); break;
			case '\n': out.add_string("\\n"); break;
			case '\r': out.add_string("\\r"); break;
			case '\t': out.add_string("\\t"); break;
			default:
				if (b < 0x20) {
					// Control characters -> space
					out.add_char(' ');
				} else if (b < 0x80) {
					out.add_char((char)b);
				} else {
					out.add_string((const char*)p, 1);
				}
				break;
			}
		}
		return out;
	}

	static bool parse_response_for_latin(const pfc::string8& response, latin_record& out) {
		pfc::string8 assistantContent;
		if (extract_assistant_content(response.c_str(), assistantContent)) {
			return parse_latin_lines(assistantContent, out);
		}

		pfc::string8 content;
		if (json_find_string_value(response.c_str(), "content", content)) {
			return parse_latin_lines(content, out);
		}

		// If the response looks like JSON but we didn't parse content, do not fallback to raw text.
		if (strstr(response.c_str(), "\"choices\"") != nullptr) return false;

		return parse_latin_lines(response, out);
	}

	static int parse_status_code(const char* statusLine) {
		if (!statusLine) return 0;
		// Expected: "HTTP/1.1 200 OK"
		const char* p = strchr(statusLine, ' ');
		if (!p) return 0;
		++p;
		if (p[0] < '0' || p[0] > '9') return 0;
		int code = 0;
		for (int i = 0; i < 3 && p[i] >= '0' && p[i] <= '9'; ++i) {
			code = code * 10 + (p[i] - '0');
		}
		return code;
	}

	static void append_body_snippet(pfc::string8& out, const pfc::string8& body, size_t maxLen = 2048) {
		if (body.length() <= maxLen) {
			out << "Body:\r\n" << body;
			return;
		}
		pfc::string8 tmp;
		tmp.add_string(body.c_str(), maxLen);
		out << "Body (first " << (unsigned)maxLen << " bytes):\r\n" << tmp;
	}

	// Core network request:
	// - Builds the JSON payload for the LLM API.
	// - Sends HTTP POST.
	// - Parses response to extract latinized title/album.
	// - Returns detailed error info for UI debugging.
	static bool request_latinized_ex(const char* title, const char* album, latin_record& out, abort_callback& abort, pfc::string8* outError, pfc::string8* outRaw) {
		using namespace foo_latinize;

		const auto& apiUrl = cfg_api_url.get();
		if (apiUrl.length() == 0) {
			if (outError) *outError = "API URL is empty.";
			return false;
		}

		pfc::string8 prompt = cfg_prompt.get();
		prompt = replace_token(prompt, "{title}", title ? title : "");
		prompt = replace_token(prompt, "{album}", album ? album : "");

		pfc::string8 body;
		body << "{";
		body << "\"model\":\"" << json_escape(cfg_api_model.get().c_str()) << "\",";
		body << "\"messages\":[";
		body << "{\"role\":\"system\",\"content\":\"You produce latinized ASCII-only names.\"},";
		body << "{\"role\":\"user\",\"content\":\"" << json_escape(prompt.c_str()) << "\"}";
		body << "],";
		body << "\"stream\":false,";
		body << "\"temperature\":0.2";
		body << "}";

		// HTTP request setup.
		http_request::ptr baseReq = static_api_ptr_t<http_client>()->create_request("POST");
		http_request_post_v2::ptr req;
		req ^= baseReq;
		req->add_header("Content-Type", "application/json");
		const auto& key = cfg_api_key.get();
		if (key.length() > 0) req->add_header("Authorization", PFC_string_formatter() << "Bearer " << key);
		req->set_post_data(body.get_ptr(), body.length(), "application/json");

		try {
			file::ptr responseFile = req->run_ex(apiUrl.c_str(), abort);

			pfc::string8 response;
			{
				t_uint8 buffer[4096];
				while (true) {
					const t_size got = responseFile->read(buffer, sizeof(buffer), abort);
					if (got == 0) break;
					response.add_string((const char*)buffer, got);
				}
			}
			if (outRaw) {
				pfc::string8 raw;
				raw << "Request URL:\r\n" << apiUrl << "\r\n\r\n";
				raw << "Resolved Prompt:\r\n" << prompt << "\r\n\r\n";
				raw << "Request Body:\r\n" << body << "\r\n\r\n";
				raw << "Response Body:\r\n" << response;
				*outRaw = raw;
			}

			pfc::string8 statusLine;
			pfc::string8 contentType;
			http_reply::ptr reply;
			reply ^= responseFile;
			if (reply.is_valid()) {
				reply->get_status(statusLine);
				reply->get_http_header("content-type", contentType);
			}
			const int statusCode = parse_status_code(statusLine.c_str());
			if (statusCode < 200 || statusCode >= 300) {
				if (outError) {
					pfc::string8 msg;
					msg << "HTTP error. Status: " << (statusLine.length() ? statusLine : "unknown");
					if (contentType.length() > 0) msg << "\r\nContent-Type: " << contentType;
					if (response.length() > 0) {
						msg << "\r\n";
						append_body_snippet(msg, response);
					}
					*outError = msg;
				}
				return false;
			}

			if (parse_response_for_latin(response, out)) return true;
			if (outError) {
				pfc::string8 msg = "Parsed response but did not find latinized fields.";
				if (response.length() > 0) {
					msg << "\r\n";
					append_body_snippet(msg, response);
				}
				*outError = msg;
			}
			return false;
		} catch (exception_aborted const&) {
			throw;
		} catch (exception_io const&) {
			if (outError) *outError = "Network/IO error.";
			return false;
		} catch (std::exception const&) {
			if (outError) *outError = "Unexpected error.";
			return false;
		}
	}

	// Simple wrapper that hides raw/error outputs.
	static bool request_latinized(const char* title, const char* album, latin_record& out, abort_callback& abort) {
		return request_latinized_ex(title, album, out, abort, nullptr, nullptr);
	}

	// Exposes cached latinized values as title formatting fields:
	// %foo_latin_title% and %foo_latin_album%
	class metadb_display_field_provider_impl : public metadb_display_field_provider_v2 {
	public:
		t_uint32 get_field_count() override { return 2; }
		void get_field_name(t_uint32 index, pfc::string_base& out) override {
			switch (index) {
			case 0: out = "foo_latin_title"; break;
			case 1: out = "foo_latin_album"; break;
			default: uBugCheck();
			}
		}
		bool process_field(t_uint32 index, metadb_handle* handle, titleformat_text_out* out) override {
			return process_field_v2(index, handle, handle->query_v2_(), out);
		}
		bool process_field_v2(t_uint32 index, metadb_handle* handle, metadb_v2::rec_t const& metarec, titleformat_text_out* out) override {
			if (!metarec.info.is_valid()) return false;
			g_db.ensure_loaded();

			const auto& info = metarec.info->info();
			const auto trackHash = get_keyer().hash_track(info, handle->get_location());
			latin_record rec;
			if (g_db.get_track(trackHash, rec)) {
				if (index == 0 && rec.title.length() > 0) {
					out->write(titleformat_inputtypes::meta, rec.title.c_str());
					return true;
				}
			}

			if (index == 1) {
				const auto albumHash = get_keyer().hash_album(info, handle->get_location());
				pfc::string8 album;
				if (g_db.get_album(albumHash, album) && album.length() > 0) {
					out->write(titleformat_inputtypes::meta, album.c_str());
					return true;
				}
				if (rec.album.length() > 0) {
					out->write(titleformat_inputtypes::meta, rec.album.c_str());
					return true;
				}
			}

			return false;
		}
	};

	static service_factory_single_t<metadb_display_field_provider_impl> g_display_field_factory;

	// Load cache once config is read.
	class init_stage_callback_impl : public init_stage_callback {
	public:
		void on_init_stage(t_uint32 stage) override {
			if (stage == init_stages::after_config_read) {
				g_db.ensure_loaded();
			}
		}
	};
	static service_factory_single_t<init_stage_callback_impl> g_init_stage_callback_impl;

	// Save cache on quit.
	class initquit_impl : public initquit {
	public:
		void on_quit() override {
			g_db.save_if_dirty();
		}
	};
	static service_factory_single_t<initquit_impl> g_initquit_impl;
}

namespace foo_latinize {
	void get_cache_snapshot(pfc::list_t<cache_entry>& out) {
		g_db.ensure_loaded();
		g_db.snapshot(out);
	}

	bool update_cache_entry(const cache_entry& entry) {
		g_db.ensure_loaded();
		const bool changed = g_db.update_entry(entry);
		if (changed) g_db.save_if_dirty();
		return changed;
	}

	bool delete_cache_entry(bool is_track, metadb_index_hash hash) {
		g_db.ensure_loaded();
		const bool changed = g_db.delete_entry(is_track, hash);
		if (changed) g_db.save_if_dirty();
		return changed;
	}

	void clear_cache() {
		g_db.ensure_loaded();
		g_db.clear_all();
		g_db.save_if_dirty();
	}

	bool test_latinize(const char* title, const char* album, pfc::string8& outTitle, pfc::string8& outAlbum, pfc::string8& outError, pfc::string8& outRaw) {
		// One-shot request used by the "Test" preferences page.
		latin_record rec;
		abort_callback_dummy abort;
		try {
			if (!request_latinized_ex(title, album, rec, abort, &outError, &outRaw)) {
				if (outError.length() == 0) outError = "Request failed.";
				return false;
			}
		} catch (exception_aborted const&) {
			outError = "Aborted.";
			return false;
		} catch (exception_io const&) {
			outError = "Network or IO error.";
			return false;
		} catch (std::exception const& e) {
			outError = e.what();
			return false;
		}
		outTitle = rec.title;
		outAlbum = rec.album;
		return true;
	}

	void RunLatinize(metadb_handle_list_cref data, fb2k::hwnd_t parent) {
		// Main entry point called from the context menu.
		// Runs a background task to keep UI responsive.
		if (data.get_count() == 0) return;
		g_db.ensure_loaded();

		auto items = std::make_shared<metadb_handle_list>(data);
		auto changed = std::make_shared<metadb_handle_list>();

		auto task = threaded_process_callback_lambda::create(
			[](threaded_process_callback::ctx_t) {},
			[items, changed](threaded_process_status& status, abort_callback& abort) {
				// Worker thread: compute missing latinized values and update cache.
				const t_size count = items->get_count();
				status.set_progress(0, count);

				for (t_size i = 0; i < count; ++i) {
					abort.check();
					status.set_progress(i, count);

					metadb_handle_ptr handle = (*items)[i];
					metadb_info_container::ptr infoContainer;
					if (!handle->get_info_ref(infoContainer)) continue;

					const file_info& info = infoContainer->info();
					const char* title = info.meta_get("TITLE", 0);
					const char* album = info.meta_get("ALBUM", 0);

					const auto trackHash = get_keyer().hash_track(info, handle->get_location());
					const auto albumHash = get_keyer().hash_album(info, handle->get_location());
					pfc::string8 cachedAlbum;
					const bool haveAlbum = g_db.get_album(albumHash, cachedAlbum) && cachedAlbum.length() > 0;

					latin_record rec;
					const bool have = g_db.get_track(trackHash, rec);
					if (have && rec.title.length() > 0 && (rec.album.length() > 0 || haveAlbum)) {
						// Ensure album cache is populated from track record if needed
						if (!haveAlbum && rec.album.length() > 0) g_db.set_album(albumHash, rec.album);
						continue;
					}

					latin_record fresh;
					if (!request_latinized(title, album, fresh, abort)) continue;

					if (fresh.title.length() == 0 && fresh.album.length() == 0) continue;

					// Prefer existing album cache to keep album naming consistent within the same album
					if (haveAlbum) {
						fresh.album = cachedAlbum;
					}

					g_db.set_track(trackHash, fresh);
					if (!haveAlbum && fresh.album.length() > 0) g_db.set_album(albumHash, fresh.album);

					changed->add_item(handle);
				}
				g_db.save_if_dirty();
			},
			[changed](threaded_process_callback::ctx_t, bool) {
				// UI thread: refresh metadata for changed items.
				if (changed->get_count() == 0) return;
				static_api_ptr_t<metadb_io>()->dispatch_refresh(*changed);
				FB2K_console_formatter() << "[foo_sample latinize] Updated " << changed->get_count() << " item(s).";
			}
		);

		threaded_process::g_run_modeless(task,
			threaded_process::flag_show_abort | threaded_process::flag_show_delayed | threaded_process::flag_no_focus,
			parent, "Latinize names");
	}

	void ClearLatinizeAll(metadb_handle_list_cref data, fb2k::hwnd_t parent) {
		// Removes both title and album latinized values for selected items.
		if (data.get_count() == 0) return;
		g_db.ensure_loaded();

		auto items = std::make_shared<metadb_handle_list>(data);
		auto changed = std::make_shared<metadb_handle_list>();

		auto task = threaded_process_callback_lambda::create(
			[](threaded_process_callback::ctx_t) {},
			[items, changed](threaded_process_status& status, abort_callback& abort) {
				const t_size count = items->get_count();
				status.set_progress(0, count);

				for (t_size i = 0; i < count; ++i) {
					abort.check();
					status.set_progress(i, count);

					metadb_handle_ptr handle = (*items)[i];
					metadb_info_container::ptr infoContainer;
					if (!handle->get_info_ref(infoContainer)) continue;

					const file_info& info = infoContainer->info();
					const auto trackHash = get_keyer().hash_track(info, handle->get_location());
					const auto albumHash = get_keyer().hash_album(info, handle->get_location());

					bool anyChanged = false;
					if (g_db.delete_entry(true, trackHash)) anyChanged = true;
					if (g_db.delete_entry(false, albumHash)) anyChanged = true;

					if (anyChanged) changed->add_item(handle);
				}
				g_db.save_if_dirty();
			},
			[changed](threaded_process_callback::ctx_t, bool) {
				if (changed->get_count() == 0) return;
				static_api_ptr_t<metadb_io>()->dispatch_refresh(*changed);
				FB2K_console_formatter() << "[foo_sample latinize] Cleared " << changed->get_count() << " item(s).";
			}
		);

		threaded_process::g_run_modeless(task,
			threaded_process::flag_show_abort | threaded_process::flag_show_delayed | threaded_process::flag_no_focus,
			parent, "Clear latinized names");
	}

	void ClearLatinizeTitleOnly(metadb_handle_list_cref data, fb2k::hwnd_t parent) {
		if (data.get_count() == 0) return;
		g_db.ensure_loaded();

		auto items = std::make_shared<metadb_handle_list>(data);
		auto changed = std::make_shared<metadb_handle_list>();

		auto task = threaded_process_callback_lambda::create(
			[](threaded_process_callback::ctx_t) {},
			[items, changed](threaded_process_status& status, abort_callback& abort) {
				const t_size count = items->get_count();
				status.set_progress(0, count);

				for (t_size i = 0; i < count; ++i) {
					abort.check();
					status.set_progress(i, count);

					metadb_handle_ptr handle = (*items)[i];
					metadb_info_container::ptr infoContainer;
					if (!handle->get_info_ref(infoContainer)) continue;

					const file_info& info = infoContainer->info();
					const auto trackHash = get_keyer().hash_track(info, handle->get_location());

					latin_record rec;
					if (!g_db.get_track(trackHash, rec)) continue;
					if (rec.title.length() == 0) continue;
					rec.title.reset();
					g_db.set_track(trackHash, rec);
					changed->add_item(handle);
				}
				g_db.save_if_dirty();
			},
			[changed](threaded_process_callback::ctx_t, bool) {
				if (changed->get_count() == 0) return;
				static_api_ptr_t<metadb_io>()->dispatch_refresh(*changed);
				FB2K_console_formatter() << "[foo_sample latinize] Cleared titles for " << changed->get_count() << " item(s).";
			}
		);

		threaded_process::g_run_modeless(task,
			threaded_process::flag_show_abort | threaded_process::flag_show_delayed | threaded_process::flag_no_focus,
			parent, "Clear latinized titles");
	}

	void ClearLatinizeAlbumOnly(metadb_handle_list_cref data, fb2k::hwnd_t parent) {
		if (data.get_count() == 0) return;
		g_db.ensure_loaded();

		auto items = std::make_shared<metadb_handle_list>(data);
		auto changed = std::make_shared<metadb_handle_list>();

		auto task = threaded_process_callback_lambda::create(
			[](threaded_process_callback::ctx_t) {},
			[items, changed](threaded_process_status& status, abort_callback& abort) {
				const t_size count = items->get_count();
				status.set_progress(0, count);

				for (t_size i = 0; i < count; ++i) {
					abort.check();
					status.set_progress(i, count);

					metadb_handle_ptr handle = (*items)[i];
					metadb_info_container::ptr infoContainer;
					if (!handle->get_info_ref(infoContainer)) continue;

					const file_info& info = infoContainer->info();
					const auto trackHash = get_keyer().hash_track(info, handle->get_location());
					const auto albumHash = get_keyer().hash_album(info, handle->get_location());

					bool anyChanged = false;
					latin_record rec;
					if (g_db.get_track(trackHash, rec)) {
						if (rec.album.length() > 0) {
							rec.album.reset();
							g_db.set_track(trackHash, rec);
							anyChanged = true;
						}
					}

					if (g_db.delete_entry(false, albumHash)) {
						anyChanged = true;
					}

					if (anyChanged) changed->add_item(handle);
				}
				g_db.save_if_dirty();
			},
			[changed](threaded_process_callback::ctx_t, bool) {
				if (changed->get_count() == 0) return;
				static_api_ptr_t<metadb_io>()->dispatch_refresh(*changed);
				FB2K_console_formatter() << "[foo_sample latinize] Cleared albums for " << changed->get_count() << " item(s).";
			}
		);

		threaded_process::g_run_modeless(task,
			threaded_process::flag_show_abort | threaded_process::flag_show_delayed | threaded_process::flag_no_focus,
			parent, "Clear latinized albums");
	}
}
