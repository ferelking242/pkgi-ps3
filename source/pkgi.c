#include "pkgi.h"
#include "pkgi_db.h"
#include "pkgi_menu.h"
#include "pkgi_config.h"
#include "pkgi_dialog.h"
#include "pkgi_download.h"
#include "pkgi_utils.h"
#include "pkgi_style.h"
#include "pkgi_sha256.h"

#include <stddef.h>
#include <mini18n.h>
#include <json/json.h>

#define content_filter(c)   (c ? 1 << (7 + c) : DbFilterAllContent)

typedef enum  {
    StateError,
    StateRefreshing,
    StateUpdateDone,
    StateMain,
    StateTerminate
} State;

static State state;

static uint32_t first_item;
static uint32_t selected_item;
static int selection_mode;
static DbItem* pending_install_item;

static int search_active;

static char refresh_url[MAX_CONTENT_TYPES][256];

static Config config;
static Config config_temp;

static int font_height;
static int avail_height;
static int bottom_y;

static char search_text[256];
static char error_state[256];
static int osk_target_folder; /* PKGi Remastered: OSK edits download folder */

#define PKGI_THUMBNAIL_SLOTS 16
#define PKGI_COVER_QUEUE_CAPACITY 32

typedef struct {
    char content[64];
    pkgi_texture texture;
} ThumbnailSlot;

static ThumbnailSlot thumbnails[PKGI_THUMBNAIL_SLOTS];
static pkgi_texture background_cover;
static char background_cover_content[64];
static int background_art_checked;
static char cover_request_queue[PKGI_COVER_QUEUE_CAPACITY][64];
static uint32_t cover_request_head;
static uint32_t cover_request_count;
static char cover_active_content[64];
static char cover_completed_content[64];
static char cover_selected_content[64];
static char background_request_content[64];
static char background_active_content[64];
static char background_attempted_content[64];
static int background_request_pending;
static volatile int cover_worker_active;

static void reposition(void);
static void cb_dialog_install(int res);

static const char* pkgi_get_ok_str(void)
{
    return pkgi_ok_button() == PKGI_BUTTON_X ? PKGI_UTF8_X : PKGI_UTF8_O;
}

static const char* pkgi_get_cancel_str(void)
{
    return pkgi_cancel_button() == PKGI_BUTTON_O ? PKGI_UTF8_O : PKGI_UTF8_X;
}

static void pkgi_refresh_thread(void)
{
    LOG("starting update");

    int refresh_requested = pkgi_menu_result() == MenuResultRefresh;

    /* Keep using the local cache on normal starts. On a fresh install (or
     * when the cache cannot be parsed), fetch the configured databases once
     * instead of leaving the user at an empty catalogue until manual refresh. */
    if (!refresh_requested &&
        pkgi_db_reload(error_state, sizeof(error_state)))
    {
        first_item = 0;
        selected_item = 0;
        state = StateUpdateDone;
        pkgi_thread_exit();
    }

    if (config.allow_refresh)
    {
        LOG(refresh_requested ? "manual database refresh" :
            "no usable local database; downloading configured databases");
        pkgi_db_update((char*) &refresh_url, sizeof(refresh_url[0]),
                       error_state, sizeof(error_state));
    }

    if (pkgi_db_reload(error_state, sizeof(error_state)))
    {
        first_item = 0;
        selected_item = 0;
        state = StateUpdateDone;
    }
    else
    {
        state = StateError;
    }
    
    pkgi_thread_exit();
}

static int install(const char* content, int show_dual_progress)
{
    LOG("installing...");
    if (show_dual_progress)
    {
        pkgi_dialog_update_install_progress(_("Sending package to the PS3 installer"), -1.f);
        pkgi_dialog_allow_close(0);
    }
    else
    {
        pkgi_dialog_start_progress(_("Sending to PS3 installer"),
                                   _("Preparing installer task..."), -1);
        pkgi_dialog_allow_close(0);
    }

    char titleid[10];
    pkgi_memcpy(titleid, content + 7, 9);
    titleid[9] = 0;

    int ok = pkgi_install(titleid);
    pkgi_dialog_allow_close(1);

    if (!ok)
    {
        pkgi_dialog_error(_("Installation failed"));
        return 0;
    }

    if (show_dual_progress)
        pkgi_dialog_update_install_progress(_("Task queued in PS3 installer"), 1.f);

    LOG("install succeeded");

    return 1;
}

static void pkgi_install_thread(void)
{
    DbItem* item = pending_install_item;

    if (item)
    {
        pkgi_sleep(250);
        pkgi_lock_process();
        int ok = install(item->content, 1);
        pkgi_unlock_process();

        if (ok)
        {
            pkgi_dialog_message(item->name,
                _("Sent to PS3 installer.\nCheck installation in the XMB.\nPKG remains in /dev_hdd0/vsh/game_pkg."));
        }
        item->presence = PresenceUnknown;
    }

    pending_install_item = NULL;
    /* Stay in PKGi after queuing; the user chooses when to return to XMB. */
    state = StateMain;
    pkgi_thread_exit();
}

static void cb_dialog_install(int res)
{
    PKGI_UNUSED(res);
    if (!pending_install_item)
        return;

    pkgi_dialog_start_dual_progress(_("Sending to PS3 installer"),
                                    _("Download complete"), 1.f);
    pkgi_dialog_update_progress(_("Download complete"), NULL, NULL, 1.f);
    pkgi_dialog_update_progress_size(pending_install_item->size,
                                    pending_install_item->size);
    pkgi_dialog_update_install_progress(_("Preparing installer task"), -1.f);
    pkgi_dialog_allow_close(0);
    pkgi_start_thread("install_thread", &pkgi_install_thread);
}

static void pkgi_download_thread(void)
{
    DbItem* item = pkgi_db_get(selected_item);

    LOG("download thread start");

    // short delay to allow download dialog to animate smoothly
    pkgi_sleep(300);

    pkgi_lock_process();
    if (pkgi_download(item, config.dl_mode_background))
    {
        if (!config.dl_mode_background)
        {
            char prompt[192];
            pending_install_item = item;
            pkgi_snprintf(prompt, sizeof(prompt),
                _("Download complete. %s: install  %s: exit (PKG stays in /dev_hdd0/vsh/game_pkg)."),
                pkgi_get_ok_str(), pkgi_get_cancel_str());
            pkgi_dialog_ok_cancel(item->name, prompt, &cb_dialog_install);
        }
        else
        {
            pkgi_dialog_message(item->name, _("Task successfully queued (reboot to start)"));
        }
        LOG("download completed!");
    }
    pkgi_unlock_process();

    if (pkgi_dialog_is_cancelled())
    {
        pkgi_dialog_close();
    }

    item->presence = PresenceUnknown;
    state = StateMain;

    pkgi_thread_exit();
}

/* ---- Batch download (PKGi Remastered) ----
 * Downloads every marked item in sequence. In direct mode each
 * finished download is sent to the PS3 installer right away;
 * in background mode the PKG is queued as an install task.
 * One failed item does not stop the queue; a summary is shown. */
