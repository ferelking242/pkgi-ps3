#include <mini18n.h>
#include "pkgi_menu.h"
#include "pkgi_config.h"
#include "pkgi_style.h"
#include "pkgi.h"

static int menu_search_clear;

static Config   menu_config;
static uint32_t menu_selected;
static int      menu_allow_refresh;

static MenuResult menu_result;

static int32_t menu_width;
static int32_t menu_delta;
static int32_t pkgi_menu_width = 0;

typedef enum {
    MenuSearch,
    MenuSearchClear,
    MenuText,
    MenuSort,
    MenuFilter,
    MenuRefresh,
    MenuMode,
    MenuUpdate,
    MenuMusic,
    MenuContent,
    MenuLayout,
    MenuLoadConfig,
    MenuFolder
} MenuType;

typedef struct {
    MenuType type;
    const char* text;
    uint32_t value;
} MenuEntry;

static MenuEntry menu_entries[] =
{
    { MenuLayout, "Display", 0 },
    { MenuSearch, "Search...", 0 },
    { MenuSearchClear, PKGI_UTF8_CLEAR " clear", 0 },

    { MenuText, "Sort by:", 0 },
    { MenuSort, "Title", SortByTitle },
    { MenuSort, "Region", SortByRegion },
    { MenuSort, "Name", SortByName },
    { MenuSort, "Size", SortBySize },

    { MenuText, "Content:", 0 },
    { MenuContent, "All", 0 },

    { MenuText, "Regions:", 0 },
    { MenuFilter, "Asia", DbFilterRegionASA },
    { MenuFilter, "Europe", DbFilterRegionEUR },
    { MenuFilter, "Japan", DbFilterRegionJPN },
    { MenuFilter, "USA", DbFilterRegionUSA },

    { MenuText, "Status:", 0 },
    { MenuFilter, "Installed", DbFilterInstalled },
    { MenuFilter, "Not installed", DbFilterMissing },

    { MenuText, "Options:", 0 },
    { MenuMode, "Back. DL", 1 },
    { MenuMusic, "Music", 1 },
    { MenuUpdate, "Updates", 1 },

    { MenuLoadConfig, "Load configuration", 0 },
    { MenuFolder, "Download folder", 0 },
    { MenuRefresh, "Refresh...", 0 },
};

static MenuEntry content_entries[] = 
{
    { MenuFilter, "All", DbFilterAllContent },
    { MenuFilter, "Games", DbFilterContentGame },
    { MenuFilter, "DLCs", DbFilterContentDLC },
    { MenuFilter, "Themes", DbFilterContentTheme },
    { MenuFilter, "Avatars", DbFilterContentAvatar },
    { MenuFilter, "Demos", DbFilterContentDemo },
    { MenuFilter, "Updates", DbFilterContentUpdate },
    { MenuFilter, "Emulators", DbFilterContentEmulator },
    { MenuFilter, "Apps", DbFilterContentApp },
    { MenuFilter, "Tools", DbFilterContentTool }
};

static int menu_scroll_y;

static int menu_entry_visible(const MenuEntry* entry)
{
    return !((entry->type == MenuSearchClear && !menu_search_clear) ||
             (entry->type == MenuRefresh && !menu_allow_refresh));
}

static int menu_entry_gap(const MenuEntry* entry, int font_height)
{
    return entry->type == MenuText || entry->type == MenuRefresh ||
           entry->type == MenuLoadConfig || entry->type == MenuFolder
        ? font_height : 0;
}

static void menu_scroll_to_selection(int font_height)
{
    int y = PKGI_MENU_TOP_PADDING;
    int selected_y = y;
    for (uint32_t i = 0; i < PKGI_COUNTOF(menu_entries); i++)
    {
        const MenuEntry* entry = menu_entries + i;
        if (!menu_entry_visible(entry))
            continue;
        y += menu_entry_gap(entry, font_height);
        if (i == menu_selected)
            selected_y = y;
        y += font_height;
    }

    int viewport_bottom = PKGI_MENU_HEIGHT - 12;
    int viewport_top = PKGI_MENU_TOP_PADDING;
    if (selected_y - menu_scroll_y < viewport_top)
        menu_scroll_y = selected_y - viewport_top;
    else if (selected_y + font_height - menu_scroll_y > viewport_bottom)
        menu_scroll_y = selected_y + font_height - viewport_bottom;
    if (menu_scroll_y < 0)
        menu_scroll_y = 0;

    int max_scroll = y - viewport_bottom;
    if (max_scroll < 0)
        max_scroll = 0;
    if (menu_scroll_y > max_scroll)
        menu_scroll_y = max_scroll;
}

