// mod: the Mods menu's mods (com_mods.h): each mods/<name>/mod.txt read into a ModInfo, the menu's choices kept in
// archived dvars, and what they need applied when a game starts (fs_mods, dvars), or when the game itself starts
// (players/mods_startup.cfg: memory and pools sized at startup). The menu itself is src/ui/ui_mods.cpp.
#include "com_mods.h"
#include "common.h"
#include "cmd.h"
#include <universal/dvar.h>
#include <universal/q_shared.h>
#include <universal/com_files.h>
#include <database/db_registry.h>
#include <win32/win_common.h>
#include "bo1_inflate.h"
#include "unzip.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

// ----- startup: players/mods_startup.cfg -----

static bool s_startupInit;
static bool s_cmdlineMods;
static int s_cmdlineZones; // words after bo1_mod_zones on the command line (or bo1_mod_memory's MB as zones)
static int s_fileZones;    // "zones N" in the startup file
static std::string s_setArgs;

static const char *s_startupFile = "players/mods_startup.cfg";

// name (case-insensitive) as a word of the command line: after its start, a space, a '+' or a quote, before a space,
// a quote or its end
static const char *CmdLine_FindWord(const char *cmdline, const char *name)
{
    const size_t len = strlen(name);
    for ( const char *p = I_stristr(cmdline, name); p; p = I_stristr(p + 1, name) )
    {
        const bool start = p == cmdline || p[-1] == ' ' || p[-1] == '+' || p[-1] == '"' || p[-1] == '\t';
        const char end = p[len];
        if ( start && (!end || end == ' ' || end == '"' || end == '\t') )
            return p;
    }
    return nullptr;
}

// the zone names after bo1_mod_zones on the command line (as PMem_Init counted them)
static int CmdLine_ModZones(const char *cmdline)
{
    const char *modZones = I_stristr(cmdline, "bo1_mod_zones");
    if ( !modZones )
        return 0;
    int words = 0;
    const char *p = modZones + strlen("bo1_mod_zones");
    for ( ;; )
    {
        while ( *p == ' ' || *p == '\t' || *p == '"' )
            ++p;
        if ( !*p || *p == '+' || *p == '-' )
            break;
        ++words;
        while ( *p && *p != ' ' && *p != '\t' && *p != '"' )
            ++p;
    }
    // (bo1_mod_zones with no zone still sized the pools for mod zones, as before)
    return words ? words : 1;
}

void Com_ModStartup_Init(const char *cmdline)
{
    if ( s_startupInit )
        return;
    s_startupInit = true;
    if ( !cmdline )
        cmdline = "";
    s_cmdlineZones = CmdLine_ModZones(cmdline);
    // bo1_mod_memory <MB>: memory for a map mod on the command line (+set fs_mods Perish +set bo1_mod_memory 384)
    if ( const char *mem = CmdLine_FindWord(cmdline, "bo1_mod_memory") )
    {
        const int mb = atoi(mem + strlen("bo1_mod_memory"));
        const int zones = mb <= 0 ? 0 : mb <= 256 ? 1 : (mb + 127) / 128;
        s_cmdlineZones = zones > s_cmdlineZones ? zones : s_cmdlineZones;
    }
    s_cmdlineMods = CmdLine_FindWord(cmdline, "fs_mods") || CmdLine_FindWord(cmdline, "fs_game");
    // a launcher's mods, a map started from the command line or a test run: the game starts as asked
    if ( s_cmdlineMods || CmdLine_FindWord(cmdline, "map") || CmdLine_FindWord(cmdline, "devmap")
        || I_stristr(cmdline, "bo1_headless") || I_stristr(cmdline, "dedicated") )
        return;
    // the players folder of fs_h (the game folder: the working directory unless the command line names another)
    std::string path = s_startupFile;
    if ( const char *h = CmdLine_FindWord(cmdline, "fs_h") )
    {
        const char *p = h + 4;
        while ( *p == ' ' )
            ++p;
        std::string home;
        if ( *p == '"' )
        {
            for ( ++p; *p && *p != '"'; ++p )
                home += *p;
        }
        else
        {
            for ( ; *p && *p != ' '; ++p )
                home += *p;
        }
        if ( !home.empty() )
            path = home + "/" + s_startupFile;
    }
    FILE *f = fopen(path.c_str(), "rb");
    if ( !f )
        return;
    char line[512];
    while ( fgets(line, sizeof(line), f) )
    {
        char key[64], dvar[64], value[256];
        if ( sscanf(line, "%63s", key) != 1 || key[0] == '/' )
            continue;
        if ( !I_stricmp(key, "zones") )
        {
            s_fileZones = atoi(line + strspn(line, " \t") + 5);
            if ( s_fileZones < 0 || s_fileZones > 6 ) // (BO1_MOD_ZONES_MAX)
                s_fileZones = s_fileZones < 0 ? 0 : 6;
        }
        else if ( !I_stricmp(key, "set") && sscanf(line, "%*s %63s %255[^\r\n]", dvar, value) == 2 )
        {
            bool ok = true;
            for ( const char *c = dvar; *c; ++c )
                ok = ok && (isalnum((unsigned char)*c) || *c == '_');
            for ( const char *c = value; *c; ++c )
                ok = ok && *c != '+' && *c != '"' && *c != ';';
            if ( ok )
            {
                s_setArgs += " +set ";
                s_setArgs += dvar;
                s_setArgs += " ";
                s_setArgs += value;
            }
        }
    }
    fclose(f);
}

int Com_ModStartup_ZonesReserved()
{
    if ( !s_startupInit )
        Com_ModStartup_Init(GetCommandLineA());
    return s_cmdlineZones > s_fileZones ? s_cmdlineZones : s_fileZones;
}

const char *Com_ModStartup_SetArgs()
{
    return s_setArgs.c_str();
}

bool Com_ModStartup_CommandLineMods()
{
    return s_cmdlineMods;
}

// ----- mod.txt -----

static std::vector<ModInfo> s_mods;
static bool s_menuActive;
static std::set<std::string> s_strings; // dvar names and descriptions (dvars keep the pointers)

static const char *Intern(const std::string &s)
{
    return s_strings.insert(s).first->c_str();
}

static bool IsDvarName(const std::string &s)
{
    if ( s.empty() || s.size() > 60 )
        return false;
    for ( char c : s )
    {
        if ( !isalnum((unsigned char)c) && c != '_' )
            return false;
    }
    return true;
}

static bool IsNumber(const std::string &s, double *v = nullptr)
{
    if ( s.empty() )
        return false;
    char *end = nullptr;
    const double d = strtod(s.c_str(), &end);
    if ( !end || *end )
        return false;
    if ( v )
        *v = d;
    return true;
}

static std::vector<std::string> Words(const std::string &s)
{
    std::vector<std::string> words;
    std::string w;
    for ( char c : s )
    {
        if ( c == ' ' || c == '\t' )
        {
            if ( !w.empty() )
                words.push_back(w);
            w.clear();
        }
        else
        {
            w += c;
        }
    }
    if ( !w.empty() )
        words.push_back(w);
    return words;
}

struct ModTokens
{
    std::vector<std::string> tokens;
    std::vector<int> lines;
    size_t pos = 0;
    std::string file;
    bool failed = false;

    bool more() const { return pos < tokens.size(); }
    const std::string &peek() const
    {
        static const std::string none;
        return pos < tokens.size() ? tokens[pos] : none;
    }
    std::string next()
    {
        if ( pos < tokens.size() )
            return tokens[pos++];
        failed = true;
        return "";
    }
    void error(const char *what)
    {
        const int line = lines.empty() ? 0 : lines[pos < lines.size() ? pos : lines.size() - 1];
        Com_PrintWarning(10, "mods: %s line %d: %s\n", file.c_str(), line, what);
        failed = true;
    }
};

