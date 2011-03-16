#define MYVERSION "0.1"

#define _WIN32_WINNT 0x0600

#include <foobar2000.h>
#include <libspotify/api.h>

#include "../helpers/dropdown_helper.h"
#include <functional>
#include <shlobj.h>

#include <stdint.h>
#include <stdlib.h>

#include "SpotifySession.h"

extern "C" {
	extern const uint8_t g_appkey[];
	extern const size_t g_appkey_size;
}

SpotifySession ss;

volatile bool failed = false;

struct Gentry {
	void *data;
    size_t size;
	int sampleRate;
	int channels;
};

struct CriticalSection {
	CRITICAL_SECTION cs;
	CriticalSection() {
		InitializeCriticalSection(&cs);
	}

	~CriticalSection() {
		DeleteCriticalSection(&cs);
	}
};

struct LockedCS {
	CRITICAL_SECTION &cs;
	LockedCS(CriticalSection &o) : cs(o.cs) {
		EnterCriticalSection(&cs);
	}

	~LockedCS() {
		LeaveCriticalSection(&cs);
	}
};

struct Buffer {

	size_t entries;
	size_t ptr;

	static const size_t MAX_ENTRIES = 255;

	Gentry *entry[MAX_ENTRIES];

	CONDITION_VARIABLE bufferNotEmpty;
	CriticalSection bufferLock;

	Buffer() : entries(0), ptr(0) {
		InitializeConditionVariable(&bufferNotEmpty);
	}

	~Buffer() {
		flush();
	}

	void add(void *data, size_t size, int sampleRate, int channels) {
		Gentry *e = new Gentry;
		e->data = data;
		e->size = size;
		e->sampleRate = sampleRate;
		e->channels = channels;

		{
			LockedCS lock(bufferLock);
			entry[(ptr + entries) % MAX_ENTRIES] = e;
			++entries;
		}
		WakeConditionVariable(&bufferNotEmpty);
	}

	bool isFull() {
		return entries >= MAX_ENTRIES;
	}

	void flush() {
		while (entries > 0)
			free(take());
	}

	Gentry *take() {
		LockedCS lock(bufferLock);
		while (entries == 0) {
			SleepConditionVariableCS(&bufferNotEmpty, &bufferLock.cs, INFINITE);
		}

		Gentry *e = entry[ptr++];
		--entries;
		if (MAX_ENTRIES == ptr)
			ptr = 0;

		return e;
	}

	void free(Gentry *e) {
		delete[] e->data;
		delete e;
	}
};

// {FDE57F91-397C-45F6-B907-A40E378DDB7A}
static const GUID spotifyUsernameGuid = 
{ 0xfde57f91, 0x397c, 0x45f6, { 0xb9, 0x7, 0xa4, 0xe, 0x37, 0x8d, 0xdb, 0x7a } };

// {543780A4-2EC2-4EFE-966E-4AC491ACADBA}
static const GUID spotifyPasswordGuid = 
{ 0x543780a4, 0x2ec2, 0x4efe, { 0x96, 0x6e, 0x4a, 0xc4, 0x91, 0xac, 0xad, 0xba } };

static advconfig_string_factory_MT spotifyUsername("Spotify Username", spotifyUsernameGuid, advconfig_entry::guid_root, 1, "", 0);
static advconfig_string_factory_MT spotifyPassword("Spotify Password (plaintext lol)", spotifyPasswordGuid, advconfig_entry::guid_root, 2, "", 0);

class InputSpotify
{
	t_filestats m_stats;

	std::string url;
	sp_track *t;

	int channels;
	int sampleRate;

public:

	void open( service_ptr_t<file> m_file, const char * p_path, t_input_open_reason p_reason, abort_callback & p_abort )
	{
		if ( p_reason == input_open_info_write ) throw exception_io_data();
		url = p_path;

		ss.get();

		{
			LockedCS lock(ss.getSpotifyCS());

			sp_link *link = sp_link_create_from_string(p_path);
			if (NULL == link)
				throw exception_io_data("couldn't parse url");

			t = sp_link_as_track(link);
			if (NULL == t)
				throw exception_io_data("url not a track");
		}

		while (true) {
			{
				LockedCS lock(ss.getSpotifyCS());

				const sp_error e = sp_track_error(t);
				if (SP_ERROR_OK == e)
					break;
				if (SP_ERROR_IS_LOADING != e)
					throw exception_io_data(sp_error_message(e));
			}

			Sleep(50);
			p_abort.check();
		}
	}

	void get_info( file_info & p_info, abort_callback & p_abort )
	{
		LockedCS lock(ss.getSpotifyCS());
		p_info.set_length(sp_track_duration(t)/1000.0);
		p_info.meta_add("ARTIST", sp_artist_name(sp_track_artist(t, 0)));
		p_info.meta_add("ALBUM", sp_album_name(sp_track_album(t)));
		p_info.meta_add("TITLE", sp_track_name(t));
		p_info.meta_add("URL", url.c_str());
	}

	t_filestats get_file_stats( abort_callback & p_abort )
	{
		return m_stats;
	}

	void decode_initialize( unsigned p_flags, abort_callback & p_abort )
	{
		ss.buf.flush();
		sp_session *sess = ss.get();

		LockedCS lock(ss.getSpotifyCS());
		sp_session_player_load(sess, t);
		sp_session_player_play(sess, 1);
	}

	bool decode_run( audio_chunk & p_chunk, abort_callback & p_abort )
	{
		if (failed)
			throw exception_io_data("failed");

		Gentry *e = ss.buf.take();

		if (NULL == e->data) {
			ss.buf.free(e);
			return false;
		}

		p_chunk.set_data_fixedpoint(
			e->data,
			e->size,
			e->sampleRate,
			e->channels,
			16,
			audio_chunk::channel_config_stereo);

		channels = e->channels;
		sampleRate = e->sampleRate;

		ss.buf.free(e);

		return true;
	}

	void decode_seek( double p_seconds,abort_callback & p_abort )
	{
		ss.buf.flush();
		sp_session *sess = ss.get();
		LockedCS lock(ss.getSpotifyCS());
		sp_session_player_seek(sess, static_cast<int>(p_seconds*1000));
	}

	bool decode_can_seek()
	{
		return true;
	}

	bool decode_get_dynamic_info( file_info & p_out, double & p_timestamp_delta )
	{
		p_out.info_set_int("CHANNELS", channels);
		p_out.info_set_int("SAMPLERATE", sampleRate);
		return true;
	}

	bool decode_get_dynamic_info_track( file_info & p_out, double & p_timestamp_delta )
	{
		return false;
	}

	void decode_on_idle( abort_callback & p_abort ) { }

	void retag( const file_info & p_info, abort_callback & p_abort )
	{
		throw exception_io_data();
	}

	static bool g_is_our_content_type( const char * p_content_type )
	{
		return false;
	}

	static bool g_is_our_path( const char * p_full_path, const char * p_extension )
	{
		return !strncmp( p_full_path, "spotify:", strlen("spotify:") );
	}
};

static input_singletrack_factory_t< InputSpotify > inputFactorySpotify;

DECLARE_COMPONENT_VERSION("Spotify Decoder", MYVERSION, "Support for spotify: urls.");
