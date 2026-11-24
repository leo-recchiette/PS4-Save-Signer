#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include <orbis/SaveData.h>
#include <orbis/NetCtl.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <mini18n.h>

#include "saves.h"
#include "menu.h"
#include "common.h"
#include "util.h"
#include "sfo.h"

typedef struct
{
	char* dir;      // folder that holds the save, ending with '/'
	char* name;     // save image file name (the sealed key is <name>.bin)
	const char* rel; // part of dir below the scanned root: kept in the destination
} import_save_t;

// where zipped saves are unpacked, mirroring their folders on the USB drive
#define IMPORT_ZIP_PATH         IMPORT_TMP_PATH "zip/"

static char host_buf[256];

static int _get_usb_path(char* usb_path)
{
	update_usb_path(usb_path);
	if (usb_path[0])
		return 1;

	show_message(_("Error! USB drive not found"));
	return 0;
}

static int _check_account_id(void)
{
	if (apollo_config.account_id)
		return 1;

	show_message("%s\n%s", _("Error! The current user has no Account ID."), _("Activate the account first (User Tools)."));
	return 0;
}

static int _update_save_details(const char* sys_path, const save_entry_t* save)
{
	char file_path[256];
	uint8_t* iconBuf;
	size_t iconSize;

	snprintf(file_path, sizeof(file_path), "%s" "param.sfo", sys_path);
	LOG("Update Save Details :: Reading %s...", file_path);

	sfo_context_t* sfo = sfo_alloc();
	if ((sfo_read(sfo, file_path) < 0) || !orbis_UpdateSaveParams(save,
		(char*) sfo_get_param_value(sfo, "MAINTITLE"), (char*) sfo_get_param_value(sfo, "SUBTITLE"),
		(char*) sfo_get_param_value(sfo, "DETAIL"), *(uint32_t*) sfo_get_param_value(sfo, "SAVEDATA_LIST_PARAM")))
	{
		LOG("Unable to read from '%s'", file_path);
		sfo_free(sfo);
		return 0;
	}

	sfo_free(sfo);

	snprintf(file_path, sizeof(file_path), "%s" "icon0.png", sys_path);
	if (read_buffer(file_path, &iconBuf, &iconSize) == SUCCESS)
	{
		snprintf(file_path, sizeof(file_path), SAVE_ICON_PATH_HDD "%s/%s_icon0.png", apollo_config.user_id, save->title_id, save->dir_name);
		mkdirs(file_path);

		if (write_buffer(file_path, iconBuf, iconSize) < 0)
			LOG("ERROR: can't write %s", file_path);

		free(iconBuf);
	}

	return 1;
}

/*
 * Copies an encrypted USB save to the HDD and resigns it for the current user
 */
static int _copy_save_pfs(const save_entry_t* save)
{
	char src_path[256];
	char hdd_path[256];
	char mount[ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE];
	sfo_patch_t patch = {
		.user_id = apollo_config.user_id,
		.account_id = apollo_config.account_id,
		.psid = (uint8_t*) apollo_config.psid,
	};

	snprintf(src_path, sizeof(src_path), "%s%s.bin", save->path, save->dir_name);
	if ((read_file(src_path, (uint8_t*) mount, 0x10) < 0) || get_max_pfskey_ver() < mount[8])
	{
		LOG("Error: Encrypted save Required firmware: %s", get_fw_by_pfskey_ver(mount[8]));
		return 0;
	}

	if (!orbis_SaveMount(save, ORBIS_SAVE_DATA_MOUNT_MODE_RDWR | ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2 | ORBIS_SAVE_DATA_MOUNT_MODE_COPY_ICON, mount))
	{
		LOG("Error: can't create HDD save");
		return 0;
	}
	orbis_SaveUmount(mount);

	// Copy the sdimg file
	snprintf(src_path, sizeof(src_path), "%s%s", save->path, save->dir_name);
	snprintf(hdd_path, sizeof(hdd_path), SAVES_PATH_HDD "%s/sdimg_%s", apollo_config.user_id, save->title_id, save->dir_name);
	LOG("Copying <%s> to %s...", src_path, hdd_path);

	if (copy_file(src_path, hdd_path) != SUCCESS)
	{
		LOG("Error: can't copy %s", hdd_path);
		return 0;
	}

	// Copy the .bin file
	snprintf(src_path, sizeof(src_path), "%s%s.bin", save->path, save->dir_name);
	snprintf(hdd_path, sizeof(hdd_path), SAVES_PATH_HDD "%s/%s.bin", apollo_config.user_id, save->title_id, save->dir_name);
	LOG("Copying <%s> to %s...", src_path, hdd_path);

	if (copy_file(src_path, hdd_path) != SUCCESS)
	{
		LOG("Error: can't copy %s", hdd_path);
		return 0;
	}

	// Now remount from HDD to patch SFO
	if (!orbis_SaveMount(save, ORBIS_SAVE_DATA_MOUNT_MODE_RDWR, mount))
	{
		LOG("Error! Can't mount encrypted save.");
		return 0;
	}

	snprintf(hdd_path, sizeof(hdd_path), APOLLO_SANDBOX_PATH "sce_sys/param.sfo", mount);
	patch_sfo(hdd_path, &patch);

	*strrchr(hdd_path, 'p') = 0;
	_update_save_details(hdd_path, save);
	orbis_SaveUmount(mount);

	LOG("Encrypted save copied: %s/%s", save->title_id, save->dir_name);
	return 1;
}

