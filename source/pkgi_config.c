#include "pkgi_config.h"
#include "pkgi.h"
#include "pkgi_db.h"

#include <stddef.h>

static char* skipnonws(char* text, char* end)
{
    while (text < end && *text != ' ' && *text != '\n' && *text != '\r')
    {
        text++;
    }
    return text;
}

static char* skipws(char* text, char* end)
{
    while (text < end && (*text == ' ' || *text == '\n' || *text == '\r'))
    {
        text++;
    }
    return text;
}

static DbSort parse_sort(const char* value, DbSort sort)
{
    if (pkgi_stricmp(value, "title") == 0)
    {
        return SortByTitle;
    }
    else if (pkgi_stricmp(value, "region") == 0)
    {
        return SortByRegion;
    }
    else if (pkgi_stricmp(value, "name") == 0)
    {
        return SortByName;
    }
    else if (pkgi_stricmp(value, "size") == 0)
    {
        return SortBySize;
    }
    else
    {
        return sort;
    }
}

static DbSortOrder parse_order(const char* value, DbSortOrder order)
{
    if (pkgi_stricmp(value, "asc") == 0)
    {
        return SortAscending;
    }
    else if (pkgi_stricmp(value, "desc") == 0)
    {
        return SortDescending;
    }
    else
    {
        return order;
    }
}

static DbSortOrder parse_filter(char* value, uint32_t filter)
{
    uint32_t result = 0;

    char* start = value;
    for (;;)
    {
        char ch = *value;
        if (ch == 0 || ch == ',')
        {
            *value = 0;
            if (pkgi_stricmp(start, "ASA") == 0)
            {
                result |= DbFilterRegionASA;
            }
            else if (pkgi_stricmp(start, "EUR") == 0)
            {
                result |= DbFilterRegionEUR;
            }
            else if (pkgi_stricmp(start, "JPN") == 0)
            {
                result |= DbFilterRegionJPN;
            }
            else if (pkgi_stricmp(start, "USA") == 0)
            {
                result |= DbFilterRegionUSA;
            }
            else
            {
                return filter;
            }
            if (ch == 0)
            {
                break;
            }
            value++;
            start = value;
        }
        else
        {
            value++;
        }
    }

    return result;
}

