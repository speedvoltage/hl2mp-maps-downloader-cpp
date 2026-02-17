#include "main.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <regex>
#include <thread>

#include <bzlib.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

using nlohmann::json;

void LiveLog::push( std::string s ) {
    std::lock_guard lk( mtx );
    lines.emplace_back( std::move( s ) );
    if ( lines.size() > 800 ) lines.erase( lines.begin(), lines.begin() + 200 );
}
void LiveLog::fail( std::string s ) {
    std::lock_guard lk( mtx );
    failures.emplace_back( std::move( s ) );
    if ( failures.size() > 200 ) failures.erase( failures.begin(), failures.begin() + 50 );
}

static std::string trim( std::string s ) {
    auto issp = []( unsigned char c ) { return std::isspace( c ) != 0; };
    while ( !s.empty() && issp( ( unsigned char ) s.front() ) ) s.erase( s.begin() );
    while ( !s.empty() && issp( ( unsigned char ) s.back() ) ) s.pop_back();
    return s;
}

static std::string lower_copy( std::string s ) {
    std::transform( s.begin(), s.end(), s.begin(), []( unsigned char c ) { return ( char ) std::tolower( c ); } );
    return s;
}

static std::vector<std::string> split_csv_terms( std::string s ) {
    std::vector<std::string> out;
    s = trim( std::move( s ) );
    if ( s.empty() ) return out;

    std::string cur;
    for ( char ch : s ) {
        if ( ch == ',' ) {
            cur = trim( cur );
            if ( !cur.empty() ) out.push_back( lower_copy( cur ) );
            cur.clear();
        }
        else {
            cur.push_back( ch );
        }
    }
    cur = trim( cur );
    if ( !cur.empty() ) out.push_back( lower_copy( cur ) );
    return out;
}

static bool passes_filters( const std::string &filename,
    const std::vector<std::string> &includes,
    const std::vector<std::string> &excludes ) {
    auto name = lower_copy( filename );

    if ( !includes.empty() ) {
        bool any = false;
        for ( auto &t : includes ) {
            if ( t.empty() ) continue;
            if ( name.find( t ) != std::string::npos ) { any = true; break; }
        }
        if ( !any ) return false;
    }

    if ( !excludes.empty() ) {
        for ( auto &t : excludes ) {
            if ( t.empty() ) continue;
            if ( name.find( t ) != std::string::npos ) return false;
        }
    }

    return true;
}

fs::path app_dir() {
#ifdef _WIN32
    wchar_t *wbuf = nullptr;
    size_t sz = 0;
    _wdupenv_s( &wbuf, &sz, L"CD" );
    if ( wbuf ) { free( wbuf ); }
#endif
    return fs::current_path();
}

fs::path sources_path() { return app_dir() / "sources.json"; }
fs::path settings_path() { return app_dir() / "settings.json"; }
fs::path logs_dir() { return app_dir() / "logs"; }

static void ensure_logs_dir( LiveLog &log ) {
    std::error_code ec;
    fs::create_directories( logs_dir(), ec );
    if ( ec ) log.push( std::string( "[!] Failed to create logs dir: " ) + ec.message() );
}

static void write_session_log( const LiveLog &log ) {
    std::error_code ec;
    fs::create_directories( logs_dir(), ec );
    if ( ec ) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t( now );
    std::tm tm{};
#ifdef _WIN32
    localtime_s( &tm, &t );
#else
    localtime_r( &t, &tm );
#endif
    char buf[ 64 ];
    std::snprintf( buf, sizeof( buf ), "session_%04d%02d%02d_%02d%02d%02d.log",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec );

    std::ofstream f( logs_dir() / buf, std::ios::binary );
    if ( !f ) return;

    for ( auto &s : log.lines ) f << s << "\n";
    if ( !log.failures.empty() ) {
        f << "\n--- FAILURES ---\n";
        for ( auto &s : log.failures ) f << s << "\n";
    }
}

std::string normalize_maps_url( std::string url ) {
    url = trim( url );
    if ( url.empty() ) return url;

    /*if ( url.find( "://" ) == std::string::npos )
        url = "http://" + url;*/

    if ( url.back() != '/' )
        url.push_back( '/' );

    return url;
}

std::vector<SourceEntry> load_sources( LiveLog &log ) {
    std::vector<SourceEntry> out;
    auto p = sources_path();
    if ( !fs::exists( p ) ) {
        json j;
        j[ "sources" ] = json::array();
        std::ofstream f( p );
        f << j.dump( 2 );
        log.push( "[i] Created sources.json (empty)." );
        return out;
    }
    try {
        std::ifstream f( p );
        json j;
        f >> j;
        for ( auto &it : j.value( "sources", json::array() ) ) {
            SourceEntry s;
            s.url = it.value( "url", "" );
            s.enabled = it.value( "enabled", true );
            s.last_latency_ms = it.value( "last_latency_ms", -1 );
            s.last_ok = it.value( "last_ok", false );
            s.url = normalize_maps_url( s.url );
            if ( !s.url.empty() ) out.push_back( std::move( s ) );
        }
    }
    catch ( ... ) {
        log.push( "[!] Failed to parse sources.json (will treat as empty)." );
    }
    return out;
}

void save_sources( const std::vector<SourceEntry> &src, LiveLog &log ) {
    json j;
    j[ "sources" ] = json::array();
    for ( auto &s : src ) {
        json it;
        it[ "url" ] = s.url;
        it[ "enabled" ] = s.enabled;
        it[ "last_latency_ms" ] = s.last_latency_ms;
        it[ "last_ok" ] = s.last_ok;
        j[ "sources" ].push_back( it );
    }
    try {
        std::ofstream f( sources_path() );
        f << j.dump( 2 );
    }
    catch ( ... ) {
        log.push( "[!] Failed to write sources.json" );
    }
}

static int default_threads() {
    auto hc = std::thread::hardware_concurrency();
    if ( hc == 0 ) return 4;
    return std::max( 1u, hc / 2 );
}

