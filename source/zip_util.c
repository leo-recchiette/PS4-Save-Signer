#include <zip.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <mini18n.h>

#include "saves.h"
#include "common.h"

#define UNZIP_BUF_SIZE 0x40000

static int unpack_zip_file(struct zip_file* zfd, const char* path, uint8_t* buf)
{
	FILE* tfd;
	ssize_t read;

	if (!zfd) {
		LOG("Unable to open file in zip.");
		return 0;
	}

	tfd = fopen(path, "wb");
	if(!tfd) {
		zip_fclose(zfd);
		LOG("Error opening file '%s'.", path);
		return 0;
	}

	for(read = zip_fread(zfd, buf, UNZIP_BUF_SIZE); read > 0; read = zip_fread(zfd, buf, UNZIP_BUF_SIZE)) {
		fwrite(buf, read, 1, tfd);
	}

	fclose(tfd);
	zip_fclose(zfd);

	if (read < 0) {
		LOG("Error reading from zip.");
		return 0;
	}

	return 1;
}

int extract_zip(const char* zip_file, const char* dest_path)
{
	char path[256];
	uint8_t* buffer;
	// no ZIP_CHECKCONS: libzip 1.9.2 rejects zips made by macOS (size in the local header + data descriptor)
	struct zip* archive = zip_open(zip_file, ZIP_RDONLY, NULL);
	int files = zip_get_num_files(archive);

	if (files <= 0) {
		LOG("Empty ZIP file.");
		zip_close(archive);
		return 0;
	}

	buffer = malloc(UNZIP_BUF_SIZE);
	if (!buffer)
	{
		zip_close(archive);
		return 0;
	}

	init_progress_bar(_("Extracting files..."));
	LOG("Extracting %s to <%s>...", zip_file, dest_path);

	for (int i = 0; i < files; i++) {
		const char* filename = zip_get_name(archive, i, 0);
		if (!filename)
			continue;

		update_progress_bar(i+1, files, filename);
		LOG("Unzip [%d/%d] '%s'...", i+1, files, filename);

		if (filename[0] == '/')
			filename++;

		// zip-slip: never write outside dest_path
		if (strstr(filename, ".."))
			continue;

		snprintf(path, sizeof(path)-1, "%s%s", dest_path, filename);
		mkdirs(path);

		if (filename[strlen(filename) - 1] == '/')
			continue;

		if(!unpack_zip_file(zip_fopen_index(archive, i, 0), path, buffer)) {
			free(buffer);
			zip_close(archive);
			end_progress_bar();
			return 0;
		}
	}

	free(buffer);
	zip_close(archive);
	end_progress_bar();

	return files;
}