void pkgi_load_config(Config* config, char* refresh_url, uint32_t refresh_len)
{
    refresh_url[0] = 0;
    config->sort = SortByName;
    config->order = SortAscending;
    config->filter = DbFilterAll;
    config->version_check = 1;
    config->dl_mode_background = 0;
    config->music = 1;
    config->content = 0;
    config->allow_refresh = 0;
    config->download_folder[0] = 0;
    pkgi_strncpy(config->language, 3, pkgi_get_user_language());

    char data[4096];
    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/config.txt", pkgi_get_config_folder());
    LOG("config location: %s", path);

    int loaded = pkgi_load(path, data, sizeof(data) - 1);
    if (loaded > 0)
    {
        data[loaded] = '\n';

        LOG("config.txt loaded, parsing");
        char* text = data;
        char* end = data + loaded + 1;

        if (loaded > 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
        {
            text += 3;
        }

        while (text < end)
        {
            char* key = text;

            text = skipnonws(text, end);
            if (text == end) break;

            *text++ = 0;

            text = skipws(text, end);
            if (text == end) break;

            char* value = text;

            text = skipnonws(text, end);
            if (text == end) break;

            *text++ = 0;

            text = skipws(text, end);

            if (pkgi_stricontains(key, "url"))
            {
                for (int i = 0; i < MAX_CONTENT_TYPES; i++)
                    if (pkgi_stricmp(key+3, pkgi_content_tag(i)) == 0)
                    {
                        pkgi_strncpy(refresh_url + refresh_len*i, refresh_len, value);
                        config->allow_refresh = 1;
                    }
            }
            else if (pkgi_stricontains(key, "db_format_"))
            {
                /* Per-content database format hint: nps | pkgi.
                 * Legacy dbformat.txt takes priority when present. */
                for (int i = 0; i < MAX_CONTENT_TYPES; i++)
                    if (pkgi_stricmp(key + 9, pkgi_content_tag(i)) == 0)
                        pkgi_db_set_format_hint(i, value);
            }
            else if (pkgi_stricmp(key, "sort") == 0)
            {
                config->sort = parse_sort(value, SortByName);
            }
            else if (pkgi_stricmp(key, "order") == 0)
            {
                config->order = parse_order(value, SortAscending);
            }
            else if (pkgi_stricmp(key, "filter") == 0)
            {
                config->filter = parse_filter(value, DbFilterAll);
            }
            else if (pkgi_stricmp(key, "no_version_check") == 0)
            {
                config->version_check = 0;
            }
            else if (pkgi_stricmp(key, "dl_mode_background") == 0)
            {
                config->dl_mode_background = 1;
            }
            else if (pkgi_stricmp(key, "no_music") == 0)
            {
                config->music = 0;
            }
            else if (pkgi_stricmp(key, "content") == 0)
            {
                config->content = (uint8_t)pkgi_strtoll(value);
            }
            else if (pkgi_stricmp(key, "language") == 0)
            {
                pkgi_strncpy(config->language, 2, value);
            }
            else if (pkgi_stricmp(key, "download_folder") == 0)
            {
                /* Custom download folder (PKGi Remastered). Must be an
                 * absolute /dev_hdd0 path; anything else is ignored. */
                char prefix[16];
                pkgi_strncpy(prefix, sizeof(prefix), value + 1);
                prefix[8] = 0;
                if (value[0] == '/' && pkgi_stricmp(prefix, "dev_hdd0") == 0)
                    pkgi_strncpy(config->download_folder,
                                 sizeof(config->download_folder), value);
            }
        }
    }
    else
    {
        /* Default NPS database URLs (new-style config, PKGi Remastered). */
        static const char* default_urls[MAX_CONTENT_TYPES] = {
            "http://nopaystation.com/tsv/PS3_GAMES.tsv",   /* games */
            "http://nopaystation.com/tsv/PS3_DLCS.tsv",    /* dlcs */
            "http://nopaystation.com/tsv/PS3_THEMES.tsv",  /* themes */
            "http://nopaystation.com/tsv/PS3_AVATARS.tsv", /* avatars */
            "http://nopaystation.com/tsv/PS3_DEMOS.tsv",   /* demos */
        };

        for (int i = 0; i < MAX_CONTENT_TYPES; i++)
        {
            if (default_urls[i] != NULL)
            {
                pkgi_strncpy(refresh_url + refresh_len * i, refresh_len,
                             default_urls[i]);
                config->allow_refresh = 1;
            }
        }

        config->filter = DbFilterAllRegions | DbFilterAllContent;
        config->dl_mode_background = 1;
        config->music = 0;

        LOG("no config.txt, using NPS default URLs");
    }
    if (config->content == 0)
    {
        config->filter |= DbFilterAllContent;
    }
    else
    {
        config->filter |= (128 << config->content);
    }
}

const char* pkgi_content_tag(ContentType content)
{
    switch (content)
    {
    case ContentGame: return "_games";
    case ContentDLC: return "_dlcs";
    case ContentTheme: return "_themes";
    case ContentAvatar: return "_avatars";
    case ContentDemo: return "_demos";
    case ContentUpdate: return "_updates";
    case ContentEmulator: return "_emulators";
    case ContentApp: return "_apps";
    case ContentTool: return "_tools";
    default: return "";
    }
}

/* ------------------------------------------------------------------ */
/* Configuration profiles (PKGi Remastered).                          */
/*                                                                    */
/* Profiles live in <config>/profiles/*.txt and use the same syntax   */
/* as config.txt. Loading is transactional: the file is parsed into   */
/* a scratch Config first; on any error the previous config stays.    */
/* ------------------------------------------------------------------ */

#define PKGI_PROFILE_MAX 8
static char profile_names[PKGI_PROFILE_MAX][64];
static int profile_count;

static int profile_list(void)
{
    profile_count = 0;

    char folder[256];
    pkgi_snprintf(folder, sizeof(folder), "%s/profiles", pkgi_get_config_folder());

    /* The PS3 cannot list folders without a full scan; use known
     * names + optional user files discovered via pkgi_load probes. */
    static const char* known[] = { "nps", "custom", "homebrew" };

    for (int i = 0; i < 3 && profile_count < PKGI_PROFILE_MAX; i++)
    {
        char path[256];
        pkgi_snprintf(path, sizeof(path), "%s/%s.txt", folder, known[i]);
        if (pkgi_get_size(path) > 0)
        {
            pkgi_strncpy(profile_names[profile_count], sizeof(profile_names[0]), known[i]);
            profile_count++;
        }
    }

    return profile_count;
}

int pkgi_config_profile_count(void)
{
    if (profile_count == 0)
        profile_list();
    return profile_count;
}

const char* pkgi_config_profile_name(int index)
{
    if (index < 0 || index >= profile_count)
        return NULL;
    return profile_names[index];
}

int pkgi_config_load_profile(const char* name, Config* config,
                             char* refresh_url, uint32_t refresh_len,
                             char* error, uint32_t error_size)
{
    char folder[256];
    char path[256];
    char data[4096];

    pkgi_snprintf(folder, sizeof(folder), "%s/profiles", pkgi_get_config_folder());
    pkgi_snprintf(path, sizeof(path), "%s/%s.txt", folder, name);

    int loaded = pkgi_load(path, data, sizeof(data) - 1);
    if (loaded <= 0)
    {
        pkgi_snprintf(error, error_size, "Cannot read profile: %s", name);
        return 0;
    }

    /* Parse into scratch values first: on error, caller keeps old config. */
    Config parsed = *config;
    char urls[MAX_CONTENT_TYPES][256];
    memset(urls, 0, sizeof(urls));

    /* Reuse the main parser by saving to a temp file is not possible on
     * all setups; instead parse inline with the same loop as config.txt. */
    data[loaded] = '\n';
    char* text = data;
    char* end = data + loaded + 1;

    if (loaded > 3 && (uint8_t)text[0] == 0xef && (uint8_t)text[1] == 0xbb && (uint8_t)text[2] == 0xbf)
        text += 3;

    int saw_error = 0;
    while (text < end && !saw_error)
    {
        char* key = text;
        text = skipnonws(text, end);
        if (text == end) break;
        *text++ = 0;
        text = skipws(text, end);
        if (text == end) break;
        char* value = text;
        text = skipnonws(text, end);
        if (text == end) break;
        *text++ = 0;
        text = skipws(text, end);

        int known_key = 0;
        if (pkgi_stricmp(key, "sort") == 0)
        {
            known_key = 1;
            DbSort s = parse_sort(value, parsed.sort);
            if (s == parsed.sort && pkgi_stricmp(value, "name") != 0 &&
                pkgi_stricmp(value, "title") != 0 && pkgi_stricmp(value, "region") != 0 &&
                pkgi_stricmp(value, "size") != 0)
                saw_error = 1;
            else
                parsed.sort = s;
        }
        else if (pkgi_stricmp(key, "order") == 0)
        {
            known_key = 1;
            if (pkgi_stricmp(value, "asc") == 0)
                parsed.order = SortAscending;
            else if (pkgi_stricmp(value, "desc") == 0)
                parsed.order = SortDescending;
            else
                saw_error = 1;
        }
        else if (pkgi_stricmp(key, "filter") == 0)
        {
            known_key = 1;
            uint32_t f = parse_filter(value, 0xFFFFFFFF);
            if (f == 0xFFFFFFFF)
                saw_error = 1;
            else
                parsed.filter = f;
        }
        else if (pkgi_stricmp(key, "content") == 0)
        {
            known_key = 1;
            int64_t c = pkgi_strtoll(value);
            if (c < 0 || c >= MAX_CONTENT_TYPES)
                saw_error = 1;
            else
                parsed.content = (uint8_t)c;
        }
        else if (pkgi_stricmp(key, "dl_mode_background") == 0)
        {
            known_key = 1;
            parsed.dl_mode_background = 1;
        }
        else if (pkgi_stricmp(key, "no_music") == 0)
        {
            known_key = 1;
            parsed.music = 0;
        }
        else if (pkgi_stricmp(key, "no_version_check") == 0)
        {
            known_key = 1;
            parsed.version_check = 0;
        }
        else if (pkgi_stricmp(key, "language") == 0)
        {
            known_key = 1;
            pkgi_strncpy(parsed.language, 2, value);
        }
        else if (pkgi_stricontains(key, "url"))
        {
            known_key = 1;
            for (int i = 0; i < MAX_CONTENT_TYPES; i++)
            {
                if (pkgi_stricmp(key + 3, pkgi_content_tag(i)) == 0)
                {
                    if (!pkgi_validate_url(value))
                    {
                        saw_error = 1;
                        break;
                    }
                    pkgi_strncpy(urls[i], sizeof(urls[0]), value);
                    parsed.allow_refresh = 1;
                }
            }
        }
        else if (pkgi_stricontains(key, "db_format_"))
        {
            known_key = 1;
            if (pkgi_stricmp(value, "nps") != 0 && pkgi_stricmp(value, "pkgi") != 0)
                saw_error = 1;
        }

        if (!known_key)
            saw_error = 1;
    }

    if (saw_error)
    {
        pkgi_snprintf(error, error_size, "Invalid setting in profile: %s", name);
        return 0;
    }

    /* Commit: copy parsed config and URLs back. */
    *config = parsed;
    for (int i = 0; i < MAX_CONTENT_TYPES; i++)
    {
        if (urls[i][0] != 0)
            pkgi_strncpy(refresh_url + refresh_len * i, refresh_len, urls[i]);
    }

    /* Format hints must be reset before the db reload picks them up. */
    for (int i = 0; i < MAX_CONTENT_TYPES; i++)
        pkgi_db_set_format_hint(i, "");

    LOG("loaded profile %s", name);
    return 1;
}

static const char* sort_str(DbSort sort)
{
    switch (sort)
    {
    case SortByTitle: return "title";
    case SortByRegion: return "region";
    case SortByName: return "name";
    case SortBySize: return "size";
    default: return "";
    }
}

static const char* order_str(DbSortOrder order)
{
    switch (order)
    {
    case SortAscending: return "asc";
    case SortDescending: return "desc";
    default: return "";
    }
}

void pkgi_save_config(const Config* config, const char* update_url, uint32_t update_len)
{
    char data[4096];
    int len = 0;

    for (int i = 0; i < MAX_CONTENT_TYPES; i++)
    {
        const char* tmp_url = update_url + update_len*i;
        if (update_url && tmp_url[0] != 0)
        {
            len += pkgi_snprintf(data + len, sizeof(data) - len, "url%s %s\n", pkgi_content_tag(i), tmp_url);
        }
    }
    len += pkgi_snprintf(data + len, sizeof(data) - len, "language %s\n", config->language);
    len += pkgi_snprintf(data + len, sizeof(data) - len, "content %d\n", config->content);
    len += pkgi_snprintf(data + len, sizeof(data) - len, "sort %s\n", sort_str(config->sort));
    len += pkgi_snprintf(data + len, sizeof(data) - len, "order %s\n", order_str(config->order));
    len += pkgi_snprintf(data + len, sizeof(data) - len, "filter ");
    const char* sep = "";
    if (config->filter & DbFilterRegionASA)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "%sASA", sep);
        sep = ",";
    }
    if (config->filter & DbFilterRegionEUR)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "%sEUR", sep);
        sep = ",";
    }
    if (config->filter & DbFilterRegionJPN)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "%sJPN", sep);
        sep = ",";
    }
    if (config->filter & DbFilterRegionUSA)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "%sUSA", sep);
        sep = ",";
    }
    len += pkgi_snprintf(data + len, sizeof(data) - len, "\n");

    if (!config->version_check)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "no_version_check 1\n");
    }

    if (config->dl_mode_background)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "dl_mode_background 1\n");
    }

    if (!config->music)
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "no_music 1\n");
    }

    if (config->download_folder[0])
    {
        len += pkgi_snprintf(data + len, sizeof(data) - len, "download_folder %s\n",
                             config->download_folder);
    }

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/config.txt", pkgi_get_config_folder());

    if (pkgi_save(path, data, len))
    {
        LOG("saved config.txt");
    }
    else
    {
        LOG("cannot save config.txt");
    }
}