static void _show_copy_result(const char* msg, int done, int err_count, const char* path)
{
	if (err_count)
		show_message("%s %d\n" "%s %d\n\n%s", msg, done, _("Errors:"), err_count, path);
	else
		show_message("%s %d\n\n%s", msg, done, path);
}

static void copyAllSavesHDD(void)
{
	int done = 0, err_count = 0;
	char usb_path[256];
	uint64_t progress = 0;
	list_node_t *node;
	save_entry_t *item;
	list_t *list;

	if (!_check_account_id() || !_get_usb_path(usb_path))
		return;

	init_loading_screen(_("Searching for save games..."));
	list = ReadUsbSaves(usb_path);
	stop_loading_screen();

	if (!list_count(list))
	{
		UnloadGameList(list);
		show_message("%s\n%s" ENC_SAVES_PATH_USB, _("No encrypted save games found in:"), usb_path);
		return;
	}

	if (!show_dialog(DIALOG_TYPE_YESNO, "%s %zu\n\n%s\n%s", _("Save games found:"), list_count(list),
		_("Copy them to the HDD?"), _("Saves already on the HDD will be overwritten.")))
	{
		UnloadGameList(list);
		return;
	}

	init_progress_bar(_("Copying all saves..."));

	LOG("Copying all saves from '%s' to HDD...", usb_path);
	for (node = list_head(list); (item = list_get(node)); node = list_next(node))
	{
		update_progress_bar(progress++, list_count(list), item->name);

		if (item->type == FILE_TYPE_PS4 && _copy_save_pfs(item))
			done++;
		else
			err_count++;
	}

	end_progress_bar();
	UnloadGameList(list);

	_show_copy_result(_("Saves copied to HDD:"), done, err_count, "");
}

static void activateAccount(int user)
{
	uint64_t account = 0;
	char value[SFO_ACCOUNT_ID_SIZE*2+1];

	snprintf(value, sizeof(value), "%016lx", 0x6F6C6C6F70610000 + (~apollo_config.user_id & 0xFFFF));
	if (!osk_dialog_get_text(_("Enter the Account ID"), value, sizeof(value)))
		return;

	if (!sscanf(value, "%lx", &account))
	{
		show_message(_("Error! Account ID is not valid"));
		return;
	};

	LOG("Activating user=%d (%lx)...", user, account);
	if (regMgr_SetAccountId(user, &account) != SUCCESS)
	{
		show_message(_("Error! Couldn't activate user account"));
		return;
	}

	show_message("%s\n%s", _("Account successfully activated!"), _("A system reboot is required"));
}

// unpack every .zip below dir_path into IMPORT_ZIP_PATH, keeping its relative folder; returns how many zips failed
static int _extract_zips(const char* dir_path, size_t root_len)
{
	DIR *d = opendir(dir_path);
	struct dirent *dir;
	char path[1024];
	int failed = 0;

	if (!d)
		return 0;

	while ((dir = readdir(d)) != NULL)
	{
		size_t len = strlen(dir->d_name);

		if (dir->d_name[0] == '.')
			continue;

		snprintf(path, sizeof(path), "%s%s", dir_path, dir->d_name);
		if (dir->d_type == DT_DIR)
		{
			strcat(path, "/");
			failed += _extract_zips(path, root_len);
		}
		else if (dir->d_type == DT_REG && len > 4 && strcasecmp(dir->d_name + len - 4, ".zip") == 0)
		{
			char dest[1024];

			snprintf(dest, sizeof(dest), IMPORT_ZIP_PATH "%s", dir_path + root_len);
			if (!extract_zip(path, dest))
			{
				LOG("Error: can't extract (%s)", path);
				failed++;
			}
		}
	}

	closedir(d);
	return failed;
}