static void pkgi_batch_thread(void)
{
    uint32_t done = 0, failed = 0, total = pkgi_db_selected_count();
    uint32_t current = 0;

    LOG("batch download thread start (%u items)", total);

    pkgi_sleep(300);
    pkgi_lock_process();

    for (current = 0; current < pkgi_db_total(); current++)
    {
        DbItem* item = pkgi_db_get(current);
        if (!item || !item->marked)
            continue;

        if (state == StateTerminate)
            break;

        char title[192];
        pkgi_snprintf(title, sizeof(title), _("Batch %u/%u"), done + failed + 1, total);

        pkgi_dialog_start_dual_progress(title, item->name, 0);
        pkgi_dialog_allow_close(1);

        if (pkgi_check_free_space(item->size))
        {
            item->presence = PresenceMissing;
            if (pkgi_download(item, config.dl_mode_background))
            {
                if (!config.dl_mode_background)
                {
                    if (install(item->content, 1))
                    {
                        done++;
                        pkgi_sleep(700);
                    }
                    else
                        failed++;
                }
                else
                {
                    pkgi_dialog_update_install_progress(_("Background download queued"), 1.f);
                    done++;
                }
            }
            else if (!pkgi_dialog_is_cancelled())
            {
                failed++;
                pkgi_dialog_error(item->name);
                pkgi_sleep(500);
            }
            else
            {
                break; /* user cancelled the queue */
            }
        }
        else
        {
            failed++;
            pkgi_sleep(500);
        }
    }

    pkgi_unlock_process();
    pkgi_db_clear_selection();

    char summary[192];
    pkgi_snprintf(summary, sizeof(summary), _("%u queued for install, %u failed"), done, failed);
    pkgi_dialog_message(_("Batch download"), summary);

    state = StateMain;
    pkgi_thread_exit();
}

static void pkgi_start_batch(void)
{
    selection_mode = 0;
    state = StateMain;
    pkgi_start_thread("batch_thread", &pkgi_batch_thread);
}

static uint32_t friendly_size(uint64_t size)
{
    if (size > 10ULL * 1000 * 1024 * 1024)
    {
        return (uint32_t)(size / (1024 * 1024 * 1024));
    }
    else if (size > 10 * 1000 * 1024)
    {
        return (uint32_t)(size / (1024 * 1024));
    }
    else if (size > 10 * 1000)
    {
        return (uint32_t)(size / 1024);
    }
    else
    {
        return (uint32_t)size;
    }
}

static const char* friendly_size_str(uint64_t size)
{
    if (size > 10ULL * 1000 * 1024 * 1024)
    {
        return _(PKGI_UTF8_GB);
    }
    else if (size > 10 * 1000 * 1024)
    {
        return _(PKGI_UTF8_MB);
    }
    else if (size > 10 * 1000)
    {
        return _(PKGI_UTF8_KB);
    }
    else
    {
        return _(PKGI_UTF8_B);
    }
}

int pkgi_check_free_space(uint64_t size)
{
    uint64_t free = pkgi_get_free_space();
    if (size > free + 1024 * 1024)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), _("pkg requires %u %s free space, but only %u %s available"),
            friendly_size(size), friendly_size_str(size),
            friendly_size(free), friendly_size_str(free)
        );

        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

void pkgi_friendly_size(char* text, uint32_t textlen, int64_t size)
{
    if (size <= 0)
    {
        text[0] = 0;
    }
    else if (size < 1000LL)
    {
        pkgi_snprintf(text, textlen, "%u %s", (uint32_t)size, _(PKGI_UTF8_B));
    }
    else if (size < 1000LL * 1000)
    {
        pkgi_snprintf(text, textlen, "%.2f %s", size / 1024.f, _(PKGI_UTF8_KB));
    }
    else if (size < 1000LL * 1000 * 1000)
    {
        pkgi_snprintf(text, textlen, "%.2f %s", size / 1024.f / 1024.f, _(PKGI_UTF8_MB));
    }
    else
    {
        pkgi_snprintf(text, textlen, "%.2f %s", size / 1024.f / 1024.f / 1024.f, _(PKGI_UTF8_GB));
    }
}

static const char* content_type_str(ContentType content)
{
    switch (content)
    {
        case 0: return _("All");
        case ContentGame: return _("Games");
        case ContentDLC: return _("DLCs");
        case ContentTheme: return _("Themes");
        case ContentAvatar: return _("Avatars");
        case ContentDemo: return _("Demos");
        case ContentUpdate: return _("Updates");
        case ContentEmulator: return _("Emulators");
        case ContentApp: return _("Apps");
        case ContentTool: return _("Tools");
        default: return _("Unknown");
    }
}

static int pkgi_cover_request_queued_locked(const char* content)
{
    for (uint32_t i = 0; i < cover_request_count; i++)
    {
        uint32_t slot = (cover_request_head + i) % PKGI_COVER_QUEUE_CAPACITY;
        if (pkgi_stricmp(cover_request_queue[slot], content) == 0)
            return 1;
    }
    return 0;
}

static void pkgi_cover_thread(void)
{
    for (;;)
    {
        char content[64] = {0};
        int background_request = 0;

        pkgi_dialog_lock();
        if (cover_request_count)
        {
            pkgi_strncpy(content, sizeof(content),
                cover_request_queue[cover_request_head]);
            cover_request_head =
                (cover_request_head + 1) % PKGI_COVER_QUEUE_CAPACITY;
            cover_request_count--;
            pkgi_strncpy(cover_active_content, sizeof(cover_active_content),
                         content);
        }
        else if (background_request_pending)
        {
            pkgi_strncpy(content, sizeof(content), background_request_content);
            background_request_pending = 0;
            background_request = 1;
            pkgi_strncpy(background_active_content,
                         sizeof(background_active_content), content);
        }
        else
        {
            cover_worker_active = 0;
            cover_active_content[0] = 0;
            background_active_content[0] = 0;
            pkgi_dialog_unlock();
            break;
        }
        pkgi_dialog_unlock();

        /* Serialize image requests with package creation and install work,
         * which also touches the shared HTTP/file services. */
        pkgi_lock_process();
        if (background_request)
            pkgi_download_background(content);
        else
            pkgi_download_icon(content);
        pkgi_unlock_process();

        pkgi_dialog_lock();
        if (background_request)
        {
            pkgi_strncpy(background_attempted_content,
                         sizeof(background_attempted_content), content);
            if (pkgi_stricmp(background_active_content, content) == 0)
                background_active_content[0] = 0;
        }
        else
        {
            pkgi_strncpy(cover_completed_content,
                         sizeof(cover_completed_content), content);
            if (pkgi_stricmp(cover_active_content, content) == 0)
                cover_active_content[0] = 0;
        }
        pkgi_dialog_unlock();
    }

    pkgi_thread_exit();
}