int pkgi_menu_is_open(void)
{
    return menu_width != 0;
}

MenuResult pkgi_menu_result()
{
    return menu_result;
}

void pkgi_menu_get(Config* config)
{
    *config = menu_config;
}

static void set_max_width(const MenuEntry* entries, int size)
{
    for (int i = 0; i < size; i++)
    {
        int width = pkgi_text_width(entries[i].text) +
                    PKGI_MENU_LEFT_PADDING * 2 + PKGI_FONT_WIDTH + 6;
        if (entries[i].type == MenuLayout)
        {
            const char* modes[] = { _("Grid"), _("List") };
            for (uint32_t mode = 0; mode < PKGI_COUNTOF(modes); mode++)
            {
                char text[64];
                pkgi_snprintf(text, sizeof(text), "%s %s: %s",
                              PKGI_UTF8_CHECK_ON, entries[i].text, modes[mode]);
                int rendered_width = pkgi_text_width(text) +
                    PKGI_MENU_LEFT_PADDING * 2 + PKGI_FONT_WIDTH + 6;
                if (rendered_width > width)
                    width = rendered_width;
            }
        }

        if (width > pkgi_menu_width)
        {
            pkgi_menu_width = width;
        }
    }
}

void pkgi_menu_start(int search_clear, const Config* config)
{
    menu_search_clear = search_clear;
    menu_width = 1;
    menu_delta = 1;
    menu_config = *config;
    menu_allow_refresh = config->allow_refresh;

    menu_entries[0].text = _("Display");
    menu_entries[1].text = _("Search...");
    menu_entries[3].text = _("Sort by:");
    menu_entries[4].text = _("Title");
    menu_entries[5].text = _("Region");
    menu_entries[6].text = _("Name");
    menu_entries[7].text = _("Size");
    menu_entries[8].text = _("Content:");
    menu_entries[9].text = _("All");
    menu_entries[10].text = _("Regions:");
    menu_entries[11].text = _("Asia");
    menu_entries[12].text = _("Europe");
    menu_entries[13].text = _("Japan");
    menu_entries[14].text = _("USA");
    menu_entries[15].text = _("Status:");
    menu_entries[16].text = _("Installed");
    menu_entries[17].text = _("Not installed");
    menu_entries[18].text = _("Options:");
    menu_entries[19].text = _("Back. DL");
    menu_entries[20].text = _("Music");
    menu_entries[21].text = _("Updates");
    menu_entries[22].text = _("Load configuration");
    menu_entries[23].text = _("Download folder");
    menu_entries[24].text = _("Refresh...");

    content_entries[0].text = _("All");
    content_entries[1].text = _("Games");
    content_entries[2].text = _("DLCs");
    content_entries[3].text = _("Themes");
    content_entries[4].text = _("Avatars");
    content_entries[5].text = _("Demos");
    content_entries[6].text = _("Updates");
    content_entries[7].text = _("Emulators");
    content_entries[8].text = _("Apps");
    content_entries[9].text = _("Tools");

    if (pkgi_menu_width)
        return;

    pkgi_menu_width = PKGI_MENU_WIDTH;
    set_max_width(menu_entries, PKGI_COUNTOF(menu_entries));
    set_max_width(content_entries, PKGI_COUNTOF(content_entries));
    if (pkgi_menu_width > VITA_WIDTH - 2 * PKGI_MAIN_HMARGIN)
        pkgi_menu_width = VITA_WIDTH - 2 * PKGI_MAIN_HMARGIN;
}