static void _find_encrypted_saves(const char* dir_path, size_t root_len, list_t* list)
{
	DIR *d;
	struct dirent *dir;
	char path[1024];
	uint64_t size;

	d = opendir(dir_path);
	if (!d)
		return;

	while ((dir = readdir(d)) != NULL)
	{
		// skip ".", ".." and hidden files (macOS "._*" resource forks, .DS_Store)
		if (dir->d_name[0] == '.')
			continue;

		if (dir->d_type == DT_DIR)
		{
			snprintf(path, sizeof(path), "%s%s/", dir_path, dir->d_name);
			_find_encrypted_saves(path, root_len, list);
			continue;
		}

		size_t len = strlen(dir->d_name);
		if (dir->d_type != DT_REG || (len > 4 && strcasecmp(dir->d_name + len - 4, ".bin") == 0))
			continue;

		// an encrypted save needs its 96-byte sealed key file
		snprintf(path, sizeof(path), "%s%s.bin", dir_path, dir->d_name);
		if (get_file_size(path, &size) < 0 || size != 0x60)
			continue;

		import_save_t* item = malloc(sizeof(import_save_t));
		item->dir = strdup(dir_path);
		item->name = strdup(dir->d_name);
		item->rel = item->dir + root_len;
		list_append(list, item);
	}

	closedir(d);
}

// returns 1 = resigned, 0 = error, 2 = skipped (already resigned)
static int _resign_import_save(const import_save_t* item, const char* dst_root, sfo_patch_t* patch)
{
	int ret = 0;
	uint64_t src_size, dst_size;
	char src[1024], dst[1024];
	char tmp_file[256], tmp_key[256];
	char title_id[10];
	char mount[ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE];
	uint8_t key_hdr[0x10];
	save_entry_t tmp_save;

	// the save's parent folder is the Title ID (e.g. .../CUSA09171/<save>)
	snprintf(src, sizeof(src), "%s", item->dir);
	src[strlen(src) - 1] = 0;
	snprintf(title_id, sizeof(title_id), "%.9s", strrchr(src, '/') ? strrchr(src, '/') + 1 : src);

	snprintf(src, sizeof(src), "%s%s.bin", item->dir, item->name);
	if (read_file(src, key_hdr, sizeof(key_hdr)) < 0 || get_max_pfskey_ver() < key_hdr[8])
	{
		LOG("Error: (%s) requires firmware %s", src, get_fw_by_pfskey_ver(key_hdr[8]));
		return 0;
	}

	// shortcut: "already resigned" = same-size image + 96-byte key at the destination, compare contents if packs get replaced under the same name
	snprintf(dst, sizeof(dst), "%s%s%s.bin", dst_root, item->rel, item->name);
	if (get_file_size(dst, &dst_size) == SUCCESS && dst_size == 0x60)
	{
		snprintf(src, sizeof(src), "%s%s", item->dir, item->name);
		snprintf(dst, sizeof(dst), "%s%s%s", dst_root, item->rel, item->name);
		if (get_file_size(src, &src_size) == SUCCESS && get_file_size(dst, &dst_size) == SUCCESS && src_size == dst_size)
			return 2;
	}

	// Work on a copy stored on the HDD: the source is left untouched,
	// and the mount paths stay short no matter how deep the source tree is
	snprintf(tmp_file, sizeof(tmp_file), IMPORT_TMP_PATH "%s", item->name);
	snprintf(tmp_key, sizeof(tmp_key), IMPORT_TMP_PATH "%s.bin", item->name);

	if (copy_file(src, tmp_key) != SUCCESS)
		goto end;

	snprintf(src, sizeof(src), "%s%s", item->dir, item->name);
	if (copy_file(src, tmp_file) != SUCCESS)
		goto end;

	memset(&tmp_save, 0, sizeof(tmp_save));
	tmp_save.path = IMPORT_TMP_PATH;
	tmp_save.title_id = title_id;
	tmp_save.dir_name = item->name;

	if (!orbis_SaveMount(&tmp_save, SAVE_FLAG_LOCKED, mount))
	{
		LOG("Error: can't mount (%s)", src);
		goto end;
	}

	snprintf(dst, sizeof(dst), APOLLO_SANDBOX_PATH "sce_sys/param.sfo", mount);
	ret = (patch_sfo(dst, patch) == SUCCESS);
	ret &= orbis_SaveUmount(mount);

	if (!ret)
	{
		LOG("Error: can't resign (%s)", src);
		goto end;
	}

	// Copy to the destination, keeping the original folder structure
	snprintf(dst, sizeof(dst), "%s%s%s", dst_root, item->rel, item->name);
	LOG("Copying resigned save to <%s>...", dst);
	ret = (copy_file(tmp_file, dst) == SUCCESS);

	snprintf(dst, sizeof(dst), "%s%s%s.bin", dst_root, item->rel, item->name);
	ret &= (copy_file(tmp_key, dst) == SUCCESS);

end:
	unlink_secure(tmp_file);
	unlink_secure(tmp_key);

	return ret;
}