static void pkgi_request_cover(const DbItem* item)
{
    if (!item || !item->content)
        return;

    pkgi_dialog_lock();
    if (pkgi_stricmp(cover_active_content, item->content) == 0 ||
        pkgi_stricmp(cover_completed_content, item->content) == 0 ||
        pkgi_cover_request_queued_locked(item->content))
    {
        pkgi_dialog_unlock();
        return;
    }
    pkgi_dialog_unlock();

    char icon_path[128];
    pkgi_snprintf(icon_path, sizeof(icon_path), PKGI_TMP_FOLDER "/%.9s.PNG",
                  item->content + 7);
    if (pkgi_get_size(icon_path) > 0)
    {
        pkgi_dialog_lock();
        pkgi_strncpy(cover_completed_content,
                     sizeof(cover_completed_content), item->content);
        pkgi_dialog_unlock();
        return;
    }

    int start_worker = 0;
    pkgi_dialog_lock();
    if (pkgi_stricmp(cover_active_content, item->content) != 0 &&
        pkgi_stricmp(cover_completed_content, item->content) != 0 &&
        !pkgi_cover_request_queued_locked(item->content))
    {
        if (cover_request_count == PKGI_COVER_QUEUE_CAPACITY)
        {
            /* Keep recent visible items instead of allowing a long scroll
             * session to fill the queue with thumbnails that are off-screen. */
            cover_request_head =
                (cover_request_head + 1) % PKGI_COVER_QUEUE_CAPACITY;
            cover_request_count--;
        }

        uint32_t slot =
            (cover_request_head + cover_request_count) %
            PKGI_COVER_QUEUE_CAPACITY;
        pkgi_strncpy(cover_request_queue[slot],
                     sizeof(cover_request_queue[slot]), item->content);
        cover_request_count++;
    }
    if (!cover_worker_active &&
        (cover_request_count || background_request_pending))
    {
        cover_worker_active = 1;
        start_worker = 1;
    }
    pkgi_dialog_unlock();

    if (start_worker)
        pkgi_start_thread("cover_thread", &pkgi_cover_thread);
}

static void pkgi_request_background(const DbItem* item)
{
    if (!item || !item->content || item->type != ContentGame)
        return;

    pkgi_dialog_lock();
    if (pkgi_stricmp(background_active_content, item->content) == 0 ||
        pkgi_stricmp(background_attempted_content, item->content) == 0 ||
        (background_request_pending &&
         pkgi_stricmp(background_request_content, item->content) == 0))
    {
        pkgi_dialog_unlock();
        return;
    }
    pkgi_dialog_unlock();

    char background_path[128];
    pkgi_snprintf(background_path, sizeof(background_path),
                  PKGI_TMP_FOLDER "/%.9s_BG.PNG", item->content + 7);
    if (pkgi_get_size(background_path) > 0)
    {
        pkgi_dialog_lock();
        pkgi_strncpy(background_attempted_content,
                     sizeof(background_attempted_content), item->content);
        pkgi_dialog_unlock();
        return;
    }

    int start_worker = 0;
    pkgi_dialog_lock();
    if (pkgi_stricmp(background_active_content, item->content) != 0 &&
        pkgi_stricmp(background_attempted_content, item->content) != 0 &&
        !(background_request_pending &&
          pkgi_stricmp(background_request_content, item->content) == 0))
    {
        /* Only keep the latest selected title's background request. */
        pkgi_strncpy(background_request_content,
                     sizeof(background_request_content), item->content);
        background_request_pending = 1;
    }
    if (!cover_worker_active &&
        (cover_request_count || background_request_pending))
    {
        cover_worker_active = 1;
        start_worker = 1;
    }
    pkgi_dialog_unlock();

    if (start_worker)
        pkgi_start_thread("cover_thread", &pkgi_cover_thread);
}

static pkgi_texture pkgi_get_thumbnail(const DbItem* item, uint32_t slot)
{
    if (!item || slot >= PKGI_THUMBNAIL_SLOTS)
        return NULL;

    ThumbnailSlot* thumbnail = thumbnails + slot;
    if (pkgi_stricmp(thumbnail->content, item->content) != 0)
    {
        if (thumbnail->texture)
            pkgi_free_texture(thumbnail->texture);
        thumbnail->texture = NULL;
        pkgi_strncpy(thumbnail->content, sizeof(thumbnail->content), item->content);
        pkgi_request_cover(item);
    }

    if (!thumbnail->texture)
    {
        char icon_path[128];
        pkgi_snprintf(icon_path, sizeof(icon_path), PKGI_TMP_FOLDER "/%.9s.PNG",
                      item->content + 7);
        if (pkgi_get_size(icon_path) > 0)
            thumbnail->texture = pkgi_load_png_file(icon_path);
    }

    return thumbnail->texture;
}

static pkgi_texture pkgi_load_local_game_art(const char* content, const char* filename)
{
    char path[256];
    pkgi_snprintf(path, sizeof(path), "/dev_hdd0/game/%.9s/%s",
                  content + 7, filename);
    if (pkgi_get_size(path) <= 0)
        return NULL;

    return pkgi_load_png_file(path);
}

static pkgi_texture pkgi_get_background_cover(const DbItem* item)
{
    if (!item)
        return NULL;

    if (pkgi_stricmp(background_cover_content, item->content) != 0)
    {
        if (background_cover)
            pkgi_free_texture(background_cover);
        background_cover = NULL;
        pkgi_strncpy(background_cover_content, sizeof(background_cover_content),
                     item->content);
        background_art_checked = 0;
    }

    if (!background_cover && !background_art_checked)
    {
        /* Installed PS3 titles may include their own full-screen artwork. */
        background_cover = pkgi_load_local_game_art(item->content, "PIC1.PNG");
        if (!background_cover)
            background_cover = pkgi_load_local_game_art(item->content, "PIC0.PNG");
        background_art_checked = 1;
    }

    if (!background_cover)
    {
        char background_path[128];
        pkgi_snprintf(background_path, sizeof(background_path),
                      PKGI_TMP_FOLDER "/%.9s_BG.PNG", item->content + 7);
        if (pkgi_get_size(background_path) > 0)
            background_cover = pkgi_load_png_file(background_path);
        else
            pkgi_request_background(item);
    }

    return background_cover;
}

static void pkgi_free_covers(void)
{
    for (uint32_t i = 0; i < PKGI_THUMBNAIL_SLOTS; i++)
    {
        if (thumbnails[i].texture)
            pkgi_free_texture(thumbnails[i].texture);
        thumbnails[i].texture = NULL;
    }
    if (background_cover)
        pkgi_free_texture(background_cover);
    background_cover = NULL;
    background_cover_content[0] = 0;
    background_art_checked = 0;
}

static void cb_dialog_exit(int res)
{
    state = StateTerminate;
}

static void cb_dialog_download(int res)
{
    DbItem* item = pkgi_db_get(selected_item);

    if (!item)
        return;
    item->presence = PresenceMissing;
    pkgi_dialog_start_progress(_("Downloading..."), _("Preparing..."), 0);
    pkgi_start_thread("download_thread", &pkgi_download_thread);
}

static void pkgi_refresh_presence(DbItem* item)
{
    if (item && item->presence == PresenceUnknown)
    {
        item->presence = pkgi_is_incomplete(item->content) ? PresenceIncomplete :
            pkgi_is_installed(item->content) ? PresenceInstalled : PresenceMissing;
    }
}

static void pkgi_start_details_install(void)
{
    DbItem* item = pkgi_db_get(selected_item);
    if (!item)
        return;

    pkgi_refresh_presence(item);
    if (!pkgi_check_free_space(item->size))
        return;

    if (item->presence == PresenceInstalled)
    {
        pkgi_dialog_ok_cancel(item->name, _("Item already installed, download again?"),
                              &cb_dialog_download);
        return;
    }

    item->presence = PresenceMissing;
    pkgi_dialog_start_progress(_("Downloading..."), _("Preparing..."), 0);
    pkgi_start_thread("download_thread", &pkgi_download_thread);
}

