#pragma once

#include "pkgi_db.h"

/* Load <config_folder>/config.txt into config; fills refresh_url
 * (refresh_len bytes per content type). Legacy layouts stay valid. */
void pkgi_load_config(Config* config, char* refresh_url, uint32_t refresh_len);
void pkgi_save_config(const Config* config, const char* update_url, uint32_t update_len);
const char* pkgi_content_tag(ContentType content);

/* Configuration profiles (PKGi Remastered):
 * files <config_folder>/profiles/<name>.txt with the config.txt syntax. */
int pkgi_config_profile_count(void);
const char* pkgi_config_profile_name(int index);

/* Transactional load: on success copies the parsed values into config
 * and refresh_url; on failure fills error and leaves everything
 * untouched. Returns 1 on success, 0 on failure. */
int pkgi_config_load_profile(const char* name, Config* config,
                             char* refresh_url, uint32_t refresh_len,
                             char* error, uint32_t error_size);