int pkgi_do_menu(pkgi_input* input)
{
    if (menu_delta != 0)
    {
        menu_width += menu_delta * (int32_t)(input->delta * PKGI_ANIMATION_SPEED/ 3000);

        if (menu_delta < 0 && menu_width <= 0)
        {
            menu_width = 0;
            menu_delta = 0;
            return 0;
        }
        else if (menu_delta > 0 && menu_width >= pkgi_menu_width)
        {
            menu_width = pkgi_menu_width;
            menu_delta = 0;
        }
    }

    if (menu_width != 0)
    {
        pkgi_draw_fill_rect_z(VITA_WIDTH - (menu_width + PKGI_MAIN_HMARGIN), PKGI_MAIN_VMARGIN, PKGI_MENU_Z, menu_width, PKGI_MENU_HEIGHT, PKGI_COLOR_MENU_BACKGROUND);
        pkgi_draw_rect_z(VITA_WIDTH - (menu_width + PKGI_MAIN_HMARGIN), PKGI_MAIN_VMARGIN, PKGI_MENU_Z, menu_width, PKGI_MENU_HEIGHT, PKGI_COLOR_MENU_BORDER);
    }

    if (input->active & PKGI_BUTTON_UP)
    {
        do {
            if (menu_selected == 0)
            {
                menu_selected = PKGI_COUNTOF(menu_entries) - 1;
            }
            else
            {
                menu_selected--;
            }
        } while (!menu_entry_visible(menu_entries + menu_selected) ||
                 menu_entries[menu_selected].type == MenuText);
    }

    if (input->active & PKGI_BUTTON_DOWN)
    {
        do {
            if (menu_selected == PKGI_COUNTOF(menu_entries) - 1)
            {
                menu_selected = 0;
            }
            else
            {
                menu_selected++;
            }
        } while (!menu_entry_visible(menu_entries + menu_selected) ||
                 menu_entries[menu_selected].type == MenuText);
    }


    if (input->pressed & pkgi_cancel_button())
    {
        menu_result = MenuResultCancel;
        menu_delta = -1;
        return 1;
    }
    else if (input->pressed & PKGI_BUTTON_T)
    {
        menu_result = MenuResultAccept;
        menu_delta = -1;
        return 1;
    }
    else if (input->pressed & pkgi_ok_button())
    {
        MenuType type = menu_entries[menu_selected].type;
        if (type == MenuSearch)
        {
            menu_result = MenuResultSearch;
            menu_delta = -1;
            return 1;
        }
        if (type == MenuSearchClear)
        {
            menu_selected--;
            menu_result = MenuResultSearchClear;
            menu_delta = -1;
            return 1;
        }
        else if (type == MenuRefresh)
        {
            menu_result = MenuResultRefresh;
            menu_delta = -1;
            return 1;
        }
        else if (type == MenuLoadConfig)
        {
            menu_result = MenuResultLoadConfig;
            menu_delta = -1;
            return 1;
        }
        else if (type == MenuFolder)
        {
            menu_result = MenuResultEditFolder;
            menu_delta = -1;
            return 1;
        }
        else if (type == MenuSort)
        {
            DbSort value = (DbSort)menu_entries[menu_selected].value;
            if (menu_config.sort == value)
            {
                menu_config.order = menu_config.order == SortAscending ? SortDescending : SortAscending;
            }
            else
            {
                menu_config.sort = value;
            }
        }
        else if (type == MenuFilter)
        {
            menu_config.filter ^= menu_entries[menu_selected].value;
        }
        else if (type == MenuMode)
        {
            menu_config.dl_mode_background ^= menu_entries[menu_selected].value;
        }
        else if (type == MenuMusic)
        {
            menu_config.music ^= menu_entries[menu_selected].value;
        }
        else if (type == MenuUpdate)
        {
            menu_config.version_check ^= menu_entries[menu_selected].value;
        }
        else if (type == MenuLayout)
        {
            menu_config.grid_mode ^= 1;
        }
        else if (type == MenuContent)
        {
            menu_config.filter ^= content_entries[menu_config.content].value;

            menu_config.content++;
            if (menu_config.content == MAX_CONTENT_TYPES)
                menu_config.content = 0;

            menu_config.filter ^= content_entries[menu_config.content].value;
        }
    }

    if (menu_width != pkgi_menu_width)
    {
        return 1;
    }

    int font_height = pkgi_text_height("M");
    menu_scroll_to_selection(font_height);

    int menu_x = VITA_WIDTH - (pkgi_menu_width + PKGI_MAIN_HMARGIN);
    pkgi_clip_set(menu_x, PKGI_MAIN_VMARGIN, pkgi_menu_width,
                  PKGI_MENU_HEIGHT);

    int y = PKGI_MENU_TOP_PADDING - menu_scroll_y;
    for (uint32_t i = 0; i < PKGI_COUNTOF(menu_entries); i++)
    {
        const MenuEntry* entry = menu_entries + i;

        MenuType type = entry->type;
        if (!menu_entry_visible(entry))
            continue;
        y += menu_entry_gap(entry, font_height);

        if (type == MenuText)
        {
            /* Section labels take the same line height as menu actions. */
        }

        int x = VITA_WIDTH - (pkgi_menu_width + PKGI_MAIN_HMARGIN) + PKGI_MENU_LEFT_PADDING;

        char text[64];
        if (type == MenuSearch || type == MenuSearchClear || type == MenuText || type == MenuRefresh)
        {
            pkgi_strncpy(text, sizeof(text), entry->text);
        }
        else if (type == MenuLoadConfig || type == MenuFolder)
        {
            pkgi_strncpy(text, sizeof(text), entry->text);
        }
        else if (type == MenuSort)
        {
            if (menu_config.sort == (DbSort)entry->value)
            {
                pkgi_snprintf(text, sizeof(text), "%s %s",
                    menu_config.order == SortAscending ? PKGI_UTF8_SORT_ASC : PKGI_UTF8_SORT_DESC,
                    entry->text);
            }
            else
            {
                x += pkgi_text_width(PKGI_UTF8_SORT_ASC " ");
                pkgi_strncpy(text, sizeof(text), entry->text);
            }
        }
        else if (type == MenuFilter)
        {
            pkgi_snprintf(text, sizeof(text), "%s %s",
                menu_config.filter & entry->value ? PKGI_UTF8_CHECK_ON : PKGI_UTF8_CHECK_OFF,
                entry->text);
        }
        else if (type == MenuMode)
        {
            pkgi_snprintf(text, sizeof(text), PKGI_UTF8_CLEAR " %s",
                menu_config.dl_mode_background == entry->value ? entry->text : _("Direct DL"));
        }
        else if (type == MenuMusic)
        {
            pkgi_snprintf(text, sizeof(text), "%s %s",
                menu_config.music == entry->value ? PKGI_UTF8_CHECK_ON : PKGI_UTF8_CHECK_OFF, entry->text);            
        }
        else if (type == MenuUpdate)
        {
            pkgi_snprintf(text, sizeof(text), "%s %s",
                menu_config.version_check == entry->value ? PKGI_UTF8_CHECK_ON : PKGI_UTF8_CHECK_OFF, entry->text);            
        }
        else if (type == MenuLayout)
        {
            pkgi_snprintf(text, sizeof(text), "%s %s: %s",
                menu_config.grid_mode ? PKGI_UTF8_CHECK_ON : PKGI_UTF8_CHECK_OFF,
                entry->text,
                menu_config.grid_mode ? _("Grid") : _("List"));
        }
        else if (type == MenuContent)
        {
            pkgi_snprintf(text, sizeof(text), PKGI_UTF8_CLEAR " %s", content_entries[menu_config.content].text);
        }
        
        if (y + font_height >= PKGI_MENU_TOP_PADDING &&
            y < PKGI_MENU_HEIGHT - 8)
        {
            pkgi_draw_text_z(x, y, PKGI_MENU_TEXT_Z,
                (menu_selected == i) ? PKGI_COLOR_TEXT_MENU_SELECTED : PKGI_COLOR_TEXT_MENU,
                text);
        }

        y += font_height;
    }

    pkgi_clip_remove();
    return 1;
}