static void _free_import_list(list_t* list)
{
	list_node_t *node;
	import_save_t *item;

	for (node = list_head(list); (item = list_get(node)); node = list_next(node))
	{
		free(item->dir);
		free(item->name);
		free(item);
	}
	list_free(list);
}

static void resignImportFolder(void)
{
	char usb_path[256], src_root[256], dst_root[256];
	int done = 0, err_count = 0, skipped = 0, zip_errors;
	char failed[1024] = "", info[1536];
	uint64_t progress = 0;
	list_node_t *node;
	import_save_t *item;
	list_t *list;
	sfo_patch_t patch = {
		.user_id = apollo_config.user_id,
		.psid = (u8*) apollo_config.psid,
		.account_id = apollo_config.account_id,
	};

	if (!_check_account_id() || !_get_usb_path(usb_path))
		return;

	snprintf(src_root, sizeof(src_root), "%s" IMPORT_PATH_USB, usb_path);
	snprintf(dst_root, sizeof(dst_root), "%s" ENC_SAVES_PATH_USB "%016lx/", usb_path, apollo_config.account_id);

	if (dir_exists(src_root) != SUCCESS)
	{
		show_message("%s\n%s", _("Error! Import folder is not available:"), src_root);
		return;
	}

	// shortcut: all zips are unpacked up front, needs free HDD space for the whole pack, unpack one by one if packs get big
	clean_directory(IMPORT_ZIP_PATH, "");
	zip_errors = _extract_zips(src_root, strlen(src_root));

	init_loading_screen(_("Searching for save games..."));
	list = list_alloc();
	_find_encrypted_saves(src_root, strlen(src_root), list);
	_find_encrypted_saves(IMPORT_ZIP_PATH, strlen(IMPORT_ZIP_PATH), list);
	stop_loading_screen();

	if (!list_count(list))
	{
		list_free(list);
		clean_directory(IMPORT_ZIP_PATH, "");
		show_message("%s\n%s\n\n%s %d", _("No encrypted save games found in:"), src_root, _(".zip files that could not be extracted:"), zip_errors);
		return;
	}

	if (!show_dialog(DIALOG_TYPE_YESNO, "%s %zu\n%s %d\n\n%s\n%s", _("Save games found:"), list_count(list),
		_(".zip files that could not be extracted:"), zip_errors, _("Resign and copy them to:"), dst_root))
	{
		_free_import_list(list);
		clean_directory(IMPORT_ZIP_PATH, "");
		return;
	}

	init_progress_bar(_("Resigning all saves..."));
	mkdirs(IMPORT_TMP_PATH);

	LOG("Resigning %zu saves from '%s' to '%s'...", list_count(list), src_root, dst_root);
	for (node = list_head(list); (item = list_get(node)); node = list_next(node))
	{
		update_progress_bar(progress++, list_count(list), item->name);
		switch (_resign_import_save(item, dst_root, &patch))
		{
			case 1: done++; break;
			case 2: skipped++; break;
			default:
				// the dialog only has room for a few names: the rest is in the log
				if (++err_count <= 6)
					snprintf(failed + strlen(failed), sizeof(failed) - strlen(failed), "\n! %.90s%.40s", item->rel, item->name);
		}
	}

	end_progress_bar();
	_free_import_list(list);
	clean_directory(IMPORT_ZIP_PATH, "");

	snprintf(info, sizeof(info), "%s\n%s %d%s%s", dst_root, _("Already resigned (skipped):"), skipped, failed, (err_count > 6) ? "\n..." : "");
	_show_copy_result(_("Saves resigned:"), done, err_count, info);
}

/*
 * Host values that cheats can read (account ID, PSID, user name...)
 */