static void pkgi_do_main(pkgi_input* input)
{
    const uint32_t grid_columns = 3;
    const int grid_cell_height = 124;
    const int list_row_height = 34;
    const int grid_mode = config.grid_mode != 0;
    const int item_height = grid_mode ? grid_cell_height : list_row_height;
    uint32_t rows_per_page = (uint32_t)(avail_height / item_height);
    if (rows_per_page == 0)
        rows_per_page = 1;
    uint32_t page_size = grid_mode ? rows_per_page * grid_columns : rows_per_page;
    int col_cover = PKGI_MAIN_HMARGIN;
    int col_region = col_cover + 46 + PKGI_MAIN_COLUMN_PADDING;
    int col_installed = col_region + pkgi_text_width("USA") + PKGI_MAIN_COLUMN_PADDING;
    int installed_label_width = pkgi_text_width(_("Installed"));
    int incomplete_label_width = pkgi_text_width(_("Incomplete"));
    int missing_label_width = pkgi_text_width(_("Not installed"));
    if (incomplete_label_width > installed_label_width)
        installed_label_width = incomplete_label_width;
    if (missing_label_width > installed_label_width)
        installed_label_width = missing_label_width;
    int status_width = pkgi_text_width(PKGI_UTF8_CHECK_ON " ") +
                       installed_label_width;
    int col_name = col_installed + status_width + PKGI_MAIN_COLUMN_PADDING;

    uint32_t db_count = pkgi_db_count();
    
    if (input)
    {
        if (pkgi_dialog_is_background())
        {
            /* Keep list navigation available, but don't replace the active
             * download dialog with another action or confirmation. L2 is
             * reserved to restore that dialog. */
            input->pressed &= ~(pkgi_cancel_button() | pkgi_ok_button() |
                                PKGI_BUTTON_SELECT | PKGI_BUTTON_START |
                                PKGI_BUTTON_T | PKGI_BUTTON_S);
            input->active &= ~(pkgi_cancel_button() | PKGI_BUTTON_SELECT |
                               PKGI_BUTTON_START | PKGI_BUTTON_L2 |
                               PKGI_BUTTON_T | PKGI_BUTTON_S);
        }

        if (input->active & pkgi_cancel_button())
        {
            input->pressed &= ~pkgi_cancel_button();
            pkgi_dialog_ok_cancel("\xE2\x98\x85  PKGi PS3 v" PKGI_VERSION "  \xE2\x98\x85", _("Exit to XMB?"), &cb_dialog_exit);
        }

        if (input->pressed & PKGI_BUTTON_SELECT)
        {
            input->pressed &= ~PKGI_BUTTON_SELECT;
            selection_mode = !selection_mode;
        }

        if (input->active & PKGI_BUTTON_START)
        {
            input->pressed &= ~PKGI_BUTTON_START;

            uint32_t marked = pkgi_db_selected_count();
            if (marked > 0)
            {
                pkgi_start_batch();
            }
            else
            {
                pkgi_dialog_message(_("Selection"),
                    _("Nothing selected. Mark items with SELECT first."));
            }
        }

        if (input->active & PKGI_BUTTON_L2)
        {
            config.filter ^= content_filter(config.content);

            if (config.content == 0)
                config.content = MAX_CONTENT_TYPES;

            config.content--;
            config.filter ^= content_filter(config.content);

            pkgi_db_configure(search_active ? search_text : NULL, &config);
            reposition();
            db_count = pkgi_db_count();
        }

        if (input->active & PKGI_BUTTON_R2)
        {
            config.filter ^= content_filter(config.content);

            config.content++;
            if (config.content == MAX_CONTENT_TYPES)
                config.content = 0;

            config.filter ^= content_filter(config.content);

            pkgi_db_configure(search_active ? search_text : NULL, &config);
            reposition();
            db_count = pkgi_db_count();
        }

        if (db_count)
        {
            if (grid_mode && (input->active & PKGI_BUTTON_LEFT))
            {
                selected_item = selected_item ? selected_item - 1 : db_count - 1;
            }
            else if (grid_mode && (input->active & PKGI_BUTTON_RIGHT))
            {
                selected_item = selected_item + 1 < db_count ? selected_item + 1 : 0;
            }

            if (input->active & PKGI_BUTTON_UP)
            {
                if (grid_mode)
                {
                    uint32_t column = selected_item % grid_columns;
                    uint32_t row = selected_item / grid_columns;
                    if (row)
                    {
                        selected_item -= grid_columns;
                    }
                    else
                    {
                        uint32_t last_row = ((db_count - 1) / grid_columns) * grid_columns;
                        selected_item = last_row + column;
                        if (selected_item >= db_count)
                            selected_item = db_count - 1;
                    }
                }
                else if (selected_item)
                {
                    selected_item--;
                }
                else
                {
                    selected_item = db_count - 1;
                }
            }

            if (input->active & PKGI_BUTTON_DOWN)
            {
                if (grid_mode)
                {
                    uint32_t next = selected_item + grid_columns;
                    selected_item = next < db_count
                        ? next : selected_item % grid_columns;
                }
                else
                {
                    selected_item = selected_item + 1 < db_count
                        ? selected_item + 1 : 0;
                }
            }

            if (input->active & PKGI_BUTTON_LT)
            {
                if (grid_mode)
                {
                    first_item = first_item > page_size ? first_item - page_size : 0;
                    selected_item = selected_item > page_size
                        ? selected_item - page_size : 0;
                }
                else
                {
                    first_item = first_item > page_size ? first_item - page_size : 0;
                    selected_item = selected_item > page_size
                        ? selected_item - page_size : 0;
                }
            }

            if (input->active & PKGI_BUTTON_RT)
            {
                selected_item = selected_item + page_size < db_count
                    ? selected_item + page_size : db_count - 1;
                first_item = (selected_item / page_size) * page_size;
            }

            if (grid_mode)
            {
                uint32_t selected_row = selected_item / grid_columns;
                uint32_t first_row = first_item / grid_columns;
                if (selected_row < first_row)
                    first_item = selected_row * grid_columns;
                else if (selected_row >= first_row + rows_per_page)
                    first_item = (selected_row - rows_per_page + 1) * grid_columns;
            }
            else if (selected_item < first_item)
            {
                first_item = selected_item;
            }
            else if (selected_item >= first_item + rows_per_page)
            {
                first_item = selected_item - rows_per_page + 1;
            }
        }
    }
    
    int list_top = font_height * 2 + PKGI_MAIN_HLINE_EXTRA +
                   PKGI_MAIN_VMARGIN + 4;
    if (grid_mode)
    {
        int grid_width = VITA_WIDTH - 2 * PKGI_MAIN_HMARGIN -
                         PKGI_MAIN_SCROLL_WIDTH - PKGI_MAIN_SCROLL_PADDING;
        int cell_width = grid_width / grid_columns;
        uint32_t end_item = min32(db_count, first_item + page_size);

        for (uint32_t i = first_item; i < end_item; i++)
        {
            DbItem* item = pkgi_db_get(i);
            uint32_t slot = i - first_item;
            int x = PKGI_MAIN_HMARGIN + (slot % grid_columns) * cell_width;
            int y = list_top + (slot / grid_columns) * grid_cell_height;
            int card_width = cell_width - 8;
            uint32_t color = PKGI_COLOR_TEXT;

            pkgi_refresh_presence(item);

            pkgi_draw_fill_rect_z(x, y, PKGI_FONT_Z - 1, card_width,
                                  grid_cell_height - 8,
                                  i == selected_item ? PKGI_COLOR_SELECTED_BACKGROUND :
                                                       PKGI_COLOR_DIALOG_INNER);
            if (i == selected_item)
                pkgi_draw_rect_z(x, y, PKGI_FONT_Z, card_width,
                                 grid_cell_height - 8, PKGI_COLOR_ACCENT);

            if (pkgi_db_is_selected(i))
                pkgi_draw_text_z(x + 5, y + 5, PKGI_FONT_Z,
                                 PKGI_COLOR_ACCENT, PKGI_UTF8_CHECK_ON);

            pkgi_texture cover = pkgi_get_thumbnail(item, slot);
            if (cover)
                pkgi_draw_texture_z(cover, x + 10, y + 29, PKGI_FONT_Z, 0.17f);

            int text_x = x + 72;
            int text_width = card_width - 82;
            pkgi_clip_set(text_x, y + 8, text_width, 58);
            pkgi_draw_text_ttf(text_x, y + 8, PKGI_FONT_Z, color, item->name);
            pkgi_clip_remove();

            const char* region = "???";
            switch (pkgi_get_region(item->content))
            {
            case RegionASA: region = "ASA"; break;
            case RegionEUR: region = "EUR"; break;
            case RegionJPN: region = "JPN"; break;
            case RegionUSA: region = "USA"; break;
            default: break;
            }

            char size_str[32];
            pkgi_friendly_size(size_str, sizeof(size_str), item->size);
            const char* status_icon = item->presence == PresenceInstalled
                ? PKGI_UTF8_CHECK_ON
                : item->presence == PresenceIncomplete
                    ? PKGI_UTF8_PARTIAL : PKGI_UTF8_CHECK_OFF;
            const char* status_label = item->presence == PresenceInstalled
                ? _("Installed")
                : item->presence == PresenceIncomplete
                    ? _("Incomplete") : _("Not installed");
            uint32_t status_color = item->presence == PresenceInstalled
                ? PKGI_COLOR_ACCENT
                : item->presence == PresenceIncomplete
                    ? PKGI_COLOR_BATTERY_LOW : PKGI_COLOR_TEXT_DIM;
            char status[48];
            pkgi_snprintf(status, sizeof(status), "%s %s",
                          status_icon, status_label);
            int status_y = y + grid_cell_height - 51;
            pkgi_clip_set(text_x, status_y, text_width, 20);
            pkgi_draw_text_z(text_x, status_y, PKGI_FONT_Z,
                             status_color, status);
            pkgi_clip_remove();

            int size_y = status_y + 19;
            char meta[64];
            pkgi_snprintf(meta, sizeof(meta), "%s  %s", region, size_str);
            pkgi_clip_set(text_x, size_y, text_width, 20);
            pkgi_draw_text_z(text_x, size_y, PKGI_FONT_Z,
                             PKGI_COLOR_TEXT_DIM, meta);
            pkgi_clip_remove();
        }
    }
    else
    {
        uint32_t end_item = min32(db_count, first_item + rows_per_page);
        for (uint32_t i = first_item; i < end_item; i++)
        {
            DbItem* item = pkgi_db_get(i);
            int y = list_top + (i - first_item) * list_row_height;
            int text_y = y + (list_row_height - font_height) / 2;
            uint32_t color = PKGI_COLOR_TEXT;

            pkgi_refresh_presence(item);

            char size_str[64];
            pkgi_friendly_size(size_str, sizeof(size_str), item->size);
            int sizew = pkgi_text_width(size_str);
            pkgi_clip_set(0, y, VITA_WIDTH, list_row_height);

            if (i == selected_item)
                pkgi_draw_fill_rect_z(0, y, PKGI_FONT_Z - 1, VITA_WIDTH,
                                      list_row_height - 1, PKGI_COLOR_SELECTED_BACKGROUND);
            if (pkgi_db_is_selected(i))
                pkgi_draw_text(col_cover, text_y, PKGI_COLOR_ACCENT, PKGI_UTF8_CHECK_ON);

            pkgi_texture cover = pkgi_get_thumbnail(item, i - first_item);
            if (cover)
                pkgi_draw_texture_z(cover, col_cover + 15, y + 6, PKGI_FONT_Z, 0.1f);

            const char* region = "???";
            switch (pkgi_get_region(item->content))
            {
            case RegionASA: region = "ASA"; break;
            case RegionEUR: region = "EUR"; break;
            case RegionJPN: region = "JPN"; break;
            case RegionUSA: region = "USA"; break;
            default: break;
            }

            pkgi_draw_text(col_region, text_y, color, region);
            const char* status_icon = item->presence == PresenceInstalled
                ? PKGI_UTF8_CHECK_ON
                : item->presence == PresenceIncomplete
                    ? PKGI_UTF8_PARTIAL : PKGI_UTF8_CHECK_OFF;
            const char* status_label = item->presence == PresenceInstalled
                ? _("Installed")
                : item->presence == PresenceIncomplete
                    ? _("Incomplete") : _("Not installed");
            uint32_t status_color = item->presence == PresenceInstalled
                ? PKGI_COLOR_ACCENT
                : item->presence == PresenceIncomplete
                    ? PKGI_COLOR_BATTERY_LOW : PKGI_COLOR_TEXT_DIM;
            char status[48];
            pkgi_snprintf(status, sizeof(status), "%s %s",
                          status_icon, status_label);
            pkgi_draw_text(col_installed, text_y, status_color, status);

            pkgi_draw_text(VITA_WIDTH - (PKGI_MAIN_SCROLL_WIDTH +
                PKGI_MAIN_SCROLL_PADDING + PKGI_MAIN_HMARGIN + sizew),
                text_y, color, size_str);
            pkgi_clip_remove();

            pkgi_clip_set(col_name, y, VITA_WIDTH - PKGI_MAIN_SCROLL_WIDTH -
                PKGI_MAIN_SCROLL_PADDING - PKGI_MAIN_COLUMN_PADDING - sizew -
                col_name, list_row_height);
            pkgi_draw_text_ttf(col_name, text_y, PKGI_FONT_Z, color, item->name);
            pkgi_clip_remove();
        }
    }

    if (db_count == 0)
    {
        const char* text = _("No items!");

        int w = pkgi_text_width(text);
        pkgi_draw_text((VITA_WIDTH - w) / 2, VITA_HEIGHT / 2, PKGI_COLOR_TEXT, text);
    }

    // scroll-bar
    if (db_count != 0)
    {
        if (page_size < db_count)
        {
            uint32_t min_height = PKGI_MAIN_SCROLL_MIN_HEIGHT;
            uint32_t height = page_size * avail_height / db_count;
            uint32_t start = first_item * (avail_height - (height < min_height ? min_height : 0)) / db_count;
            height = max32(height, min_height);
            pkgi_draw_fill_rect_z(VITA_WIDTH - (PKGI_MAIN_HMARGIN + PKGI_MAIN_SCROLL_WIDTH), font_height + PKGI_MAIN_HLINE_EXTRA + PKGI_MAIN_VMARGIN + start + 2, PKGI_FONT_Z, PKGI_MAIN_SCROLL_WIDTH, height, PKGI_COLOR_SCROLL_BAR);
        }
    }

    if (input && selection_mode && (input->pressed & pkgi_ok_button()) && db_count)
    {
        input->pressed &= ~pkgi_ok_button();
        pkgi_db_toggle_select(selected_item);
    }
    else if (input && (input->pressed & pkgi_ok_button()) && db_count)
    {
        input->pressed &= ~pkgi_ok_button();

        DbItem* item = pkgi_db_get(selected_item);
        pkgi_refresh_presence(item);

        if (grid_mode)
        {
            pkgi_request_cover(item);
            pkgi_dialog_details(item, content_type_str(item->type));
        }
        else if (!pkgi_check_free_space(item->size))
        {
            LOG("[%.9s] %s - no free space", item->content + 7, item->name);
            pkgi_dialog_error(_("Not enough free space on HDD"));
        }
        else if (item->presence == PresenceInstalled)
        {
            LOG("[%.9s] %s - already installed", item->content + 7, item->name);
            pkgi_dialog_ok_cancel(item->name, _("Item already installed, download again?"), &cb_dialog_download);
        }
        else if (item->presence == PresenceIncomplete || (item->presence == PresenceMissing))
        {
            LOG("[%.9s] %s - starting to install", item->content + 7, item->name);
            pkgi_dialog_start_progress(_("Downloading..."), _("Preparing..."), 0);
            pkgi_start_thread("download_thread", &pkgi_download_thread);
        }
    }
    else if (input && (input->pressed & PKGI_BUTTON_T))
    {
        input->pressed &= ~PKGI_BUTTON_T;

        config_temp = config;

        pkgi_menu_start(search_active, &config);
    }
    else if (input && (input->active & PKGI_BUTTON_S) && db_count)
    {
        input->pressed &= ~PKGI_BUTTON_S;

        DbItem* item = pkgi_db_get(selected_item);

        pkgi_request_cover(item);
        pkgi_dialog_details(item, content_type_str(item->type));
    }
}