// words, "quoted strings" (no escapes; a line break ends one), { and }; // and /* */ comments
static void Mod_Tokenize(const std::string &text, ModTokens &t)
{
    int line = 1;
    for ( size_t i = 0; i < text.size(); )
    {
        const char c = text[i];
        if ( c == '\n' )
        {
            ++line;
            ++i;
        }
        else if ( isspace((unsigned char)c) )
        {
            ++i;
        }
        else if ( c == '/' && i + 1 < text.size() && text[i + 1] == '/' )
        {
            while ( i < text.size() && text[i] != '\n' )
                ++i;
        }
        else if ( c == '/' && i + 1 < text.size() && text[i + 1] == '*' )
        {
            i += 2;
            while ( i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/') )
                line += text[i++] == '\n';
            i += 2;
        }
        else if ( c == '{' || c == '}' )
        {
            t.tokens.push_back(std::string(1, c));
            t.lines.push_back(line);
            ++i;
        }
        else if ( c == '"' )
        {
            std::string s;
            for ( ++i; i < text.size() && text[i] != '"' && text[i] != '\n'; ++i )
                s += text[i];
            if ( i < text.size() && text[i] == '"' )
                ++i;
            t.tokens.push_back(s);
            t.lines.push_back(line);
        }
        else
        {
            std::string s;
            while ( i < text.size() && !isspace((unsigned char)text[i]) && text[i] != '{' && text[i] != '}'
                    && text[i] != '"' )
                s += text[i++];
            t.tokens.push_back(s);
            t.lines.push_back(line);
        }
    }
}

// set <dvar> <value> [map <map>] | startup <dvar> <value> | needs <zone> "<label>" [map <map>]
static bool Mod_ParseSetting(ModTokens &t, const std::string &keyword, std::vector<ModSetting> &settings,
    std::vector<ModNeed> &needs)
{
    if ( keyword == "set" || keyword == "startup" )
    {
        ModSetting s;
        s.dvar = t.next();
        s.value = t.next();
        s.startup = keyword == "startup";
        if ( !s.startup && t.peek() == "map" )
        {
            t.next();
            s.map = t.next();
        }
        if ( t.failed || !IsDvarName(s.dvar) )
        {
            t.error("set / startup: a dvar name and a value");
            return true;
        }
        if ( s.value.find_first_of("+\";") != std::string::npos )
        {
            t.error("a value can't hold '+', '\"' or ';'");
            return true;
        }
        settings.push_back(s);
        return true;
    }
    if ( keyword == "needs" )
    {
        ModNeed n;
        n.zone = t.next();
        n.label = t.next();
        if ( t.peek() == "map" )
        {
            t.next();
            n.map = t.next();
        }
        if ( t.failed || !IsDvarName(n.zone) )
            t.error("needs: a zone name and its label");
        else
            needs.push_back(n);
        return true;
    }
    return false;
}

static void Mod_ParseOption(ModTokens &t, ModInfo &mod)
{
    ModOption o;
    o.dvar = t.next();
    o.type = "choice";
    o.var = nullptr;
    if ( !IsDvarName(o.dvar) || t.next() != "{" )
    {
        t.error("option <dvar> { ... }");
        return;
    }
    double rangeMin = 0, rangeMax = 0, rangeStep = 1;
    bool range = false;
    while ( !t.failed && t.more() && t.peek() != "}" )
    {
        const std::string k = t.next();
        if ( k == "label" )
            o.label = t.next();
        else if ( k == "help" )
            o.help = t.next();
        else if ( k == "type" )
            o.type = t.next();
        else if ( k == "default" )
            o.def = t.next();
        else if ( k == "min" || k == "max" || k == "step" )
        {
            double v;
            if ( !IsNumber(t.next(), &v) )
                t.error("min / max / step: a number");
            else
                (k == "min" ? rangeMin : k == "max" ? rangeMax : rangeStep) = v;
            range = true;
        }
        else if ( k == "value" )
        {
            ModValue v;
            v.value = t.next();
            v.label = t.peek() == "{" ? v.value : t.next();
            if ( t.peek() == "{" )
            {
                t.next();
                while ( !t.failed && t.more() && t.peek() != "}" )
                {
                    const std::string sk = t.next();
                    if ( !Mod_ParseSetting(t, sk, v.settings, v.needs) )
                        t.error(va("unknown word '%s' in a value's { }", sk.c_str()));
                }
                if ( t.next() != "}" )
                    t.error("a value's { without }");
            }
            o.values.push_back(v);
        }
        else
        {
            t.error(va("unknown word '%s' in option %s", k.c_str(), o.dvar.c_str()));
        }
    }
    if ( t.next() != "}" )
    {
        t.error("option { without }");
        return;
    }
    if ( o.type == "toggle" && o.values.empty() )
    {
        o.values.push_back({ "0", "OFF", {}, {} });
        o.values.push_back({ "1", "ON", {}, {} });
    }
    else if ( (o.type == "range" || o.type == "slider") && o.values.empty() && range && rangeStep > 0 && rangeMax >= rangeMin )
    {
        // (a menu list holds 32 values: a longer range is stepped more coarsely)
        double step = rangeStep;
        while ( (rangeMax - rangeMin) / step + 1 > 32 )
            step *= 2;
        for ( double v = rangeMin; v <= rangeMax + step * 0.001; v += step )
        {
            const std::string s = va("%g", v);
            o.values.push_back({ s, s, {}, {} });
        }
    }
    if ( o.values.empty() || o.values.size() > 32 )
    {
        t.error(va("option %s: 1 to 32 values", o.dvar.c_str()));
        return;
    }
    if ( o.label.empty() )
        o.label = o.dvar;
    bool found = false;
    for ( const ModValue &v : o.values )
        found = found || v.value == o.def;
    if ( !found )
        o.def = o.values[0].value;
    mod.options.push_back(o);
}

static bool Mod_Parse(const std::string &folder, const std::string &file, const std::string &text, ModInfo &mod)
{
    ModTokens t;
    t.file = file;
    Mod_Tokenize(text, t);
    mod.folder = folder;
    mod.name = folder;
    mod.order = 100;
    mod.enabled = nullptr;
    mod.mapMod = false;
    while ( !t.failed && t.more() )
    {
        const std::string k = t.next();
        if ( k == "name" )
            mod.name = t.next();
        else if ( k == "description" )
            mod.description = t.next();
        else if ( k == "order" )
            mod.order = atoi(t.next().c_str());
        else if ( k == "version" )
            mod.version = t.next();
        else if ( k == "map" )
        {
            ModMap m;
            m.name = t.next();
            // (the label is optional: the next word is a keyword or a map's label)
            static const char *const keywords[] = { "name", "description", "order", "map", "needs", "set", "startup", "option",
                "clear", "version" };
            bool keyword = !t.more();
            for ( const char *kw : keywords )
                keyword = keyword || t.peek() == kw;
            m.label = keyword ? m.name : t.next();
            mod.maps.push_back(m);
        }
        else if ( k == "option" )
            Mod_ParseOption(t, mod);
        else if ( k == "clear" )
        {
            const std::string pattern = t.next();
            const std::string name = !pattern.empty() && pattern.back() == '*' ? pattern.substr(0, pattern.size() - 1) : pattern;
            if ( !IsDvarName(name) )
                t.error("clear: a dvar name, or the start of one and *");
            else
                mod.clears.push_back(pattern);
        }
        else if ( !Mod_ParseSetting(t, k, mod.settings, mod.needs) )
            t.error(va("unknown word '%s'", k.c_str()));
    }
    return !t.failed;
}

static std::string Mod_ReadFile(const std::string &path)
{
    std::string text;
    FILE *f = fopen(path.c_str(), "rb");
    if ( !f )
        return text;
    char buf[4096];
    size_t n;
    while ( (n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 256 * 1024 )
        text.append(buf, n);
    fclose(f);
    return text;
}

static std::map<std::string, std::string> s_sessionStartup; // a startup dvar's value in this session (first look)

static void Mod_RememberStartup(const std::vector<ModSetting> &settings)
{
    for ( const ModSetting &s : settings )
    {
        if ( !s.startup || s.dvar == "bo1_mod_zones" || s.dvar == "bo1_mod_memory" || s_sessionStartup.count(s.dvar) )
            continue;
        const dvar_s *d = Dvar_FindVar(s.dvar.c_str());
        s_sessionStartup[s.dvar] = d ? Dvar_DisplayableValue(d) : "";
    }
}

// the memory a zone's blocks take when it loads (the XFile header at the start of its compressed data), 0 if unknown
static unsigned int Mod_ZoneMemory(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if ( !f )
        return 0;
    unsigned char in[4096];
    const size_t n = fread(in, 1, sizeof(in), f);
    fclose(f);
    if ( n < 64 || memcmp(in, "IWff", 4) )
        return 0;
    unsigned int header[9] = {}; // size, externalSize, blockSize[7]
    z_stream s;
    memset(&s, 0, sizeof(s));
    s.next_in = in + 12;
    s.avail_in = (uInt)(n - 12);
    s.next_out = (Bytef *)header;
    s.avail_out = sizeof(header);
    if ( Bo1_InflateInit2(&s, MAX_WBITS) != Z_OK )
        return 0;
    Bo1_Inflate(&s, Z_SYNC_FLUSH);
    const bool ok = s.avail_out == 0;
    Bo1_InflateEnd(&s);
    if ( !ok )
        return 0;
    unsigned long long total = 0;
    for ( int b = 2; b < 9; ++b )
        total += header[b];
    return total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)total;
}

// mod.arena (a mod-tools mod's list of its maps, as retail's .arena files): a { } block per map, "map" its zone's name,
// "longname" the localization key of its name
struct ModArenaMap
{
    std::string map;
    std::string longname;
};

static std::vector<ModArenaMap> Mod_ReadArena(const std::string &path)
{
    const std::string text = Mod_ReadFile(path);
    std::vector<std::string> tokens;
    for ( size_t i = 0; i < text.size(); )
    {
        const char c = text[i];
        if ( isspace((unsigned char)c) )
        {
            ++i;
        }
        else if ( c == '/' && i + 1 < text.size() && text[i + 1] == '/' )
        {
            while ( i < text.size() && text[i] != '\n' )
                ++i;
        }
        else if ( c == '{' || c == '}' )
        {
            tokens.push_back(std::string(1, c));
            ++i;
        }
        else if ( c == '"' )
        {
            std::string t;
            for ( ++i; i < text.size() && text[i] != '"'; ++i )
                t += text[i];
            ++i;
            tokens.push_back(t);
        }
        else
        {
            std::string t;
            while ( i < text.size() && !isspace((unsigned char)text[i]) && text[i] != '{' && text[i] != '}'
                    && text[i] != '"' )
                t += text[i++];
            tokens.push_back(t);
        }
    }
    std::vector<ModArenaMap> maps;
    ModArenaMap cur;
    bool in = false;
    for ( size_t i = 0; i < tokens.size(); ++i )
    {
        if ( tokens[i] == "{" )
        {
            in = true;
            cur = ModArenaMap();
        }
        else if ( tokens[i] == "}" )
        {
            if ( in && IsDvarName(cur.map) )
                maps.push_back(cur);
            in = false;
        }
        else if ( in && i + 1 < tokens.size() && tokens[i + 1] != "{" && tokens[i + 1] != "}" )
        {
            if ( !I_stricmp(tokens[i].c_str(), "map") )
                cur.map = tokens[i + 1];
            else if ( !I_stricmp(tokens[i].c_str(), "longname") )
                cur.longname = tokens[i + 1];
            ++i;
        }
    }
    return maps;
}

// the text a zone gives localization keys: its localize assets as the loader reads them, the two pointers -1 and then
// the value and the key inline (a value the zone has already is a pointer to it instead: not found here). A zone is read
// once a session (a map mod's mod.ff, ~40 MB inflated: ~0.3 s)
static std::map<std::string, std::map<std::string, std::string>> s_zoneStrings; // zone path -> key -> text

static const std::map<std::string, std::string> &Mod_ZoneStrings(const std::string &path,
    const std::vector<std::string> &keys)
{
    std::map<std::string, std::string> &found = s_zoneStrings[path];
    bool all = true;
    for ( const std::string &k : keys )
        all = all && found.count(k);
    if ( all )
        return found;
    for ( const std::string &k : keys )
        found[k] = ""; // (not found, unless it is below)
    FILE *f = fopen(path.c_str(), "rb");
    if ( !f )
        return found;
    unsigned char header[12];
    z_stream s;
    memset(&s, 0, sizeof(s));
    if ( fread(header, 1, sizeof(header), f) != sizeof(header) || memcmp(header, "IWff", 4)
         || Bo1_InflateInit2(&s, MAX_WBITS) != Z_OK )
    {
        fclose(f);
        return found;
    }
    std::vector<unsigned char> in(1 << 20);
    std::string raw;
    const size_t step = 4 << 20;
    int ret = Z_OK;
    while ( ret == Z_OK && raw.size() < (768u << 20) )
    {
        if ( !s.avail_in )
        {
            const size_t n = fread(in.data(), 1, in.size(), f);
            if ( !n )
                break;
            s.next_in = in.data();
            s.avail_in = (uInt)n;
        }
        const size_t at = raw.size();
        raw.resize(at + step);
        s.next_out = (Bytef *)&raw[at];
        s.avail_out = (uInt)step;
        ret = Bo1_Inflate(&s, Z_NO_FLUSH);
        raw.resize(at + step - s.avail_out);
    }
    Bo1_InflateEnd(&s);
    fclose(f);
    for ( const std::string &key : keys )
    {
        const std::string pattern = key + std::string(1, '\0');
        for ( size_t p = raw.find(pattern); p != std::string::npos; p = raw.find(pattern, p + 1) )
        {
            if ( p < 10 || raw[p - 1] != '\0' )
                continue;
            size_t start = p - 1;
            while ( start > 0 && p - 1 - start < 256 && (unsigned char)raw[start - 1] >= 0x20
                    && (unsigned char)raw[start - 1] != 0xFF && raw[start - 1] != 0x7F )
                --start;
            if ( start == p - 1 || start < 8 || raw.compare(start - 8, 8, std::string(8, '\xFF')) )
                continue;
            found[key] = raw.substr(start, p - 1 - start);
            break;
        }
    }
    return found;
}

// a map's name from its zone name, when its mod doesn't give one ("zombie_ol_expan": "Ol Expan")
static std::string Mod_MapNameFromZone(const std::string &zone)
{
    std::string name = zone;
    for ( const char *prefix : { "nazi_zombie_", "zombie_" } )
    {
        if ( !I_strnicmp(name.c_str(), prefix, (int)strlen(prefix)) && name.size() > strlen(prefix) )
        {
            name = name.substr(strlen(prefix));
            break;
        }
    }
    return name;
}

// "OVERLOOK REVISITED", "ol expan": "Overlook Revisited", "Ol Expan"
static std::string Mod_TitleCase(const std::string &s)
{
    std::string t;
    bool start = true;
    for ( char c : s )
    {
        if ( c == '_' )
            c = ' ';
        t += start ? (char)toupper((unsigned char)c) : (char)tolower((unsigned char)c);
        start = c == ' ';
    }
    return t;
}

// a file of a mod (its folder's, else one in its .iwd files: a path as in the .iwd, "level_dependencies.csv"); "" none
static std::string Mod_ReadModFile(const std::string &dir, const char *path)
{
    std::string text = Mod_ReadFile(dir + "/" + path);
    if ( !text.empty() )
        return text;
    int count = 0;
    char **files = (char **)Sys_ListFiles((char *)dir.c_str(), (char *)"iwd", nullptr, &count, 0);
    for ( int i = 0; files && i < count && text.empty(); ++i )
    {
        unzFile uf = unzOpen((dir + "/" + files[i]).c_str());
        if ( !uf )
            continue;
        char name[256];
        unz_file_info info;
        for ( int r = unzGoToFirstFile(uf); r == UNZ_OK; r = unzGoToNextFile(uf) )
        {
            if ( unzGetCurrentFileInfo(uf, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK )
                break;
            for ( char *c = name; *c; ++c )
                *c = *c == '\\' ? '/' : *c;
            if ( I_stricmp(name, path) || info.uncompressed_size > 256 * 1024 )
                continue;
            if ( unzOpenCurrentFile(uf) == UNZ_OK )
            {
                text.resize(info.uncompressed_size);
                const int n = text.empty() ? 0 : unzReadCurrentFile(uf, &text[0], (unsigned)text.size());
                text.resize(n > 0 ? n : 0);
                unzCloseCurrentFile(uf);
            }
            break;
        }
        unzClose(uf);
    }
    if ( files )
        FS_FreeFileList((const char **)files);
    return text;
}

// the zones a mod's own level_dependencies.csv has a map load ("<map>,<zone>" lines: the second words), which are no
// maps of its own
static std::set<std::string> Mod_LevelDependencyZones(const std::string &dir)
{
    std::set<std::string> zones;
    const std::string text = Mod_ReadModFile(dir, "level_dependencies.csv");
    size_t p = 0;
    while ( p < text.size() )
    {
        size_t e = text.find('\n', p);
        if ( e == std::string::npos )
            e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        if ( line.compare(0, 2, "//") == 0 )
            continue;
        const size_t comma = line.find(',');
        if ( comma == std::string::npos )
            continue;
        std::string zone = line.substr(comma + 1);
        const size_t end = zone.find_first_of(",\r\t ");
        if ( end != std::string::npos )
            zone.resize(end);
        for ( char &c : zone )
            c = (char)tolower((unsigned char)c);
        if ( !zone.empty() )
            zones.insert(zone);
    }
    return zones;
}

// the memory a mod's zones that load with a level take: its mod.ff and its mod1.ff .. mod7.ff (each while the one before
// exists, as LinkerMod's game_mod loads them), its level dependencies' zones and a common_zombie_patch_override.ff
static unsigned long long Mod_ModZonesMemory(const std::string &dir)
{
    unsigned long long total = Mod_ZoneMemory(dir + "/mod.ff");
    for ( int m = 1; m < 8; ++m )
    {
        const std::string path = dir + "/mod" + std::to_string(m) + ".ff";
        FILE *f = fopen(path.c_str(), "rb");
        if ( !f )
            break;
        fclose(f);
        total += Mod_ZoneMemory(path);
    }
    for ( const std::string &zone : Mod_LevelDependencyZones(dir) )
        total += Mod_ZoneMemory(dir + "/" + zone + ".ff"); // (its own level dependencies' zones, 0 for the game's)
    return total + Mod_ZoneMemory(dir + "/common_zombie_patch_override.ff");
}

// a map mod's maps: those its mod.arena lists, if it has one (Overlook: zombie_overlook and zombie_ol_expan; its
// mod1.ff, sounds, is no map); else its zone files but mod.ff, a language's (en_<zone>), _patch and _load ones (with a
// zombie_* among them: those, as a mod-tools zombies map is named). Its memory need beyond what a retail map has (Kino
// 103 MB, the memory is sized for ~110), with room to spare. A mod with more than one map names each from its mod.arena
// longname (the text its zones give that key: Mission Select has a row per map), or from the map's zone name
static void Mod_FindMaps(const std::string &dir, ModInfo &mod)
{
    static const char *const languages[] = { "en_", "fr_", "de_", "it_", "es_", "ru_", "pl_", "ja_", "ko_", "tw_", "cn_",
        "cz_", "br_", "th_", "le_", "fy_" };
    int count = 0;
    char **files = (char **)Sys_ListFiles((char *)dir.c_str(), (char *)"ff", nullptr, &count, 0);
    std::vector<std::string> maps;
    bool modZone = false;
    for ( int i = 0; files && i < count; ++i )
    {
        std::string name = files[i];
        if ( name.size() < 4 || I_stricmp(name.c_str() + name.size() - 3, ".ff") )
            continue;
        name.resize(name.size() - 3);
        if ( !I_stricmp(name.c_str(), "mod") )
        {
            modZone = true;
            continue;
        }
        // (a mod's other zones, as LinkerMod's game_mod loads them: mod1 .. mod7 after mod.ff, patch overrides)
        bool skip = !IsDvarName(name) || (name.size() > 6 && (!I_stricmp(name.c_str() + name.size() - 6, "_patch")))
                    || (name.size() > 5 && !I_stricmp(name.c_str() + name.size() - 5, "_load"))
                    || (name.size() > 15 && !I_stricmp(name.c_str() + name.size() - 15, "_patch_override"))
                    || (name.size() == 4 && !I_strnicmp(name.c_str(), "mod", 3) && name[3] >= '1' && name[3] <= '7');
        for ( const char *l : languages )
            skip = skip || !I_strnicmp(name.c_str(), l, 3);
        if ( !skip )
            maps.push_back(name);
    }
    if ( files )
        FS_FreeFileList((const char **)files);
    // (a zone its level_dependencies.csv loads with a map is no map: LinkerMod's game_mod)
    const std::set<std::string> dependencies = Mod_LevelDependencyZones(dir);
    maps.erase(std::remove_if(maps.begin(), maps.end(), [&](const std::string &m) {
        std::string l = m;
        for ( char &c : l )
            c = (char)tolower((unsigned char)c);
        return dependencies.count(l) != 0;
    }), maps.end());
    if ( maps.empty() )
        return;
    std::sort(maps.begin(), maps.end());
    // the maps the mod.arena lists, in its order (those with a zone here)
    std::vector<ModArenaMap> arena;
    for ( const ModArenaMap &a : Mod_ReadArena(dir + "/mod.arena") )
    {
        for ( const std::string &m : maps )
        {
            if ( !I_stricmp(m.c_str(), a.map.c_str()) )
            {
                arena.push_back({ m, a.longname });
                break;
            }
        }
    }
    if ( !arena.empty() )
    {
        maps.clear();
        for ( const ModArenaMap &a : arena )
            maps.push_back(a.map);
    }
    else
    {
        std::vector<std::string> zombie;
        for ( const std::string &m : maps )
        {
            if ( I_stristr(m.c_str(), "zombie") )
                zombie.push_back(m);
        }
        if ( !zombie.empty() )
            maps.swap(zombie);
    }
    mod.mapMod = true;
    // (more than one map: each one's longname key, and without "_CAPS", looked for in mod.ff, then in its own zone)
    std::vector<std::vector<std::string>> keys(maps.size());
    std::vector<std::string> allKeys;
    for ( size_t n = 0; maps.size() > 1 && n < arena.size() && n < maps.size(); ++n )
    {
        const std::string &key = arena[n].longname;
        if ( !IsDvarName(key) )
            continue;
        keys[n].push_back(key);
        if ( key.size() > 5 && !I_stricmp(key.c_str() + key.size() - 5, "_CAPS") )
            keys[n].push_back(key.substr(0, key.size() - 5));
        allKeys.insert(allKeys.end(), keys[n].begin(), keys[n].end());
    }
    const std::map<std::string, std::string> noText;
    const std::map<std::string, std::string> &modText =
        modZone && !allKeys.empty() ? Mod_ZoneStrings(dir + "/mod.ff", allKeys) : noText;
    unsigned long long biggest = 0;
    for ( size_t n = 0; n < maps.size(); ++n )
    {
        const std::string &m = maps[n];
        bool listed = false;
        for ( const ModMap &mm : mod.maps )
            listed = listed || !I_stricmp(mm.name.c_str(), m.c_str());
        if ( !listed )
        {
            std::string label = maps.size() > 1 ? "" : mod.name;
            for ( int zone = 0; zone < 2 && label.empty() && !keys[n].empty(); ++zone )
            {
                const std::map<std::string, std::string> &text =
                    zone == 0 ? modText : Mod_ZoneStrings(dir + "/" + m + ".ff", keys[n]);
                for ( const std::string &k : keys[n] )
                {
                    const auto it = text.find(k);
                    if ( label.empty() && it != text.end() && !it->second.empty() )
                        label = Mod_TitleCase(it->second);
                }
            }
            if ( label.empty() )
                label = Mod_TitleCase(Mod_MapNameFromZone(m));
            mod.maps.push_back({ m, label });
        }
        const unsigned long long need = Mod_ZoneMemory(dir + "/" + m + ".ff");
        biggest = need > biggest ? need : biggest;
    }
    const unsigned long long modNeed = modZone ? Mod_ModZonesMemory(dir) : 0;
    const long long extraMB = (long long)((biggest + modNeed) >> 20) - 110 + 64;
    if ( extraMB > 0 )
        mod.settings.push_back({ "bo1_mod_memory", va("%d", (int)extraMB), "", true });
}

// the retail zombie maps, in Mission Select's order
static const struct
{
    const char *map;
    const char *label;
} kRetailMaps[] = {
    { "zombie_theater", "Kino der Toten" },
    { "zombie_pentagon", "Five" },
    { "zombie_cosmodrome", "Ascension" },
    { "zombie_coast", "Call of the Dead" },
    { "zombie_temple", "Shangri-La" },
    { "zombie_moon", "Moon" },
    { "zombie_cod5_prototype", "Nacht der Untoten" },
    { "zombie_cod5_asylum", "Verruckt" },
    { "zombie_cod5_sumpf", "Shi No Numa" },
    { "zombie_cod5_factory", "Der Riese" },
};

static bool Mod_FileExists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if ( f )
        fclose(f);
    return f != nullptr;
}

// a zone mod's maps (ModInfo::zoneMod): the retail maps it has a maps/<map>.gsc for, in its folder or its .iwd files
// (wds: Kino der Toten, Five, Ascension and Moon; WAW weapons: the ten), as its scripts replace those maps' own; none,
// every map. And the memory its mod.ff needs on top of a retail map's (the memory is sized for ~110 MB, a retail map's
// most), with room to spare, as a map mod's (Mod_FindMaps)
static void Mod_FindZoneModMaps(const std::string &dir, ModInfo &mod)
{
    std::set<std::string> scripts;
    int count = 0;
    char **files = (char **)Sys_ListFiles((char *)dir.c_str(), (char *)"iwd", nullptr, &count, 0);
    for ( int i = 0; files && i < count; ++i )
    {
        unzFile uf = unzOpen((dir + "/" + files[i]).c_str());
        if ( !uf )
            continue;
        char name[256];
        unz_file_info info;
        for ( int r = unzGoToFirstFile(uf); r == UNZ_OK; r = unzGoToNextFile(uf) )
        {
            if ( unzGetCurrentFileInfo(uf, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK )
                break;
            for ( char *c = name; *c; ++c )
                *c = *c == '\\' ? '/' : (char)tolower((unsigned char)*c);
            if ( !strncmp(name, "maps/", 5) )
                scripts.insert(name);
        }
        unzClose(uf);
    }
    if ( files )
        FS_FreeFileList((const char **)files);
    if ( mod.maps.empty() )
    {
        for ( const auto &m : kRetailMaps )
        {
            const std::string path = std::string("maps/") + m.map + ".gsc";
            if ( scripts.count(path) || Mod_FileExists(dir + "/" + path) )
                mod.maps.push_back({ m.map, m.label });
        }
    }
    const unsigned long long modNeed = Mod_ModZonesMemory(dir);
    mod.settings.push_back({ "bo1_mod_memory", va("%d", (int)(modNeed >> 20) + 64), "", true });
}

// mod.json (a mod-tools mod's): "desc" and "vers"
static std::string Mod_JsonString(const std::string &json, const char *key)
{
    const std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if ( p == std::string::npos || (p = json.find(':', p + k.size())) == std::string::npos
        || (p = json.find('"', p)) == std::string::npos )
        return "";
    std::string value;
    for ( ++p; p < json.size() && json[p] != '"'; ++p )
    {
        if ( json[p] == '\\' && p + 1 < json.size() )
            ++p;
        value += json[p];
    }
    return value;
}

void Mods_Discover()
{
    std::vector<ModInfo> mods;
    char dir[256];
    Com_sprintf(dir, sizeof(dir), "%s/mods", fs_basepath ? (const char *)fs_basepath->current.string : ".");
    int count = 0;
    char **folders = (char **)Sys_ListFiles(dir, nullptr, nullptr, &count, 1);
    for ( int i = 0; folders && i < count; ++i )
    {
        const std::string dirName = folders[i];
        // (mods/_stack is the hooks every stack needs; a mod's name is a script path: letters, digits, '_', so a folder
        // with other characters in its name is the mod with them as '_': "Rust MW2" is Rust_MW2)
        if ( dirName.empty() || dirName[0] == '_' || dirName[0] == '.' )
            continue;
        char id[64];
        FS_ModIdForFolder(dirName.c_str(), id, sizeof(id));
        const std::string folder = id;
        if ( !IsDvarName(folder) )
            continue;
        bool taken = false;
        for ( const ModInfo &m : mods )
            taken = taken || !I_stricmp(m.folder.c_str(), folder.c_str());
        if ( taken )
        {
            Com_PrintWarning(10, "mods: mods/%s is the mod %s too: left out (rename one of the folders)\n",
                dirName.c_str(), folder.c_str());
            continue;
        }
        const std::string modDir = std::string(dir) + "/" + dirName;
        const std::string text = Mod_ReadFile(modDir + "/mod.txt");
        ModInfo mod;
        if ( !text.empty() && !Mod_Parse(folder, "mods/" + dirName + "/mod.txt", text, mod) )
        {
            Com_PrintWarning(10, "mods: mods/%s/mod.txt has errors, the mod is not in the Mods menu\n", dirName.c_str());
            continue;
        }
        if ( text.empty() )
        {
            mod.folder = folder;
            mod.name = dirName;
            mod.order = 100;
            mod.enabled = nullptr;
            mod.mapMod = false;
        }
        mod.dir = dirName;
        // a map mod (with or without a mod.txt): its zone files; mod.json's text unless the mod.txt has one. A mod-tools
        // mod that isn't a map (a mod.ff): a zone mod
        Mod_FindMaps(modDir, mod);
        if ( !mod.mapMod && Mod_FileExists(modDir + "/mod.ff") )
        {
            mod.zoneMod = true;
            Mod_FindZoneModMaps(modDir, mod);
        }
        if ( text.empty() && !mod.mapMod && !mod.zoneMod )
            continue;
        const std::string json = Mod_ReadFile(modDir + "/mod.json");
        if ( mod.description.empty() )
            mod.description = Mod_JsonString(json, "desc");
        if ( mod.version.empty() )
            mod.version = Mod_JsonString(json, "vers");
        mods.push_back(mod);
    }
    if ( folders )
        FS_FreeFileList((const char **)folders);
    std::stable_sort(mods.begin(), mods.end(), [](const ModInfo &a, const ModInfo &b) {
        return a.order != b.order ? a.order < b.order : I_stricmp(a.folder.c_str(), b.folder.c_str()) < 0;
    });
    // the menu's choices: archived dvars (players/config.cfg)
    for ( ModInfo &mod : mods )
    {
        mod.enabled = _Dvar_RegisterBool(Intern("bo1_mods_" + mod.folder), 0, 1u,
            Intern("mod: the Mods menu's " + mod.name + " (mods/" + mod.folder + ") is on"));
        Mod_RememberStartup(mod.settings);
        for ( ModOption &o : mod.options )
        {
            // (a dvar the engine has already, of another type: that one, as it is)
            const dvar_s *d = Dvar_FindVar(o.dvar.c_str());
            if ( d && !(d->flags & 0x4000) && d->type != DVAR_TYPE_STRING )
                o.var = d;
            else
                o.var = _Dvar_RegisterString(Intern(o.dvar), Intern(o.def), 1u,
                    Intern("mod: the Mods menu's " + mod.name + " option: " + o.label));
            for ( const ModValue &v : o.values )
                Mod_RememberStartup(v.settings);
        }
    }
    s_mods.swap(mods);
}

const std::vector<ModInfo> &Mods_List()
{
    return s_mods;
}

void Mods_SetMenuActive()
{
    s_menuActive = true;
}

bool Mods_MenuActive()
{
    return s_menuActive;
}

// ----- what the choices need -----

static std::map<std::string, bool> s_zoneInstalled;

static bool Mod_ZoneInstalled(const std::string &zone)
{
    auto it = s_zoneInstalled.find(zone);
    if ( it != s_zoneInstalled.end() )
        return it->second;
    const bool installed = DB_FileExists(zone.c_str(), FFD_DEFAULT) != 0;
    s_zoneInstalled[zone] = installed;
    return installed;
}

static bool Mod_NeedsMet(const std::vector<ModNeed> &needs, const char *map, std::vector<std::string> *missing)
{
    bool met = true;
    for ( const ModNeed &n : needs )
    {
        if ( map && !n.map.empty() && I_stricmp(n.map.c_str(), map) )
            continue;
        if ( Mod_ZoneInstalled(n.zone) )
            continue;
        met = false;
        if ( missing && std::find(missing->begin(), missing->end(), n.label) == missing->end() )
            missing->push_back(n.label);
    }
    return met;
}

// the extra zones to reserve for memory MB (PMem_Init: one or two zones add 256 MB, more 128 MB each)
static int Mod_ZonesForMemory(int mb)
{
    return mb <= 0 ? 0 : mb <= 256 ? 1 : (mb + 127) / 128;
}

static int Mod_MemoryForZones(int zones)
{
    return zones <= 0 ? 0 : zones <= 2 ? 256 : zones * 128;
}

static bool Mod_StartupMet(const ModSetting &s)
{
    if ( !s.startup )
        return true;
    if ( s.dvar == "bo1_mod_zones" )
        return (int)Words(s.value).size() <= Com_ModStartup_ZonesReserved();
    // (a map mod's memory, bo1_mod_memory MB: Mod_FindMaps)
    if ( s.dvar == "bo1_mod_memory" )
        return atoi(s.value.c_str()) <= Mod_MemoryForZones(Com_ModStartup_ZonesReserved());
    auto it = s_sessionStartup.find(s.dvar);
    const std::string session = it != s_sessionStartup.end() ? it->second : "";
    double have, want;
    if ( IsNumber(session, &have) && IsNumber(s.value, &want) )
        return have >= want;
    return !I_stricmp(session.c_str(), s.value.c_str());
}

static bool Mod_StartupMet(const std::vector<ModSetting> &settings)
{
    for ( const ModSetting &s : settings )
    {
        if ( !Mod_StartupMet(s) )
            return false;
    }
    return true;
}

bool Mods_IsEnabled(const ModInfo &mod)
{
    return mod.enabled && mod.enabled->current.enabled;
}

static bool Mod_OnMap(const ModInfo &mod, const char *map)
{
    if ( mod.maps.empty() )
        return true;
    for ( const ModMap &m : mod.maps )
    {
        if ( !I_stricmp(m.name.c_str(), map) )
            return true;
    }
    return false;
}

static std::string JoinLabels(const std::vector<std::string> &labels)
{
    std::string s;
    for ( size_t i = 0; i < labels.size(); ++i )
    {
        if ( i )
            s += i + 1 == labels.size() ? " and " : ", ";
        s += labels[i];
    }
    return s;
}

bool Mods_IsAvailable(const ModInfo &mod, std::string *why)
{
    std::vector<std::string> missing;
    bool any = false;
    if ( mod.maps.empty() )
    {
        any = Mod_NeedsMet(mod.needs, nullptr, &missing);
    }
    else
    {
        for ( const ModMap &m : mod.maps )
            any = Mod_NeedsMet(mod.needs, m.name.c_str(), &missing) || any;
    }
    if ( why )
        *why = missing.empty() ? "" : "Needs " + JoinLabels(missing) + (missing.size() > 1 ? ", which aren't" : ", which isn't")
            + " installed.";
    return any;
}

const ModInfo *Mods_MapModFor(const char *map)
{
    for ( const ModInfo &mod : s_mods )
    {
        if ( mod.mapMod && Mod_OnMap(mod, map) && !mod.maps.empty() )
            return &mod;
    }
    return nullptr;
}

std::string Mods_MapList(const ModInfo &mod)
{
    std::vector<std::string> labels;
    for ( const ModMap &m : mod.maps )
        labels.push_back(m.label);
    return labels.empty() ? "every map" : JoinLabels(labels);
}

int Mods_OptionValueIndex(const ModOption &option)
{
    if ( !option.var )
        return -1;
    const char *current = Dvar_DisplayableValue(option.var);
    double cur;
    const bool numeric = IsNumber(current, &cur);
    for ( size_t i = 0; i < option.values.size(); ++i )
    {
        double v;
        if ( !I_stricmp(option.values[i].value.c_str(), current)
            || (numeric && IsNumber(option.values[i].value, &v) && v == cur) )
            return (int)i;
    }
    return -1;
}

// the value whose settings a game on map uses: the chosen one, or, if this session or the install can't give it what
// it needs, the nearest one before it in the list that it can (the option's dvar keeps the player's choice)
static int Mod_EffectiveValue(const ModOption &option, const char *map)
{
    int i = Mods_OptionValueIndex(option);
    while ( i > 0
            && !(Mod_StartupMet(option.values[i].settings) && Mod_NeedsMet(option.values[i].needs, map, nullptr)) )
        --i;
    return i;
}

std::string Mods_RestartReason(const ModInfo &mod)
{
    if ( !Mods_IsAvailable(mod, nullptr) )
        return "";
    if ( !Mod_StartupMet(mod.settings) )
        return mod.name;
    std::vector<std::string> reasons;
    for ( const ModOption &o : mod.options )
    {
        const int i = Mods_OptionValueIndex(o);
        if ( i >= 0 && !Mod_StartupMet(o.values[i].settings) && Mod_NeedsMet(o.values[i].needs, nullptr, nullptr) )
            reasons.push_back(mod.name + " (" + o.label + ": " + o.values[i].label + ")");
    }
    return JoinLabels(reasons);
}

// several mods setting one dvar: the larger number, or every word of each (zone lists)
static void Mod_Merge(std::vector<std::pair<std::string, std::string>> &out, const std::string &dvar, const std::string &value)
{
    for ( auto &kv : out )
    {
        if ( I_stricmp(kv.first.c_str(), dvar.c_str()) )
            continue;
        double a, b;
        if ( IsNumber(kv.second, &a) && IsNumber(value, &b) )
        {
            if ( b > a )
                kv.second = value;
            return;
        }
        std::vector<std::string> words = Words(kv.second);
        for ( const std::string &w : Words(value) )
        {
            if ( std::find(words.begin(), words.end(), w) == words.end() )
                words.push_back(w);
        }
        kv.second.clear();
        for ( const std::string &w : words )
            kv.second += (kv.second.empty() ? "" : " ") + w;
        return;
    }
    out.push_back({ dvar, value });
}

bool Mods_SaveStartup(std::string *who)
{
    if ( who )
        who->clear();
    if ( Com_ModStartup_CommandLineMods() )
        return false;
    std::vector<std::pair<std::string, std::string>> startup;
    std::vector<std::string> restart;
    for ( const ModInfo &mod : s_mods )
    {
        // (a map mod too: what it needs, its memory, is set aside while it is on)
        if ( !Mods_IsEnabled(mod) || !Mods_IsAvailable(mod, nullptr) )
            continue;
        std::vector<const ModSetting *> settings;
        for ( const ModSetting &s : mod.settings )
            settings.push_back(&s);
        for ( const ModOption &o : mod.options )
        {
            const int i = Mods_OptionValueIndex(o);
            if ( i >= 0 && Mod_NeedsMet(o.values[i].needs, nullptr, nullptr) )
            {
                for ( const ModSetting &s : o.values[i].settings )
                    settings.push_back(&s);
            }
        }
        for ( const ModSetting *s : settings )
        {
            if ( s->startup )
                Mod_Merge(startup, s->dvar, s->value);
        }
        const std::string reason = Mods_RestartReason(mod);
        if ( !reason.empty() )
            restart.push_back(reason);
    }
    std::string text = "// BO1 Zombies: what the mods turned on in the Mods menu need when the game starts (the menu writes\n"
                       "// this file; src/qcommon/com_mods.cpp reads it at startup). Delete it to start without them.\n";
    int zones = 0;
    for ( const auto &kv : startup )
    {
        if ( kv.first == "bo1_mod_zones" )
            zones = std::max(zones, (int)Words(kv.second).size());
        else if ( kv.first == "bo1_mod_memory" )
            zones = std::max(zones, Mod_ZonesForMemory(atoi(kv.second.c_str())));
        else
            text += "set " + kv.first + " " + kv.second + "\n";
    }
    if ( zones )
        text += va("zones %d\n", zones);
    char path[512];
    Com_sprintf(path, sizeof(path), "%s/%s", fs_homepath ? (const char *)fs_homepath->current.string : ".",
        s_startupFile);
    const std::string old = Mod_ReadFile(path);
    if ( startup.empty() && old.empty() )
    {
        // (nothing to keep: no file, as before the menu)
    }
    else if ( old != text )
    {
        FILE *f = fopen(path, "wb");
        if ( f )
        {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            Com_Printf(10, "mods: wrote %s (%d settings for the next start)\n", s_startupFile, (int)startup.size());
        }
        else
        {
            Com_PrintWarning(10, "mods: can't write %s\n", path);
        }
    }
    if ( who )
        *who = JoinLabels(restart);
    return !restart.empty();
}

// ----- applying the choices when a game starts -----

// a dvar the menu's mods set, as it was before (lowercase name): its value, or that it didn't exist
struct ModRestore
{
    bool existed;
    std::string value;
};
static std::map<std::string, ModRestore> s_restore;

static std::string Lower(const std::string &s)
{
    std::string l = s;
    for ( char &c : l )
        c = (char)tolower((unsigned char)c);
    return l;
}

// the settings a game on mapName gets from mod: its own and its options' values (a value the game can't give: the
// nearest one before it in the list that it can, log says so)
static void Mod_AddSettings(const ModInfo &mod, const char *mapName, std::vector<std::pair<std::string, std::string>> &sets,
    bool log)
{
    std::vector<const ModSetting *> settings;
    for ( const ModSetting &s : mod.settings )
        settings.push_back(&s);
    for ( const ModOption &o : mod.options )
    {
        const int chosen = Mods_OptionValueIndex(o);
        const int i = Mod_EffectiveValue(o, mapName);
        if ( i < 0 )
            continue;
        if ( i != chosen && log )
            Com_PrintWarning(10, "mods: %s %s %s needs %s: this game uses the settings of %s\n", mod.name.c_str(),
                o.label.c_str(), o.values[chosen].label.c_str(),
                Mod_NeedsMet(o.values[chosen].needs, mapName, nullptr) ? "a restart of the game first"
                                                                       : "a map that isn't installed",
                o.values[i].label.c_str());
        for ( const ModSetting &s : o.values[i].settings )
            settings.push_back(&s);
    }
    for ( const ModSetting *s : settings )
    {
        if ( !s->startup && (s->map.empty() || !I_stricmp(s->map.c_str(), mapName)) )
            Mod_Merge(sets, s->dvar, s->value);
    }
}

// fs_mods and the dvars a game on map gets (front end: none); log prints why a mod or a value is left out
static void Mod_Compute(const char *mapName, std::string &fsMods, std::vector<std::pair<std::string, std::string>> &sets,
    bool log)
{
    fsMods.clear();
    sets.clear();
    if ( !*mapName || Com_IsSPFrontEndLevel(mapName) || Com_IsMenuLevel(mapName) )
        return;
    // the map's own mod (a map mod), first: its zones and files (mod.ff, .iwd) come from its folder. On or off (the
    // Mods menu's ON lists it in the zombies menu's Mission Select and sets its memory aside; "/map" plays it anyway)
    const ModInfo *mapMod = Mods_MapModFor(mapName);
    const ModInfo *zoneMod = nullptr; // (the game's one zone mod: its mod.ff loads with the level)
    if ( mapMod )
    {
        fsMods = mapMod->folder;
        if ( log && !Mod_StartupMet(mapMod->settings) )
            Com_PrintWarning(10, "mods: %s needs more memory than this start of the game set aside: %s\n",
                mapMod->name.c_str(), Mods_IsEnabled(*mapMod) ? "restart the game (the Mods menu has saved what it needs)"
                                                             : "turn it on in the Mods menu, then restart the game");
        Mod_AddSettings(*mapMod, mapName, sets, log);
    }
    for ( const ModInfo &mod : s_mods )
    {
        if ( mod.mapMod || !Mods_IsEnabled(mod) )
            continue;
        std::vector<std::string> missing;
        if ( mod.zoneMod && mapMod )
        {
            if ( log )
                Com_Printf(10, "mods: %s is not for %s (a map mod's map, with its own mod.ff): left out\n",
                    mod.name.c_str(), mapName);
            continue;
        }
        if ( mod.zoneMod && zoneMod )
        {
            if ( log )
                Com_PrintWarning(10, "mods: %s left out: a game has one mod with its own mod.ff (%s is on)\n",
                    mod.name.c_str(), zoneMod->name.c_str());
            continue;
        }
        if ( !Mod_OnMap(mod, mapName) )
        {
            if ( log )
                Com_Printf(10, "mods: %s is not for %s (it plays on %s): left out\n", mod.name.c_str(), mapName,
                    Mods_MapList(mod).c_str());
            continue;
        }
        if ( !Mod_NeedsMet(mod.needs, mapName, &missing) )
        {
            if ( log )
                Com_PrintWarning(10, "mods: %s needs %s on %s, not installed: left out\n", mod.name.c_str(),
                    JoinLabels(missing).c_str(), mapName);
            continue;
        }
        if ( !Mod_StartupMet(mod.settings) )
        {
            if ( log )
                Com_PrintWarning(10, "mods: %s needs a restart of the game first (Mods menu): left out\n",
                    mod.name.c_str());
            continue;
        }
        if ( mod.zoneMod )
            zoneMod = &mod;
        fsMods += (fsMods.empty() ? "" : " ") + mod.folder;
        Mod_AddSettings(mod, mapName, sets, log);
    }
}

// a dvar a mod's scripts made in the last game (SetDvar: an external dvar, not saved) that the mod's "clear" names
struct ModClearScan
{
    std::vector<std::string> patterns;
    std::vector<std::string> names;
};

static void Mod_ClearScan(const dvar_s *d, void *user)
{
    ModClearScan *scan = (ModClearScan *)user;
    if ( !(d->flags & 0x4000) || (d->flags & 1) || d->type != DVAR_TYPE_STRING || !d->current.string
        || !*(const char *)d->current.string )
        return;
    const char *name = d->name;
    for ( const std::string &p : scan->patterns )
    {
        const bool prefix = !p.empty() && p.back() == '*';
        if ( prefix ? !I_strnicmp(name, p.c_str(), (int)p.size() - 1) : !I_stricmp(name, p.c_str()) )
        {
            scan->names.push_back(name);
            return;
        }
    }
}

void Mods_ApplyForMap(const char *map)
{
    if ( !s_menuActive || Com_ModStartup_CommandLineMods() || !map )
        return;
    const std::string mapCopy = map; // (an Info_ValueForKey buffer)
    const char *mapName = mapCopy.c_str();
    std::string fsMods;
    std::vector<std::pair<std::string, std::string>> sets;
    Mod_Compute(mapName, fsMods, sets, true);
    // what the last game set and this one doesn't: back to how it was
    for ( auto it = s_restore.begin(); it != s_restore.end(); )
    {
        bool kept = false;
        for ( const auto &kv : sets )
            kept = kept || Lower(kv.first) == it->first;
        if ( kept )
        {
            ++it;
            continue;
        }
        dvar_s *d = (dvar_s *)Dvar_FindVar(it->first.c_str());
        // (a dvar first made by the mod's value and registered since: its default)
        if ( d && !it->second.existed && !(d->flags & 0x4000) )
            Dvar_Reset(d, DVAR_SOURCE_INTERNAL);
        else if ( d )
            Dvar_SetFromString(d, (char *)it->second.value.c_str());
        it = s_restore.erase(it);
    }
    // what the mods' scripts set in the last game (every mod's, on or off): empty, as when the game starts
    ModClearScan scan;
    for ( const ModInfo &mod : s_mods )
        scan.patterns.insert(scan.patterns.end(), mod.clears.begin(), mod.clears.end());
    if ( !scan.patterns.empty() )
        Dvar_ForEach(Mod_ClearScan, &scan);
    for ( const std::string &name : scan.names )
        Dvar_SetFromStringByName(name.c_str(), (char *)"");
    std::string summary;
    for ( const auto &kv : sets )
    {
        const dvar_s *d = Dvar_FindVar(kv.first.c_str());
        // (a saved setting would keep the mod's value in players/config.cfg)
        if ( d && (d->flags & 1) )
        {
            Com_PrintWarning(10, "mods: %s is a saved setting, a mod can't set it (%s skipped)\n", kv.first.c_str(),
                kv.second.c_str());
            continue;
        }
        const std::string key = Lower(kv.first);
        if ( !s_restore.count(key) )
            s_restore[key] = { d != nullptr, d ? Dvar_DisplayableValue(d) : "" };
        Dvar_SetFromStringByName(kv.first.c_str(), (char *)kv.second.c_str());
        summary += " " + kv.first + " " + kv.second + ";";
    }
    if ( fs_modsVar && I_stricmp(fs_modsVar->current.string, fsMods.c_str()) )
        Dvar_SetString((dvar_s *)fs_modsVar, fsMods.c_str());
    if ( !fsMods.empty() || !summary.empty() )
        Com_Printf(10, "mods: %s: fs_mods \"%s\";%s\n", mapName, fsMods.c_str(), summary.c_str());
}

// ----- bo1_mods [map]: the mods, and what a game on map would get -----

static void Mods_List_f()
{
    Mods_Discover();
    Com_Printf(0, "mods: %d in mods/ (a mod.txt, or a map's zone)%s; extra zones reserved %d%s\n", (int)s_mods.size(),
        Com_ModStartup_CommandLineMods() ? " (the command line picks the mods: the menu's choices are not used)" : "",
        Com_ModStartup_ZonesReserved(), s_menuActive ? "" : "; the menu's choices apply from the main menu on");
    for ( const ModInfo &mod : s_mods )
    {
        std::string why;
        const bool available = Mods_IsAvailable(mod, &why);
        const std::string restart = Mods_RestartReason(mod);
        Com_Printf(0, "  %-10s %-3s %s (order %d; %s)%s%s%s\n", mod.folder.c_str(),
            Mods_IsEnabled(mod) ? "ON" : "off", mod.name.c_str(), mod.order,
            ((mod.mapMod ? "a map: " : "maps: ") + Mods_MapList(mod)).c_str(),
            available ? "" : " - ", why.c_str(), restart.empty() ? "" : " - needs a restart");
        for ( const ModSetting &s : mod.settings )
        {
            if ( s.dvar == "bo1_mod_memory" )
                Com_Printf(0, "      memory: %s MB more than an original map (bo1_mod_memory %s: %d extra zones)\n",
                    s.value.c_str(), s.value.c_str(), Mod_ZonesForMemory(atoi(s.value.c_str())));
        }
        for ( const ModOption &o : mod.options )
        {
            const int i = Mods_OptionValueIndex(o);
            Com_Printf(0, "      %s = %s (%s)\n", o.dvar.c_str(), o.var ? Dvar_DisplayableValue(o.var) : "",
                i >= 0 ? o.values[i].label.c_str() : "not one of its values");
        }
    }
    if ( Cmd_Argc() > 1 )
    {
        std::string fsMods;
        std::vector<std::pair<std::string, std::string>> sets;
        Mod_Compute(Cmd_Argv(1), fsMods, sets, true);
        std::string summary;
        for ( const auto &kv : sets )
            summary += " " + kv.first + " " + kv.second + ";";
        Com_Printf(0, "mods: a game on %s: fs_mods \"%s\";%s\n", Cmd_Argv(1), fsMods.c_str(), summary.c_str());
    }
}

void Mods_RegisterCommands()
{
    static cmd_function_s listCmd;
    static bool registered;
    if ( registered )
        return;
    registered = true;
    Cmd_AddCommandInternal("bo1_mods", Mods_List_f, &listCmd);
}