Settings load_settings( LiveLog &log ) {
    Settings s;
    s.threads = default_threads();
    auto p = settings_path();
    if ( !fs::exists( p ) ) return s;
    try {
        std::ifstream f( p );
        json j;
        f >> j;
        s.hl2mp_path = j.value( "hl2mp_path", "" );
        s.threads = j.value( "threads", s.threads );
        s.decompress = j.value( "decompress", false );
        s.delete_bz2 = j.value( "delete_bz2", false );
        s.index_timeout_ms = j.value( "index_timeout_ms", 8000 );
        s.head_timeout_ms = j.value( "head_timeout_ms", 5000 );
        s.dl_timeout_ms = j.value( "dl_timeout_ms", 30000 );
        s.retries = j.value( "retries", 3 );
        s.include_filters = j.value( "include_filters", "" );
        s.exclude_filters = j.value( "exclude_filters", "" );
    }
    catch ( ... ) {
        log.push( "[!] Failed to parse settings.json (defaults used)." );
    }
    return s;
}

void save_settings( const Settings &s, LiveLog &log ) {
    json j;
    j[ "hl2mp_path" ] = s.hl2mp_path.string();
    j[ "threads" ] = s.threads;
    j[ "decompress" ] = s.decompress;
    j[ "delete_bz2" ] = s.delete_bz2;
    j[ "index_timeout_ms" ] = s.index_timeout_ms;
    j[ "head_timeout_ms" ] = s.head_timeout_ms;
    j[ "dl_timeout_ms" ] = s.dl_timeout_ms;
    j[ "retries" ] = s.retries;
    j[ "include_filters" ] = s.include_filters;
    j[ "exclude_filters" ] = s.exclude_filters;

    try {
        std::ofstream f( settings_path() );
        f << j.dump( 2 );
    }
    catch ( ... ) {
        log.push( "[!] Failed to write settings.json" );
    }
}