static void* orbis_host_callback(int id, uint32_t* size)
{
	OrbisNetCtlInfo info;

	memset(host_buf, 0, sizeof(host_buf));

	switch (id)
	{
	case APOLLO_HOST_TEMP_PATH:
		if (size) *size = strlen(APOLLO_LOCAL_CACHE);
		return APOLLO_LOCAL_CACHE;

	case APOLLO_HOST_DATA_PATH:
		if (size) *size = strlen(APOLLO_DATA_PATH);
		return APOLLO_DATA_PATH;

	case APOLLO_HOST_SYS_NAME:
		if (sceSystemServiceParamGetString(ORBIS_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, host_buf, sizeof(host_buf)) < 0)
			LOG("Error getting System name");

		if (size) *size = strlen(host_buf);
		return host_buf;

	case APOLLO_HOST_PSID:
		memcpy(host_buf, apollo_config.psid, 16);
		if (size) *size = 16;
		return host_buf;

	case APOLLO_HOST_ACCOUNT_ID:
		memcpy(host_buf, &apollo_config.account_id, 8);
		*(uint64_t*)host_buf = ES64(*(uint64_t*)host_buf);
		if (size) *size = 8;
		return host_buf;

	case APOLLO_HOST_USERNAME:
		if (sceUserServiceGetUserName(apollo_config.user_id, host_buf, sizeof(host_buf)) < 0)
			LOG("Error getting Username");

		if (size) *size = strlen(host_buf);
		return host_buf;

	case APOLLO_HOST_LAN_ADDR:
	case APOLLO_HOST_WLAN_ADDR:
		memset(&info, 0, sizeof(OrbisNetCtlInfo));
		if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_ETHER_ADDR, &info) < 0)
			LOG("Error getting Wlan Ethernet Address");
		else
			memcpy(host_buf, info.ether_addr, 6);

		if (size) *size = 6;
		return host_buf;
	}

	if (size) *size = 1;
	return host_buf;
}

// entry->path must point to the mounted save
static int apply_cheat_patches(const save_entry_t* entry)
{
	int ret = 1;
	char tmpfile[256];
	char* filename;
	code_entry_t* code;
	list_node_t* node;

	for (node = list_head(entry->codes); (code = list_get(node)); node = list_next(node))
	{
		if (!code->activated || (code->type != PATCH_GAMEGENIE && code->type != PATCH_BSD && code->type != PATCH_PYTHON))
			continue;

		LOG("Active code: [%s]", code->name);

		if (strrchr(code->file, '\\'))
			filename = strrchr(code->file, '\\')+1;
		else
			filename = code->file;

		// target picked among the save files matching the mask
		if (strchr(filename, '*'))
		{
			option_value_t* optval = list_get_item(code->options[0].opts, code->options[0].sel);
			filename = optval->name;
		}

		if (strncmp(code->file, "~extracted\\", 11) == 0)
			snprintf(tmpfile, sizeof(tmpfile), "%s", code->file);
		else
			snprintf(tmpfile, sizeof(tmpfile), "%s%s", entry->path, filename);

		if (!apollo_apply_code(tmpfile, code, &orbis_host_callback))
		{
			LOG("Error: failed to apply (%s)", code->name);
			ret = 0;
		}

		code->activated = 0;
	}

	apollo_free_var_list();
	return ret;
}

/*
 * Applies the selected cheats to an encrypted USB save and resigns it for the current user
 */
void applyCheats(save_entry_t* entry)
{
	int ret;
	char *tmp;
	char path[256];
	char mount[ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE];
	sfo_patch_t patch = {
		.user_id = apollo_config.user_id,
		.psid = (u8*) apollo_config.psid,
		.account_id = apollo_config.account_id,
	};

	if (!orbis_SaveMount(entry, SAVE_FLAG_LOCKED, mount))
	{
		show_message(_("Error Mounting Save!"));
		return;
	}

	tmp = entry->path;
	asprintf(&entry->path, APOLLO_SANDBOX_PATH, mount);

	init_loading_screen(_("Applying changes..."));
	LOG("Applying cheats to '%s'...", entry->name);
	ret = apply_cheat_patches(entry);

	snprintf(path, sizeof(path), "%s" "sce_sys/param.sfo", entry->path);
	ret = (patch_sfo(path, &patch) == SUCCESS) ? ret : -1;
	stop_loading_screen();

	orbis_SaveUmount(mount);
	free(entry->path);
	entry->path = tmp;

	if (ret < 0)
		show_message(_("Error! Account changes couldn't be applied"));
	else if (!ret)
		show_message(_("Error! Cheat codes couldn't be applied"));
	else
		show_message(_("Save %s successfully modified!"), entry->title_id);

	if (ret > 0 && apollo_config.account_id)
		entry->flags |= SAVE_FLAG_OWNER;
}

void execCommand(const save_entry_t* entry)
{
	switch (entry->cmd)
	{
		case CMD_RESIGN_IMPORT_FOLDER:
			resignImportFolder();
			break;

		case CMD_COPY_ALL_SAVES_HDD:
			copyAllSavesHDD();
			break;

		case CMD_ACTIVATE_ACCOUNT:
			activateAccount(entry->arg);
			break;

		default:
			break;
	}
}