static void pkgi_do_refresh(void)
{
    char text[256];

    uint32_t updated;
    uint32_t total;
    pkgi_db_get_update_status(&updated, &total);

    if (total == 0)
    {
        pkgi_snprintf(text, sizeof(text), "%s... %.2f %s", _("Refreshing"), (uint32_t)updated / 1024.f, _("KB"));
    }
    else
    {
        pkgi_snprintf(text, sizeof(text), "%s... %u%%", _("Refreshing"), updated * 100U / total);
    }

    int w = pkgi_text_width(text);
    pkgi_draw_text((VITA_WIDTH - w) / 2, VITA_HEIGHT / 2, PKGI_COLOR_TEXT, text);
}

static void pkgi_do_head(void)
{
    char title[256];
    pkgi_snprintf(title, sizeof(title), "PKGi PS3 v%s - %s", PKGI_VERSION, content_type_str(config.content));
    pkgi_draw_text(PKGI_MAIN_HMARGIN, PKGI_MAIN_VMARGIN, PKGI_COLOR_TEXT_HEAD, title);

    pkgi_draw_fill_rect(0, font_height + PKGI_MAIN_VMARGIN, VITA_WIDTH, PKGI_MAIN_HLINE_HEIGHT, PKGI_COLOR_HLINE);

    char battery[256];
    pkgi_snprintf(battery, sizeof(battery), "CPU: %u""\xf8""C RSX: %u""\xf8""C", pkgi_get_temperature(0), pkgi_get_temperature(1));

    uint32_t color = pkgi_temperature_is_high() ? PKGI_COLOR_BATTERY_LOW : PKGI_COLOR_BATTERY_CHARGING;
    int rightw = pkgi_text_width(battery);
    pkgi_draw_text(VITA_WIDTH - PKGI_MAIN_HLINE_EXTRA - (rightw + PKGI_MAIN_HMARGIN), PKGI_MAIN_VMARGIN, color, battery);

    static char network_text[96];
    static uint32_t network_text_color = PKGI_COLOR_TEXT_DIM;
    static uint32_t next_network_check;
    uint32_t now = pkgi_time_msec();
    if (next_network_check == 0 || now >= next_network_check)
    {
        char ip_address[32];
        if (pkgi_get_ip_address(ip_address, sizeof(ip_address)))
        {
            pkgi_snprintf(network_text, sizeof(network_text),
                          "WebMAN: http://%s/setup.ps3", ip_address);
            network_text_color = PKGI_COLOR_ACCENT;
        }
        else
        {
            pkgi_snprintf(network_text, sizeof(network_text), "%s",
                          _("Network IP unavailable"));
            network_text_color = PKGI_COLOR_TEXT_DIM;
        }
        next_network_check = now + 1000;
    }
    int network_y = font_height + PKGI_MAIN_VMARGIN +
                    PKGI_MAIN_HLINE_EXTRA + 4;
    pkgi_clip_set(PKGI_MAIN_HMARGIN, network_y,
                  VITA_WIDTH - 2 * PKGI_MAIN_HMARGIN, font_height + 2);
    pkgi_draw_text(PKGI_MAIN_HMARGIN, network_y, network_text_color,
                   network_text);
    pkgi_clip_remove();

    float download_progress = 0.f;
    if (pkgi_dialog_background_progress(&download_progress))
    {
        char percent[16];
        pkgi_snprintf(percent, sizeof(percent), "%.0f%%", download_progress * 100.f);
        int x = VITA_WIDTH / 2 + 30;
        int y = PKGI_MAIN_VMARGIN + font_height / 2;
        pkgi_draw_fill_rect(x, y, 76, 5, PKGI_COLOR_PROGRESS_BACKGROUND);
        pkgi_draw_fill_rect(x, y, (int)(76 * download_progress), 5, PKGI_COLOR_PROGRESS_BAR);
        pkgi_draw_text(x + 84, PKGI_MAIN_VMARGIN, PKGI_COLOR_ACCENT, percent);
    }

    if (search_active)
    {
        char text[256];
        int left = pkgi_text_width(search_text) + PKGI_MAIN_TEXT_PADDING;
        int right = rightw + PKGI_MAIN_TEXT_PADDING;

        pkgi_snprintf(text, sizeof(text), ">> %s <<", search_text);

        pkgi_clip_set(left, PKGI_MAIN_VMARGIN, VITA_WIDTH - right - left, font_height + PKGI_MAIN_HLINE_EXTRA);
        pkgi_draw_text((VITA_WIDTH - pkgi_text_width(text)) / 2, PKGI_MAIN_VMARGIN, PKGI_COLOR_TEXT_TAIL, text);
        pkgi_clip_remove();
    }
}