static std::vector<fs::path> parse_libraryfolders_vdf( const fs::path &steamapps ) {
    std::vector<fs::path> out;
    auto vdf = steamapps / "libraryfolders.vdf";
    if ( !fs::exists( vdf ) ) return out;

    std::ifstream f( vdf, std::ios::binary );
    if ( !f ) return out;
    std::string txt( ( std::istreambuf_iterator<char>( f ) ), std::istreambuf_iterator<char>() );

    std::regex re( R"VDF("path"\s*"([^"]+)")VDF", std::regex::icase );
    for ( std::sregex_iterator it( txt.begin(), txt.end(), re ), end; it != end; ++it ) {
        auto p = ( *it )[ 1 ].str();
#ifdef _WIN32
        std::replace( p.begin(), p.end(), '\\', '/' );
#endif
        out.push_back( fs::path( p ) / "steamapps" );
    }
    return out;
}

std::optional<fs::path> find_hl2mp_dir() {
    std::vector<fs::path> candidates;

#ifdef _WIN32
    const char *pf86 = std::getenv( "ProgramFiles(x86)" );
    const char *pf = std::getenv( "ProgramFiles" );
    if ( pf86 ) candidates.push_back( fs::path( pf86 ) / "Steam" / "steamapps" );
    if ( pf ) candidates.push_back( fs::path( pf ) / "Steam" / "steamapps" );
#else
    candidates.push_back( fs::path( getenv( "HOME" ) ? getenv( "HOME" ) : "" ) / ".steam/steam/steamapps" );
    candidates.push_back( fs::path( getenv( "HOME" ) ? getenv( "HOME" ) : "" ) / ".local/share/Steam/steamapps" );
    candidates.push_back( fs::path( getenv( "HOME" ) ? getenv( "HOME" ) : "" ) / "Library/Application Support/Steam/steamapps" );
#endif

    std::vector<fs::path> steamapps_all;
    for ( auto &root : candidates ) {
        steamapps_all.push_back( root );
        auto libs = parse_libraryfolders_vdf( root );
        steamapps_all.insert( steamapps_all.end(), libs.begin(), libs.end() );
    }

    for ( auto &steamapps : steamapps_all ) {
        auto hl2mp = steamapps / "common" / "Half-Life 2 Deathmatch" / "hl2mp";
        if ( fs::exists( hl2mp / "maps" ) || fs::exists( hl2mp / "download" ) ) {
            return fs::weakly_canonical( hl2mp );
        }
    }
    return std::nullopt;
}

static size_t curl_write_cb( void *contents, size_t size, size_t nmemb, void *userp ) {
    auto *s = ( std::string * ) userp;
    s->append( ( char * ) contents, size * nmemb );
    return size * nmemb;
}

static CURL *curl_easy() {
    CURL *c = curl_easy_init();
    curl_easy_setopt( c, CURLOPT_FOLLOWLOCATION, 1L );
    curl_easy_setopt( c, CURLOPT_NOSIGNAL, 1L );
    curl_easy_setopt( c, CURLOPT_USERAGENT, "hl2mp-maps-downloader/0.1" );
    return c;
}

HttpResult http_get_text( const std::string &url, int timeout_ms ) {
    HttpResult r;
    CURL *c = curl_easy();
    std::string body;

    curl_easy_setopt( c, CURLOPT_URL, url.c_str() );
    curl_easy_setopt( c, CURLOPT_TIMEOUT_MS, ( long ) timeout_ms );
    curl_easy_setopt( c, CURLOPT_WRITEFUNCTION, curl_write_cb );
    curl_easy_setopt( c, CURLOPT_WRITEDATA, &body );

    auto t0 = std::chrono::steady_clock::now();
    auto code = curl_easy_perform( c );
    auto t1 = std::chrono::steady_clock::now();
    r.latency_ms = ( int ) std::chrono::duration_cast< std::chrono::milliseconds >( t1 - t0 ).count();

    if ( code != CURLE_OK ) {
        r.err = curl_easy_strerror( code );
    }
    else {
        long status = 0;
        curl_easy_getinfo( c, CURLINFO_RESPONSE_CODE, &status );
        r.status = status;
        r.body = std::move( body );
    }
    curl_easy_cleanup( c );
    return r;
}

HttpResult http_head( const std::string &url, int timeout_ms ) {
    HttpResult r;
    CURL *c = curl_easy();
    curl_easy_setopt( c, CURLOPT_URL, url.c_str() );
    curl_easy_setopt( c, CURLOPT_TIMEOUT_MS, ( long ) timeout_ms );
    curl_easy_setopt( c, CURLOPT_NOBODY, 1L );

    auto t0 = std::chrono::steady_clock::now();
    auto code = curl_easy_perform( c );
    auto t1 = std::chrono::steady_clock::now();
    r.latency_ms = ( int ) std::chrono::duration_cast< std::chrono::milliseconds >( t1 - t0 ).count();

    if ( code != CURLE_OK ) {
        r.err = curl_easy_strerror( code );
    }
    else {
        long status = 0;
        curl_easy_getinfo( c, CURLINFO_RESPONSE_CODE, &status );
        r.status = status;
    }
    curl_easy_cleanup( c );
    return r;
}

static std::string url_join( const std::string &base, const std::string &rel ) {
    if ( rel.find( "http://" ) == 0 || rel.find( "https://" ) == 0 ) return rel;
    if ( base.empty() ) return rel;
    if ( !base.empty() && base.back() == '/' && !rel.empty() && rel.front() == '/' ) return base + rel.substr( 1 );
    if ( !base.empty() && base.back() != '/' && !rel.empty() && rel.front() != '/' ) return base + "/" + rel;
    return base + rel;
}

std::vector<std::string> extract_map_links_from_index_html( const std::string &base_url, const std::string &html ) {
    std::vector<std::string> out;
    std::regex href_re( R"(href\s*=\s*["']([^"']+)["'])", std::regex::icase );
    for ( std::sregex_iterator it( html.begin(), html.end(), href_re ), end; it != end; ++it ) {
        auto href = ( *it )[ 1 ].str();
        href = trim( href );
        if ( href.empty() ) continue;
        if ( href.back() == '/' ) continue;
        auto low = lower_copy( href );
        if ( !( low.ends_with( ".bsp" ) || low.ends_with( ".bz2" ) ) ) continue;
        out.push_back( url_join( base_url, href ) );
    }
    out.erase( std::unique( out.begin(), out.end() ), out.end() );
    return out;
}

void scan_existing_maps( const fs::path &hl2mp, RunState &rs, LiveLog &log ) {
    rs.existing_files.clear();
    std::vector<fs::path> roots = {
        hl2mp / "maps",
        hl2mp / "download" / "maps"
    };
    for ( auto &root : roots ) {
        if ( !fs::exists( root ) ) continue;
        for ( auto &e : fs::recursive_directory_iterator( root ) ) {
            if ( !e.is_regular_file() ) continue;
            auto name = e.path().filename().string();
            auto ext = lower_copy( e.path().extension().string() );
            if ( ext == ".bsp" || ext == ".bz2" ) rs.existing_files.insert( name );
        }
    }
    log.push( "[i] Existing map files found: " + std::to_string( rs.existing_files.size() ) );
}

static size_t curl_file_write_cb( void *ptr, size_t size, size_t nmemb, void *stream ) {
    auto *f = ( FILE * ) stream;
    return fwrite( ptr, size, nmemb, f );
}

bool download_file( const std::string &url, const fs::path &out_file, int timeout_ms, int retries,
    std::atomic<bool> &cancel, LiveLog &log ) {
    fs::create_directories( out_file.parent_path() );

    auto tmp = out_file;
    tmp += ".part";

    for ( int attempt = 1; attempt <= retries && !cancel.load(); ++attempt ) {
        if ( fs::exists( tmp ) ) { std::error_code ec; fs::remove( tmp, ec ); }

        FILE *fp = nullptr;
#ifdef _WIN32
        fp = _wfopen( tmp.wstring().c_str(), L"wb" );
#else
        fp = fopen( tmp.string().c_str(), "wb" );
#endif
        if ( !fp ) {
            log.fail( "[DL] Failed to open for writing: " + tmp.string() );
            return false;
        }

        CURL *c = curl_easy();
        curl_easy_setopt( c, CURLOPT_URL, url.c_str() );
        curl_easy_setopt( c, CURLOPT_TIMEOUT_MS, ( long ) timeout_ms );
        curl_easy_setopt( c, CURLOPT_WRITEFUNCTION, curl_file_write_cb );
        curl_easy_setopt( c, CURLOPT_WRITEDATA, fp );

        auto code = curl_easy_perform( c );
        fclose( fp );

        long status = 0;
        curl_easy_getinfo( c, CURLINFO_RESPONSE_CODE, &status );
        curl_easy_cleanup( c );

        if ( cancel.load() ) return false;

        if ( code == CURLE_OK && status >= 200 && status < 300 ) {
            std::error_code ec;
            fs::rename( tmp, out_file, ec );
            if ( ec ) {
                fs::copy_file( tmp, out_file, fs::copy_options::overwrite_existing, ec );
                fs::remove( tmp, ec );
            }
            return true;
        }

        std::error_code ec;
        fs::remove( tmp, ec );

        if ( attempt < retries ) {
            log.push( "[Retry " + std::to_string( attempt ) + "/" + std::to_string( retries ) + "] " +
                out_file.filename().string() );
            std::this_thread::sleep_for( std::chrono::milliseconds( 250 ) );
        }
        else {
            log.fail( "[DL] Failed: " + out_file.filename().string() + " (" + url + ")" );
        }
    }
    return false;
}

bool decompress_bz2_to_file( const fs::path &bz2_file, const fs::path &out_file, int retries,
    std::atomic<bool> &cancel, LiveLog &log ) {
    for ( int attempt = 1; attempt <= retries && !cancel.load(); ++attempt ) {
        FILE *in = nullptr;
        FILE *out = nullptr;
#ifdef _WIN32
        in = _wfopen( bz2_file.wstring().c_str(), L"rb" );
        out = _wfopen( out_file.wstring().c_str(), L"wb" );
#else
        in = fopen( bz2_file.string().c_str(), "rb" );
        out = fopen( out_file.string().c_str(), "wb" );
#endif
        if ( !in || !out ) {
            if ( in ) fclose( in );
            if ( out ) fclose( out );
            log.fail( "[BZ2] Open failed: " + bz2_file.filename().string() );
            return false;
        }

        int bzerr = BZ_OK;
        BZFILE *bz = BZ2_bzReadOpen( &bzerr, in, 0, 0, nullptr, 0 );
        if ( !bz || bzerr != BZ_OK ) {
            fclose( in );
            fclose( out );
            log.fail( "[BZ2] ReadOpen failed: " + bz2_file.filename().string() );
            return false;
        }

        char buf[ 1 << 16 ];
        while ( !cancel.load() ) {
            int n = BZ2_bzRead( &bzerr, bz, buf, ( int ) sizeof( buf ) );
            if ( bzerr == BZ_OK || bzerr == BZ_STREAM_END ) {
                if ( n > 0 ) fwrite( buf, 1, ( size_t ) n, out );
                if ( bzerr == BZ_STREAM_END ) break;
            }
            else {
                break;
            }
        }

        BZ2_bzReadClose( &bzerr, bz );
        fclose( in );
        fclose( out );

        if ( cancel.load() ) return false;

        if ( bzerr == BZ_STREAM_END || bzerr == BZ_OK ) return true;

        std::error_code ec;
        fs::remove( out_file, ec );

        if ( attempt == retries ) {
            log.fail( "[BZ2] Failed: " + bz2_file.filename().string() );
            return false;
        }
    }
    return false;
}

struct SourceIndex {
    SourceEntry *src = nullptr;
    std::vector<std::string> links;
};

static void reset_phases( RunState &rs ) {
    rs.indexing.running.store( false );
    rs.indexing.done.store( 0 );
    rs.indexing.total.store( 0 );

    rs.downloading.running.store( false );
    rs.downloading.done.store( 0 );
    rs.downloading.total.store( 0 );

    rs.decompressing.running.store( false );
    rs.decompressing.done.store( 0 );
    rs.decompressing.total.store( 0 );

    rs.deleting.running.store( false );
    rs.deleting.done.store( 0 );
    rs.deleting.total.store( 0 );
}

static std::map<std::string, std::vector<SourceEntry *>> build_availability(
    const std::vector<SourceIndex> &indexed ) {
    std::map<std::string, std::vector<SourceEntry *>> availability;
    for ( auto &si : indexed ) {
        if ( !si.src || !si.src->enabled || !si.src->last_ok ) continue;
        for ( auto &u : si.links ) {
            auto name = fs::path( u ).filename().string();
            availability[ name ].push_back( si.src );
        }
    }
    return availability;
}

static SourceEntry *pick_best_source( const std::vector<SourceEntry *> &srcs ) {
    SourceEntry *best = nullptr;
    for ( auto *ssrc : srcs ) {
        if ( !best ) best = ssrc;
        else {
            int a = ssrc->last_latency_ms >= 0 ? ssrc->last_latency_ms : 1'000'000;
            int b = best->last_latency_ms >= 0 ? best->last_latency_ms : 1'000'000;
            if ( a < b ) best = ssrc;
        }
    }
    return best;
}

static void run_index_only( Settings s, std::vector<SourceEntry> &sources, RunState &rs, LiveLog &log ) {
    rs.cancel.store( false );
    reset_phases( rs );

    if ( s.hl2mp_path.empty() || !fs::exists( s.hl2mp_path ) ) {
        log.fail( "[!] HL2MP path invalid." );
        return;
    }

    scan_existing_maps( s.hl2mp_path, rs, log );

    std::vector<SourceEntry *> enabled;
    for ( auto &src : sources ) if ( src.enabled ) enabled.push_back( &src );
    if ( enabled.empty() ) {
        log.fail( "[!] No enabled sources." );
        return;
    }

    auto includes = split_csv_terms( s.include_filters );
    auto excludes = split_csv_terms( s.exclude_filters );

    rs.indexing.running.store( true );
    rs.indexing.done.store( 0 );
    rs.indexing.total.store( ( int ) enabled.size() );
    log.push( "[i] Indexing sources..." );

    std::mutex idx_mtx;
    std::vector<SourceIndex> indexed;

    auto worker_index = [ & ]( SourceEntry *src ) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = http_get_text( src->url, s.index_timeout_ms );
        auto t1 = std::chrono::steady_clock::now();
        int ms = ( int ) std::chrono::duration_cast< std::chrono::milliseconds >( t1 - t0 ).count();

        src->last_latency_ms = ms;
        src->last_ok = ( r.err.empty() && r.status >= 200 && r.status < 400 );

        SourceIndex si;
        si.src = src;
        if ( src->last_ok ) {
            si.links = extract_map_links_from_index_html( src->url, r.body );
            log.push( "[+] " + src->url + " -> " + std::to_string( si.links.size() ) + " file(s) (" +
                std::to_string( ms ) + "ms)" );
        }
        else {
            log.fail( "[IDX] " + src->url + " failed (" +
                ( r.err.empty() ? ( "HTTP " + std::to_string( r.status ) ) : r.err ) + ")" );
        }

        {
            std::lock_guard lk( idx_mtx );
            indexed.push_back( std::move( si ) );
        }
        rs.indexing.done.fetch_add( 1 );
        };

    std::vector<std::future<void>> futs;
    int threads = std::max( 1, s.threads );
    std::atomic<int> in_flight{ 0 };

    for ( auto *src : enabled ) {
        while ( !rs.cancel.load() ) {
            if ( in_flight.load() < threads ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        if ( rs.cancel.load() ) break;

        in_flight.fetch_add( 1 );
        futs.push_back( std::async( std::launch::async, [ &, src ] {
            worker_index( src );
            in_flight.fetch_sub( 1 );
            } ) );
    }
    for ( auto &f : futs ) f.wait();

    rs.indexing.running.store( false );

    auto availability = build_availability( indexed );

    int remote_unique = ( int ) availability.size();
    int remote_after_filters = 0;
    int already_have = 0;
    int to_download = 0;

    for ( auto &[name, srcs] : availability ) {
        if ( !passes_filters( name, includes, excludes ) ) continue;
        remote_after_filters++;

        if ( rs.existing_files.contains( name ) ) already_have++;
        else to_download++;
    }

    rs.last_remote_unique.store( remote_unique );
    rs.last_remote_after_filters.store( remote_after_filters );
    rs.last_already_have.store( already_have );
    rs.last_to_download.store( to_download );

    log.push( "[i] Index complete." );
    log.push( "[i] Remote unique files: " + std::to_string( remote_unique ) );
    log.push( "[i] After filters: " + std::to_string( remote_after_filters ) );
    log.push( "[i] Already present locally: " + std::to_string( already_have ) );
    log.push( "[i] Would download: " + std::to_string( to_download ) );
}

static void run_pipeline( Settings s, std::vector<SourceEntry> &sources, RunState &rs, LiveLog &log ) {
    rs.cancel.store( false );
    reset_phases( rs );

    if ( s.hl2mp_path.empty() || !fs::exists( s.hl2mp_path ) ) {
        log.fail( "[!] HL2MP path invalid." );
        return;
    }

    auto dl_dir = s.hl2mp_path / "download" / "maps";
    std::error_code ec;
    fs::create_directories( dl_dir, ec );
    if ( ec ) {
        log.fail( "[!] Failed to create download/maps: " + ec.message() );
        return;
    }

    scan_existing_maps( s.hl2mp_path, rs, log );

    std::vector<SourceEntry *> enabled;
    for ( auto &src : sources ) if ( src.enabled ) enabled.push_back( &src );
    if ( enabled.empty() ) {
        log.fail( "[!] No enabled sources." );
        return;
    }

    auto includes = split_csv_terms( s.include_filters );
    auto excludes = split_csv_terms( s.exclude_filters );

    rs.indexing.running.store( true );
    rs.indexing.done.store( 0 );
    rs.indexing.total.store( ( int ) enabled.size() );
    log.push( "[i] Indexing sources..." );

    std::mutex idx_mtx;
    std::vector<SourceIndex> indexed;

    auto worker_index = [ & ]( SourceEntry *src ) {
        auto t0 = std::chrono::steady_clock::now();
        auto r = http_get_text( src->url, s.index_timeout_ms );
        auto t1 = std::chrono::steady_clock::now();
        int ms = ( int ) std::chrono::duration_cast< std::chrono::milliseconds >( t1 - t0 ).count();

        src->last_latency_ms = ms;
        src->last_ok = ( r.err.empty() && r.status >= 200 && r.status < 400 );

        SourceIndex si;
        si.src = src;
        if ( src->last_ok ) {
            si.links = extract_map_links_from_index_html( src->url, r.body );
            log.push( "[+] " + src->url + " -> " + std::to_string( si.links.size() ) + " file(s) (" +
                std::to_string( ms ) + "ms)" );
        }
        else {
            log.fail( "[IDX] " + src->url + " failed (" +
                ( r.err.empty() ? ( "HTTP " + std::to_string( r.status ) ) : r.err ) + ")" );
        }

        {
            std::lock_guard lk( idx_mtx );
            indexed.push_back( std::move( si ) );
        }
        rs.indexing.done.fetch_add( 1 );
        };

    std::vector<std::future<void>> futs;
    int threads = std::max( 1, s.threads );
    std::atomic<int> in_flight{ 0 };

    for ( auto *src : enabled ) {
        while ( !rs.cancel.load() ) {
            if ( in_flight.load() < threads ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );
        }
        if ( rs.cancel.load() ) break;

        in_flight.fetch_add( 1 );
        futs.push_back( std::async( std::launch::async, [ &, src ] {
            worker_index( src );
            in_flight.fetch_sub( 1 );
            } ) );
    }
    for ( auto &f : futs ) f.wait();

    rs.indexing.running.store( false );

    auto availability = build_availability( indexed );

    int remote_unique = ( int ) availability.size();
    int remote_after_filters = 0;
    int already_have = 0;
    int to_download = 0;

    std::vector<std::string> to_get;
    to_get.reserve( availability.size() );

    for ( auto &[name, srcs] : availability ) {
        if ( !passes_filters( name, includes, excludes ) ) continue;
        remote_after_filters++;

        if ( rs.existing_files.contains( name ) ) already_have++;
        else {
            to_download++;
            to_get.push_back( name );
        }
    }

    rs.last_remote_unique.store( remote_unique );
    rs.last_remote_after_filters.store( remote_after_filters );
    rs.last_already_have.store( already_have );
    rs.last_to_download.store( to_download );

    log.push( "[i] Remote unique files: " + std::to_string( remote_unique ) );
    log.push( "[i] After filters: " + std::to_string( remote_after_filters ) );
    log.push( "[i] Already present locally: " + std::to_string( already_have ) );
    log.push( "[i] Unique maps to download: " + std::to_string( to_download ) );

    rs.downloading.running.store( true );
    rs.downloading.done.store( 0 );
    rs.downloading.total.store( ( int ) to_get.size() );

    std::atomic<int> dl_inflight{ 0 };
    std::vector<std::future<void>> dl_futs;

    for ( auto &name : to_get ) {
        while ( !rs.cancel.load() ) {
            if ( dl_inflight.load() < threads ) break;
            std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
        }
        if ( rs.cancel.load() ) break;

        dl_inflight.fetch_add( 1 );
        dl_futs.push_back( std::async( std::launch::async, [ &, name ] {
            auto &srcs = availability[ name ];
            auto *best = pick_best_source( srcs );
            if ( !best ) {
                log.fail( "[DL] No source for: " + name );
                rs.downloading.done.fetch_add( 1 );
                dl_inflight.fetch_sub( 1 );
                return;
            }
            auto url = url_join( best->url, name );
            auto out = dl_dir / name;
            if ( !download_file( url, out, s.dl_timeout_ms, s.retries, rs.cancel, log ) ) {
                rs.downloading.done.fetch_add( 1 );
                dl_inflight.fetch_sub( 1 );
                return;
            }
            rs.downloading.done.fetch_add( 1 );
            dl_inflight.fetch_sub( 1 );
            } ) );
    }
    for ( auto &f : dl_futs ) f.wait();
    rs.downloading.running.store( false );

    if ( rs.cancel.load() ) {
        log.push( "[i] Cancelled." );
        return;
    }

    if ( s.decompress ) {
        std::vector<fs::path> bz2s;
        for ( auto &e : fs::directory_iterator( dl_dir ) ) {
            if ( !e.is_regular_file() ) continue;
            auto ext = lower_copy( e.path().extension().string() );
            if ( ext == ".bz2" ) bz2s.push_back( e.path() );
        }

        rs.decompressing.running.store( true );
        rs.decompressing.done.store( 0 );
        rs.decompressing.total.store( ( int ) bz2s.size() );
        log.push( "[i] Decompressing .bz2: " + std::to_string( bz2s.size() ) );

        std::atomic<int> bz_inflight{ 0 };
        std::vector<std::future<void>> bz_futs;

        for ( auto &bz2 : bz2s ) {
            while ( !rs.cancel.load() ) {
                if ( bz_inflight.load() < threads ) break;
                std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
            }
            if ( rs.cancel.load() ) break;

            bz_inflight.fetch_add( 1 );
            bz_futs.push_back( std::async( std::launch::async, [ &, bz2 ] {
                auto out = bz2;
                out.replace_extension( "" );
                if ( !decompress_bz2_to_file( bz2, out, s.retries, rs.cancel, log ) ) {
                    rs.decompressing.done.fetch_add( 1 );
                    bz_inflight.fetch_sub( 1 );
                    return;
                }
                rs.decompressing.done.fetch_add( 1 );
                bz_inflight.fetch_sub( 1 );
                } ) );
        }
        for ( auto &f : bz_futs ) f.wait();
        rs.decompressing.running.store( false );

        if ( s.delete_bz2 && !rs.cancel.load() ) {
            rs.deleting.running.store( true );
            rs.deleting.done.store( 0 );
            rs.deleting.total.store( ( int ) bz2s.size() );
            log.push( "[i] Deleting .bz2 files..." );

            for ( auto &bz2 : bz2s ) {
                if ( rs.cancel.load() ) break;
                std::error_code dec;
                fs::remove( bz2, dec );
                if ( dec ) log.fail( "[DEL] " + bz2.filename().string() + " -> " + dec.message() );
                rs.deleting.done.fetch_add( 1 );
            }
            rs.deleting.running.store( false );
        }
    }

    log.push( "[i] Done." );
}

static float progress01( const PhaseProgress &p ) {
    int t = p.total.load();
    int d = p.done.load();
    if ( t <= 0 ) return 0.f;
    float v = ( float ) d / ( float ) t;
    if ( v < 0.f ) v = 0.f;
    if ( v > 1.f ) v = 1.f;
    return v;
}

int main() {
    curl_global_init( CURL_GLOBAL_DEFAULT );

    LiveLog log;
    ensure_logs_dir( log );

    auto sources = load_sources( log );
    auto settings = load_settings( log );

    if ( settings.hl2mp_path.empty() || !fs::exists( settings.hl2mp_path ) ) {
        auto found = find_hl2mp_dir();
        if ( found ) settings.hl2mp_path = *found;
    }

    RunState rs;

    using namespace ftxui;

    int tab = 0;
    std::vector<std::string> tabs = { "Run", "Sources", "Settings", "Logs" };

    std::string hl2mp_path_str = settings.hl2mp_path.string();
    std::string threads_str = std::to_string( settings.threads > 0 ? settings.threads : default_threads() );
    std::string idx_to_str = std::to_string( settings.index_timeout_ms );
    std::string dl_to_str = std::to_string( settings.dl_timeout_ms );
    std::string head_to_str = std::to_string( settings.head_timeout_ms );
    std::string retries_str = std::to_string( settings.retries );

    std::string include_filters_str = settings.include_filters;
    std::string exclude_filters_str = settings.exclude_filters;

    auto settings_view = Container::Vertical( {
        Input( &hl2mp_path_str, "Path to hl2mp" ),
        Input( &threads_str, "Threads" ),

        Input( &include_filters_str, "Include filters (comma)" ),
        Input( &exclude_filters_str, "Exclude filters (comma)" ),

        Checkbox( "Decompress .bz2", &settings.decompress ),
        Checkbox( "Delete .bz2 after extract", &settings.delete_bz2 ),

        Input( &idx_to_str, "Index timeout (ms)" ),
        Input( &head_to_str, "HEAD timeout (ms)" ),
        Input( &dl_to_str, "Download timeout (ms)" ),
        Input( &retries_str, "Retries" ),

        Button( "Auto-detect hl2mp", [ & ] {
            auto found = find_hl2mp_dir();
            if ( found ) {
                settings.hl2mp_path = *found;
                hl2mp_path_str = settings.hl2mp_path.string();
                log.push( "[i] Detected: " + hl2mp_path_str );
            }
            else {
                log.fail( "[!] Auto-detect failed." );
            }
        } ),

        Button( "Save", [ & ] {
            settings.hl2mp_path = fs::path( trim( hl2mp_path_str ) );
            settings.threads = std::max( 1, std::atoi( trim( threads_str ).c_str() ) );

            settings.include_filters = trim( include_filters_str );
            settings.exclude_filters = trim( exclude_filters_str );

            settings.index_timeout_ms = std::max( 1000, std::atoi( trim( idx_to_str ).c_str() ) );
            settings.head_timeout_ms = std::max( 500, std::atoi( trim( head_to_str ).c_str() ) );
            settings.dl_timeout_ms = std::max( 5000, std::atoi( trim( dl_to_str ).c_str() ) );
            settings.retries = std::clamp( std::atoi( trim( retries_str ).c_str() ), 0, 20 );

            save_settings( settings, log );
            log.push( "[i] Saved settings.json" );
        } ),
        } );

    std::atomic<bool> running{ false };
    std::thread runner;

    auto apply_ui_to_settings = [ & ] {
        settings.hl2mp_path = fs::path( trim( hl2mp_path_str ) );
        settings.threads = std::max( 1, std::atoi( trim( threads_str ).c_str() ) );

        settings.include_filters = trim( include_filters_str );
        settings.exclude_filters = trim( exclude_filters_str );

        settings.index_timeout_ms = std::max( 1000, std::atoi( trim( idx_to_str ).c_str() ) );
        settings.head_timeout_ms = std::max( 1000, std::atoi( trim( head_to_str ).c_str() ) );
        settings.dl_timeout_ms = std::max( 5000, std::atoi( trim( dl_to_str ).c_str() ) );
        settings.retries = std::clamp( std::atoi( trim( retries_str ).c_str() ), 0, 20 );
        };

    auto start_btn = Button( "Start", [ & ] {
        if ( running.load() ) return;

        apply_ui_to_settings();
        save_settings( settings, log );
        save_sources( sources, log );

        rs.cancel.store( false );
        running.store( true );

        if ( runner.joinable() ) runner.join();
        runner = std::thread( [ & ] {
            run_pipeline( settings, sources, rs, log );
            save_sources( sources, log );
            write_session_log( log );
            running.store( false );
            } );
        } );

    auto index_btn = Button( "Index", [ & ] {
        if ( running.load() ) return;

        apply_ui_to_settings();
        save_settings( settings, log );
        save_sources( sources, log );

        rs.cancel.store( false );
        running.store( true );

        if ( runner.joinable() ) runner.join();
        runner = std::thread( [ & ] {
            run_index_only( settings, sources, rs, log );
            save_sources( sources, log );
            write_session_log( log );
            running.store( false );
            } );
        } );

    auto cancel_btn = Button( "Cancel", [ & ] { rs.cancel.store( true ); } );

    auto run_view = Container::Vertical( { start_btn, index_btn, cancel_btn } );

    auto run_panel = Renderer( run_view, [ & ] {
        auto phase = [ & ]( const char *name, const PhaseProgress &p ) {
            int d = p.done.load();
            int t = p.total.load();
            return hbox( {
                text( std::string( name ) + " " ) | size( WIDTH, EQUAL, 16 ),
                gauge( progress01( p ) ) | flex,
                text( " " + std::to_string( d ) + "/" + std::to_string( t ) )
                } );
            };

        auto stats = vbox( {
            text( "Last Index Summary" ) | bold,
            text( "Remote unique: " + std::to_string( rs.last_remote_unique.load() ) ),
            text( "After filters: " + std::to_string( rs.last_remote_after_filters.load() ) ),
            text( "Already have: " + std::to_string( rs.last_already_have.load() ) ),
            text( "Would download: " + std::to_string( rs.last_to_download.load() ) ),
            } ) | border;

        return vbox( {
            text( "Run" ) | bold,
            separator(),
            hbox( {
                start_btn->Render(),
                text( "  " ),
                index_btn->Render(),
                text( "  " ),
                cancel_btn->Render(),
                text( running.load() ? "  (running)" : "" )
            } ),
            separator(),
            phase( "Indexing", rs.indexing ),
            phase( "Downloading", rs.downloading ),
            phase( "Decompress", rs.decompressing ),
            phase( "Deleting", rs.deleting ),
            separator(),
            stats,
            separator(),
            text( "Note: sources/links must be end with \"/maps/\", e.g. https://www.example.com/hl2mp/maps/." ),
            text( "Tip: Indexing will only show the number of maps that will be downloaded per your filters, if any." ),
            } ) | border;
        } );

    auto settings_panel = Renderer( settings_view, [ & ] {
        auto line = []( const std::string &label, const ftxui::Component &c ) {
            return ftxui::vbox( { ftxui::text( label ), c->Render() } );
            };

        return ftxui::vbox( {
            ftxui::text( "Settings" ) | ftxui::bold,
            ftxui::separator(),

            line( "HL2DM hl2mp folder:", settings_view->ChildAt( 0 ) ),
            line( "Threads (parallel workers):", settings_view->ChildAt( 1 ) ),

            ftxui::separator(),
            line( "Include filters (comma-separated substrings):", settings_view->ChildAt( 2 ) ),
            line( "Exclude filters (comma-separated substrings):", settings_view->ChildAt( 3 ) ),

            ftxui::separator(),
            settings_view->ChildAt( 4 )->Render(),
            settings_view->ChildAt( 5 )->Render(),

            ftxui::separator(),
            line( "Index timeout (ms):", settings_view->ChildAt( 6 ) ),
            line( "HEAD timeout (ms):", settings_view->ChildAt( 7 ) ),
            line( "Download timeout (ms):", settings_view->ChildAt( 8 ) ),
            line( "Retries:", settings_view->ChildAt( 9 ) ),

            ftxui::separator(),
            settings_view->ChildAt( 10 )->Render(),
            settings_view->ChildAt( 11 )->Render(),
            } ) | ftxui::border;
        } );

    auto logs_view = Renderer( [ & ] {
        std::lock_guard lk( log.mtx );
        Elements els;
        els.push_back( text( "Live Log" ) | bold );
        els.push_back( separator() );
        for ( auto it = log.lines.rbegin(); it != log.lines.rend() && ( int ) els.size() < 22; ++it )
            els.push_back( text( *it ) );
        els.push_back( separator() );
        els.push_back( text( "Failures" ) | bold );
        for ( auto it = log.failures.rbegin(); it != log.failures.rend() && ( int ) els.size() < 34; ++it )
            els.push_back( text( *it ) | color( Color::RedLight ) );
        return vbox( std::move( els ) ) | border;
        } );

    auto screen = ScreenInteractive::TerminalOutput();

    struct SourcesUI {
        int selected = 0;
        int scroll = 0;
        std::string add_url;
        ftxui::Box list_box;
    } sui;

    auto add_input = Input( &sui.add_url, "https://..." );

    auto add_btn = Button( "Add", [ & ] {
        auto u = normalize_maps_url( sui.add_url );
        if ( u.empty() ) return;

        auto it = std::find_if( sources.begin(), sources.end(),
            [ & ]( const SourceEntry &s ) { return s.url == u; } );

        if ( it == sources.end() ) {
            sources.push_back( SourceEntry{ u, true, -1, false } );
            log.push( "[i] Added source: " + u );
            save_sources( sources, log );
        }
        else {
            it->enabled = true;
            log.push( "[i] Source already exists, enabled: " + u );
            save_sources( sources, log );
        }

        sui.add_url.clear();
        if ( !sources.empty() ) sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
        screen.PostEvent( Event::Custom );
        } );

    auto del_selected_btn = Button( "Delete Selected", [ & ] {
        if ( sources.empty() ) return;
        sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
        log.push( "[i] Deleted source: " + sources[ ( size_t ) sui.selected ].url );
        sources.erase( sources.begin() + ( ptrdiff_t ) sui.selected );
        if ( sources.empty() ) sui.selected = 0;
        else sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
        save_sources( sources, log );
        screen.PostEvent( Event::Custom );
        } );

    auto del_disabled_btn = Button( "Delete Disabled", [ & ] {
        auto before = sources.size();
        sources.erase( std::remove_if( sources.begin(), sources.end(),
            []( const SourceEntry &s ) { return !s.enabled; } ),
            sources.end() );
        if ( sources.size() != before ) {
            log.push( "[i] Deleted disabled sources." );
            save_sources( sources, log );
        }
        if ( sources.empty() ) sui.selected = 0;
        else sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
        screen.PostEvent( Event::Custom );
        } );

    auto save_sources_btn = Button( "Save", [ & ] {
        save_sources( sources, log );
        log.push( "[i] Saved sources.json" );
        screen.PostEvent( Event::Custom );
        } );

    auto sources_panel_inner = Container::Vertical( {
        add_input,
        Container::Horizontal( { add_input, add_btn } ),
        Container::Horizontal( { del_selected_btn, del_disabled_btn, save_sources_btn } ),
        } );

    auto sources_panel = Renderer( sources_panel_inner, [ & ] {
        Elements els;
        els.push_back( text( "Sources (base URL or /maps/ directory)" ) | bold );
        els.push_back( separator() );

        int list_height = 18;
        if ( !sources.empty() ) {
            sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
            int max_scroll = std::max( 0, ( int ) sources.size() - list_height );
            sui.scroll = std::clamp( sui.scroll, 0, max_scroll );

            if ( sui.selected < sui.scroll ) sui.scroll = sui.selected;
            if ( sui.selected >= sui.scroll + list_height ) sui.scroll = sui.selected - list_height + 1;

            Elements rows;
            for ( int row = 0; row < list_height; ++row ) {
                int idx = sui.scroll + row;
                if ( idx >= ( int ) sources.size() ) break;

                auto &s = sources[ ( size_t ) idx ];
                std::string badge = s.last_ok ? ( " ok " + std::to_string( s.last_latency_ms ) + "ms" ) : " ? ";
                std::string box = s.enabled ? "[x] " : "[ ] ";
                std::string del = " [Del]";

                auto line = hbox( {
                    text( box ),
                    text( s.url ) | flex,
                    text( " " ),
                    text( badge ),
                    text( del ),
                    } );

                if ( idx == sui.selected ) line = line | inverted;
                rows.push_back( line );
            }

            els.push_back(
                vbox( std::move( rows ) )
                | ftxui::reflect( sui.list_box )
                | frame
                | vscroll_indicator
                | size( ftxui::HEIGHT, ftxui::EQUAL, list_height )
            );
        }
        else {
            els.push_back( text( "No sources. Add one below." ) );
            els.push_back( text( "" ) );
        }

        els.push_back( separator() );
        els.push_back( hbox( { text( "Add: " ), add_input->Render() | flex, add_btn->Render() } ) );
        els.push_back( hbox( {
            del_selected_btn->Render(),
            text( "  " ),
            del_disabled_btn->Render(),
            text( "  " ),
            save_sources_btn->Render()
            } ) );
        els.push_back( separator() );
        els.push_back( text( "Keys: ↑/↓ select, Space toggle enabled, Ctrl+D delete selected" ) );

        return vbox( std::move( els ) ) | border;
        } );

    sources_panel = CatchEvent( sources_panel, [ & ]( Event e ) {
        if ( add_input->Focused() ) return false;

        auto delete_selected = [ & ] {
            if ( sources.empty() ) return;
            sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
            log.push( "[i] Deleted source: " + sources[ ( size_t ) sui.selected ].url );
            sources.erase( sources.begin() + ( ptrdiff_t ) sui.selected );
            if ( sources.empty() ) sui.selected = 0;
            else sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
            save_sources( sources, log );
            screen.PostEvent( Event::Custom );
            };

        auto toggle_selected = [ & ] {
            if ( sources.empty() ) return;
            sui.selected = std::clamp( sui.selected, 0, ( int ) sources.size() - 1 );
            sources[ ( size_t ) sui.selected ].enabled = !sources[ ( size_t ) sui.selected ].enabled;
            save_sources( sources, log );
            screen.PostEvent( Event::Custom );
            };

        if ( e.is_mouse() ) {
            auto m = e.mouse();

            if ( m.motion == ftxui::Mouse::WheelUp ) {
                if ( !sources.empty() ) {
                    sui.selected = std::max( 0, sui.selected - 1 );
                    screen.PostEvent( Event::Custom );
                }
                return true;
            }
            if ( m.motion == ftxui::Mouse::WheelDown ) {
                if ( !sources.empty() ) {
                    sui.selected = std::min( ( int ) sources.size() - 1, sui.selected + 1 );
                    screen.PostEvent( Event::Custom );
                }
                return true;
            }

            if ( m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Pressed ) {
                if ( !sources.empty() && sui.list_box.Contain( m.x, m.y ) ) {
                    int row = m.y - sui.list_box.y_min;
                    int idx = sui.scroll + row;

                    if ( idx >= 0 && idx < ( int ) sources.size() ) {
                        sui.selected = idx;

                        int del_width = 5;
                        if ( m.x >= sui.list_box.x_max - del_width ) {
                            delete_selected();
                        }
                        else {
                            toggle_selected();
                        }

                        screen.PostEvent( Event::Custom );
                        return true;
                    }
                }
            }

            return false;
        }

        if ( e == Event::ArrowUp ) {
            if ( !sources.empty() ) {
                sui.selected = std::max( 0, sui.selected - 1 );
                screen.PostEvent( Event::Custom );
            }
            return true;
        }
        if ( e == Event::ArrowDown ) {
            if ( !sources.empty() ) {
                sui.selected = std::min( ( int ) sources.size() - 1, sui.selected + 1 );
                screen.PostEvent( Event::Custom );
            }
            return true;
        }
        if ( e == Event::Character( ' ' ) || e == Event::Return ) {
            toggle_selected();
            return true;
        }
        if ( e == Event::CtrlD ) {
            delete_selected();
            return true;
        }

        return false;
        } );

    auto tab_toggle = Toggle( &tabs, &tab );

    auto main_container = Container::Vertical( {
        tab_toggle,
        Container::Tab( {
            run_panel,
            sources_panel,
            settings_panel,
            logs_view
        }, &tab ),
        } );

    auto ui = Renderer( main_container, [ & ] {
        return vbox( {
            tab_toggle->Render(),
            main_container->ChildAt( 1 )->Render()
            } );
        } );

    screen.TrackMouse( true );
    screen.Loop( ui );

    rs.cancel.store( true );
    if ( runner.joinable() ) runner.join();

    save_sources( sources, log );
    save_settings( settings, log );
    write_session_log( log );

    curl_global_cleanup();
    return 0;
}