static void pkgi_do_tail(void)
{
    pkgi_draw_fill_rect_z(0, bottom_y - font_height/2, PKGI_FONT_Z, VITA_WIDTH, PKGI_MAIN_HLINE_HEIGHT, PKGI_COLOR_HLINE);

    uint32_t count = pkgi_db_count();
    uint32_t total = pkgi_db_total();

    char text[256];
    if (count == total)
    {
        pkgi_snprintf(text, sizeof(text), "%s: %u", _("Count"), count);
    }
    else
    {
        pkgi_snprintf(text, sizeof(text), "%s: %u (%u)", _("Count"), count, total);
    }
    pkgi_draw_text(PKGI_MAIN_HMARGIN, bottom_y, PKGI_COLOR_TEXT_TAIL, text);

    char size[64];
    pkgi_friendly_size(size, sizeof(size), pkgi_get_free_space());

    char free_str[64];
    pkgi_snprintf(free_str, sizeof(free_str), "%s: %s", _("Free"), size);

    int rightw = pkgi_text_width(free_str);
    pkgi_draw_text(VITA_WIDTH - (PKGI_MAIN_HLINE_EXTRA + PKGI_MAIN_HMARGIN + rightw), bottom_y, PKGI_COLOR_TEXT_TAIL, free_str);

    int left = pkgi_text_width(text) + PKGI_MAIN_TEXT_PADDING;
    int right = rightw + PKGI_MAIN_TEXT_PADDING;

    if (pkgi_menu_is_open())
    {
        pkgi_snprintf(text, sizeof(text), "%s %s  " PKGI_UTF8_T " %s  %s %s", pkgi_get_ok_str(), _("Select"), _("Close"), pkgi_get_cancel_str(), _("Cancel"));
    }
    else
    {
        uint32_t marked = pkgi_db_selected_count();
        if (selection_mode)
        {
            pkgi_snprintf(text, sizeof(text),
                _("%u selected  %s: mark/unmark  START: download  SELECT: done"),
                marked, pkgi_get_ok_str());
        }
        else if (marked > 0)
        {
            char sel[64];
            pkgi_snprintf(sel, sizeof(sel), _("%u selected"), marked);
            pkgi_snprintf(text, sizeof(text), "%s - START: %s", sel, _("Download all"));
        }
        else
        {
            pkgi_snprintf(text, sizeof(text), "%s %s  " PKGI_UTF8_T " %s  " PKGI_UTF8_S " %s  SELECT %s  %s %s", pkgi_get_ok_str(), _("Download"), _("Menu"), _("Details"), _("Selection"), pkgi_get_cancel_str(), _("Exit"));
        }
    }

    pkgi_clip_set(left, bottom_y, VITA_WIDTH - right - left, VITA_HEIGHT - bottom_y);
    pkgi_draw_text_z((VITA_WIDTH - pkgi_text_width(text)) / 2, bottom_y, PKGI_FONT_Z, PKGI_COLOR_TEXT_TAIL, text);
    pkgi_clip_remove();
}

static void pkgi_do_error(void)
{
    const char* text = error_state;
    const int line_height = PKGI_FONT_HEIGHT + 2;
    int line_count = 1;

    for (const char* p = error_state; *p; p++)
    {
        if (*p == '\n')
            line_count++;
    }

    const int margin = 32;
    pkgi_clip_set(margin, margin, VITA_WIDTH - 2 * margin,
                  VITA_HEIGHT - 2 * margin);

    int y = (VITA_HEIGHT - line_count * line_height) / 2;
    while (*text)
    {
        char line[sizeof(error_state)];
        uint32_t length = 0;

        while (text[length] && text[length] != '\n' &&
               length < sizeof(line) - 1)
        {
            line[length] = text[length];
            length++;
        }
        line[length] = 0;

        int width = pkgi_text_width_ttf(line);
        int available_width = VITA_WIDTH - 2 * margin;
        int x = width <= available_width
            ? margin + (available_width - width) / 2
            : margin;
        pkgi_draw_text_ttf(x, y, PKGI_FONT_Z,
                           PKGI_COLOR_TEXT_ERROR, line);
        y += line_height;

        text += length;
        if (*text == '\n')
            text++;
    }

    pkgi_clip_remove();
}

static void reposition(void)
{
    uint32_t count = pkgi_db_count();
    if (first_item + selected_item < count)
    {
        return;
    }

    uint32_t max_items = (avail_height + font_height + PKGI_MAIN_ROW_PADDING - 1) / (font_height + PKGI_MAIN_ROW_PADDING) - 1;
    if (count > max_items)
    {
        uint32_t delta = selected_item - first_item;
        first_item = count - max_items;
        selected_item = first_item + delta;
    }
    else
    {
        first_item = 0;
        selected_item = 0;
    }
}

const char * json_parse(json_object * jobj, const char* search)
{
    json_object *val;
    if (json_object_object_get_ex(jobj, search, &val) && (json_object_get_type(val) == json_type_string))
        return (json_object_get_string(val));

    return NULL;
}

static void pkgi_update_check_thread(void)
{
    const char *value;
    char *buffer;
    uint32_t size;

    LOG("checking latest pkgi version at %s", PKGI_UPDATE_URL);

    buffer = pkgi_http_download_buffer(PKGI_UPDATE_URL, &size);

    if (!buffer)
    {
        LOG("no update data available");
        pkgi_thread_exit();
    }

    json_object * jobj = json_tokener_parse(buffer);

    if ((value = json_parse(jobj, "name")) == NULL || !pkgi_memequ("PKGi PS3", value, 8) || pkgi_stricmp(PKGI_VERSION, value+10) == 0)
    {
        LOG("no new version available");
        pkgi_thread_exit();
    }

    LOG("latest version is %s", value+9);

    value = json_parse(json_object_array_get_idx(json_object_object_get(jobj, "assets"), 0), "browser_download_url");
    if (!value)
    {
        LOG("no download URL found");
        pkgi_thread_exit();
    }

	LOG("download URL is %s", value);

    DbItem update_item = {
        .content = "UP0001-NP00PKGR1_00-0000000000000000",
        .name    = "PKGi PS3 Update",
        .url     = value,
    };

    pkgi_dialog_start_progress(update_item.name, _("Preparing..."), 0);
    
    if (pkgi_download(&update_item, 0) && install(update_item.content, 0))
    {
        pkgi_dialog_message(update_item.name, _("Successfully downloaded PKGi PS3 update"));
        LOG("update downloaded!");
    }

    pkgi_thread_exit();
}

static void pkgi_load_language(const char* lang)
{
    char path[256];

    pkgi_snprintf(path, sizeof(path), PKGI_APP_FOLDER "/LANG/%s.po", lang);
    LOG("Loading language file (%s)...", path);
    mini18n_set_locale(path);
}

int main(int argc, const char* argv[])
{
    pkgi_start();

    pkgi_load_config(&config, (char*) &refresh_url, sizeof(refresh_url[0]));
    pkgi_set_download_folder(config.download_folder[0] ? config.download_folder : NULL);
    if (config.music)
    {
        pkgi_start_music();
    }
    
    pkgi_load_language(config.language);
    pkgi_dialog_init();
    
    font_height = pkgi_text_height("M");
    avail_height = VITA_HEIGHT - 2 * (font_height + PKGI_MAIN_HLINE_EXTRA*2 + PKGI_MAIN_VMARGIN)
                   - font_height / 2 - 4;
    bottom_y = VITA_HEIGHT - (PKGI_MAIN_VMARGIN + font_height);

    state = StateRefreshing;
    pkgi_start_thread("refresh_thread", &pkgi_refresh_thread);

    pkgi_texture background = pkgi_load_image_buffer(background, jpg);

    if (config.version_check)
    {
        pkgi_start_thread("update_thread", &pkgi_update_check_thread);
    }

    pkgi_input input = {0, 0, 0, 0};
    while (pkgi_update(&input) && (state != StateTerminate))
    {
        if (pkgi_dialog_is_background() && (input.pressed & PKGI_BUTTON_L2))
        {
            pkgi_dialog_restore_background();
            input.pressed &= ~PKGI_BUTTON_L2;
            input.active &= ~PKGI_BUTTON_L2;
        }

        pkgi_texture selected_art = NULL;
        if (state == StateMain && pkgi_db_count())
        {
            DbItem* selected = pkgi_db_get(selected_item);
            if (selected)
            {
                if (pkgi_stricmp(cover_selected_content, selected->content) != 0)
                {
                    pkgi_request_cover(selected);
                    pkgi_strncpy(cover_selected_content,
                                 sizeof(cover_selected_content), selected->content);
                }
                selected_art = pkgi_get_background_cover(selected);
            }
        }

        pkgi_draw_background(selected_art ? selected_art : background);
        if (selected_art)
        {
            /* Match the artwork's far plane so the tint cannot depth-occlude
             * default-depth UI; the interface is drawn after this overlay. */
            pkgi_draw_fill_rect_alpha_z(0, 0, PKGI_ART_TINT_Z,
                VITA_WIDTH, VITA_HEIGHT, PKGI_COLOR_ART_TINT, 178);
        }

        if (state == StateUpdateDone)
        {
            pkgi_db_configure(NULL, &config);
            state = StateMain;
        }

        pkgi_do_head();
        switch (state)
        {
        case StateError:
            pkgi_do_error();
            // leave the menu open if there's no database and we have URLs available
            if (!pkgi_menu_is_open() && config.allow_refresh)
            {
                config_temp = config;
                pkgi_menu_start(search_active, &config);
            }            
            break;

        case StateRefreshing:
            pkgi_do_refresh();
            break;

        case StateMain:
            pkgi_do_main((pkgi_dialog_is_open() && !pkgi_dialog_is_background()) ||
                         pkgi_menu_is_open() ? NULL : &input);
            break;

        default:
            // never happens, just to shut up the compiler
            break;
        }

        pkgi_do_tail();

        if (pkgi_dialog_is_open())
        {
            pkgi_do_dialog(&input);

            if (pkgi_dialog_is_cancelled())
            {
                pkgi_dialog_close();
            }
        }

        if (pkgi_dialog_take_details_install())
            pkgi_start_details_install();

        if (pkgi_dialog_input_update())
        {
            char input_text[256];
            pkgi_dialog_input_get_text(input_text, sizeof(input_text));

            if (osk_target_folder)
            {
                /* PKGi Remastered: OSK was opened for the download folder. */
                osk_target_folder = 0;

                if (input_text[0] == '/' && strstr(input_text, "dev_hdd0") == input_text + 1)
                {
                    pkgi_strncpy(config.download_folder, sizeof(config.download_folder), input_text);
                    pkgi_set_download_folder(config.download_folder);
                    pkgi_mkdirs(config.download_folder);
                    pkgi_save_config(&config, (char*)&refresh_url, sizeof(refresh_url[0]));
                    pkgi_dialog_message(_("Download folder"), input_text);
                }
                else
                {
                    pkgi_set_download_folder(NULL);
                    config.download_folder[0] = 0;
                    pkgi_dialog_error(_("Invalid folder: must be an absolute /dev_hdd0 path"));
                }
            }
            else
            {
                search_active = 1;
                pkgi_strncpy(search_text, sizeof(search_text), input_text);
                pkgi_db_configure(search_text, &config);
                reposition();
            }
        }

        if (pkgi_menu_is_open())
        {
            if (pkgi_do_menu(&input))
            {
                Config new_config;
                pkgi_menu_get(&new_config);
                if (config_temp.sort != new_config.sort ||
                    config_temp.order != new_config.order ||
                    config_temp.filter != new_config.filter ||
                    config_temp.grid_mode != new_config.grid_mode)
                {
                    config_temp = new_config;
                    pkgi_db_configure(search_active ? search_text : NULL, &config_temp);
                    reposition();
                }
                else if (config_temp.music != new_config.music)
                {
                    config_temp = new_config;
                    (config_temp.music ? pkgi_start_music() : pkgi_stop_music());
                }
            }
            else
            {
                MenuResult mres = pkgi_menu_result();
                if (mres == MenuResultSearch)
                {
                    pkgi_dialog_input_text(_("Search"), search_text);
                }
                else if (mres == MenuResultSearchClear)
                {
                    search_active = 0;
                    search_text[0] = 0;
                    pkgi_db_configure(NULL, &config);
                }
                else if (mres == MenuResultCancel)
                {
                    if (config_temp.sort != config.sort || config_temp.order != config.order ||
                        config_temp.filter != config.filter ||
                        config_temp.grid_mode != config.grid_mode)
                    {
                        pkgi_db_configure(search_active ? search_text : NULL, &config);
                        reposition();
                    }
                    if (config_temp.music != config.music)
                    {
                        (config.music ? pkgi_start_music() : pkgi_stop_music());
                    }
                }
                else if (mres == MenuResultAccept)
                {
                    pkgi_menu_get(&config);
                    pkgi_save_config(&config, (char*) &refresh_url, sizeof(refresh_url[0]));
                }
                else if (mres == MenuResultEditFolder)
                {
                    /* PKGi Remastered: ask the new download folder via
                     * the on-screen keyboard, validate, apply. */
                    osk_target_folder = 1;
                    pkgi_dialog_input_text(_("Download folder"),
                        config.download_folder[0] ? config.download_folder : PKGI_TMP_FOLDER);
                }
                else if (mres == MenuResultRefresh)
                {
                    state = StateRefreshing;
                    pkgi_start_thread("refresh_thread", &pkgi_refresh_thread);
                }
                else if (mres == MenuResultLoadConfig)
                {
                    /* Load the first available profile (transactional:
                     * on failure the current config stays untouched). */
                    if (pkgi_config_profile_count() > 0)
                    {
                        Config old_config = config;
                        char old_urls[MAX_CONTENT_TYPES][256];
                        pkgi_memcpy(old_urls, &refresh_url, sizeof(old_urls));

                        char cfg_error[192];
                        char cfg_ok[224];
                        const char* name = pkgi_config_profile_name(0);
                        if (pkgi_config_load_profile(name, &config,
                                (char*)&refresh_url, sizeof(refresh_url[0]),
                                cfg_error, sizeof(cfg_error)))
                        {
                            pkgi_snprintf(cfg_ok, sizeof(cfg_ok),
                                "%s: %s", _("Loaded configuration"), name);
                            pkgi_dialog_message(_("Configuration"), cfg_ok);
                            state = StateRefreshing;
                            pkgi_start_thread("refresh_thread", &pkgi_refresh_thread);
                        }
                        else
                        {
                            config = old_config;
                            pkgi_memcpy(&refresh_url, old_urls, sizeof(old_urls));
                            pkgi_dialog_error(cfg_error);
                        }
                    }
                    else
                    {
                        pkgi_dialog_message(_("Configuration"),
                            _("No configuration profile found in /profiles"));
                    }
                }
            }
        }

        pkgi_swap();
    }

    LOG("finished");
    mini18n_close();
    pkgi_free_covers();
    pkgi_free_texture(background);
    pkgi_end();
	return 0;
}
