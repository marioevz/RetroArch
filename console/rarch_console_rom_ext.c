/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2013 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2013 - Daniel De Matteis
 * 
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <ctype.h>

#ifdef _WIN32
#include "../compat/posix_string.h"
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include "../general.h"
#include "../file.h"

#include "rarch_console_settings.h"
#include "rarch_console.h"
#include "rarch_console_rom_ext.h"

#ifdef HAVE_ZLIB
#include "../deps/rzlib/zlib.h"

static int rarch_extract_currentfile_in_zip(unzFile uf, const char *current_dir, char *slash, char *write_filename, size_t write_filename_size, unsigned extract_zip_mode)
{
   char filename_inzip[PATH_MAX];
   bool is_dir = false;
   FILE *file_out = NULL;

   unz_file_info file_info;
   int ret = unzGetCurrentFileInfo(uf,
         &file_info, filename_inzip, sizeof(filename_inzip),
         NULL, 0, NULL, 0);

   if (ret != UNZ_OK)
   {
      RARCH_ERR("Error %d while trying to get ZIP file information.\n", ret);
      return ret;
   }

   size_t size_buf = WRITEBUFFERSIZE;
   void *buf = malloc(size_buf);
   if (!buf)
   {
      RARCH_ERR("Error allocating memory for ZIP extract operation.\n");
      return UNZ_INTERNALERROR;
   }

   switch(extract_zip_mode)
   {
      case ZIP_EXTRACT_TO_CURRENT_DIR:
      case ZIP_EXTRACT_TO_CURRENT_DIR_AND_LOAD_FIRST_FILE:
         snprintf(write_filename, write_filename_size, "%s%s%s", current_dir, slash, filename_inzip);
         break;
#ifdef HAVE_HDD_CACHE_PARTITION
      case ZIP_EXTRACT_TO_CACHE_DIR:
         snprintf(write_filename, write_filename_size, "%s%s", default_paths.cache_dir, filename_inzip);
         break;
#endif
   }

   if(filename_inzip[strlen(filename_inzip) - 1] == '/')
      is_dir = true;

   ret = unzOpenCurrentFile(uf);
   if (ret != UNZ_OK)
      RARCH_ERR("Error %d while trying to open ZIP file.\n", ret);
   else
   {
      /* success */
      if(is_dir)
      {
#ifdef _WIN32
         _mkdir(write_filename);
#else
         mkdir(write_filename, S_IRWXU | S_IRWXG | S_IRWXO | S_IFDIR);
#endif
      }
      else
      {
         file_out = fopen(write_filename, "wb");

         if (!file_out)
            RARCH_ERR("Error opening %s.\n", write_filename);
      }
   }

   if (is_dir || file_out)
   {
      RARCH_LOG("Extracting: %s..\n", write_filename);

      do
      {
         ret = unzReadCurrentFile(uf, buf, size_buf);
         if (ret < 0)
         {
            RARCH_ERR("Error %d while reading from ZIP file.\n", ret);
            break;
         }

         if (ret > 0 && !is_dir)
         {
            if (fwrite(buf, ret, 1, file_out) != 1)
            {
               RARCH_ERR("Error while extracting file(s) from ZIP.\n");
               ret = UNZ_ERRNO;
               break;
            }
         }
      }while (ret > 0);

      if (!is_dir && file_out)
         fclose(file_out);
   }

   if (ret == UNZ_OK)
   {
      ret = unzCloseCurrentFile (uf);
      if (ret != UNZ_OK)
         RARCH_ERR("Error %d while trying to close ZIP file.\n", ret);
   }
   else
      unzCloseCurrentFile(uf); 

   free(buf);
   return ret;
}

int rarch_extract_zipfile(const char *zip_path, char *first_file, size_t first_file_size, unsigned extract_zip_mode)
{
   char dir_path[PATH_MAX];
   bool found_first_file = false;
   (void)found_first_file;

   fill_pathname_basedir(dir_path, zip_path, sizeof(dir_path));

   unzFile uf = unzOpen(zip_path); 

   unz_global_info gi;
   memset(&gi, 0, sizeof(unz_global_info));

   int ret = unzGetGlobalInfo(uf, &gi);
   if(ret != UNZ_OK)
      RARCH_ERR("Error %d while trying to get ZIP file global info.\n", ret);

   for (unsigned i = 0; i < gi.number_entry; i++)
   {
      static char write_filename[PATH_MAX];
      char slash[6];
#ifdef _XBOX
      snprintf(slash, sizeof(slash), "\\");
#else
      snprintf(slash, sizeof(slash), "/");
#endif
      if (rarch_extract_currentfile_in_zip(uf, dir_path, slash, write_filename, sizeof(write_filename), extract_zip_mode) != UNZ_OK)
      {
         RARCH_ERR("Failed to extract current file from ZIP archive.\n");
         break;
      }
#ifdef HAVE_LIBRETRO_MANAGEMENT
      else
      {
         if(!found_first_file)
         {
            // is the extension of the file supported by the libretro core?
            struct string_list *ext_list = NULL;
            const char *file_ext = path_get_extension(write_filename);
            const char *ext = rarch_console_get_rom_ext();

            if (ext)
               ext_list = string_split(ext, "|");

            if (ext_list && string_list_find_elem(ext_list, file_ext))
               found_first_file = true; 

            if(found_first_file)
               snprintf(first_file, first_file_size, write_filename);
         }
      }
#endif

      if ((i + 1) < gi.number_entry)
      {
         ret = unzGoToNextFile(uf);
         if (ret != UNZ_OK)
         {
            RARCH_ERR("Error %d while trying to go to the next file in the ZIP archive.\n", ret);
            break;
         }
      }
   }

   return 0;
}

#endif

void rarch_console_load_game_wrap(const char *path, unsigned extract_zip_mode, unsigned delay)
{
   const char *game_to_load;
   char first_file_inzip[PATH_MAX];
   struct retro_system_info info;
   bool extract_zip_cond = false;
   bool extract_zip_and_load_game_cond = false;
   bool load_game = !extract_zip_cond;

   retro_get_system_info(&info);

#ifdef HAVE_ZLIB
   extract_zip_cond = (strstr(path, ".zip") || strstr(path, ".ZIP"))
   && !info.block_extract;

   if(extract_zip_cond)
   {
      rarch_extract_zipfile(path, first_file_inzip, sizeof(first_file_inzip), extract_zip_mode);
      if(g_extern.console.rmenu.state.msg_info.enable)
         rarch_settings_msg(S_MSG_EXTRACTED_ZIPFILE, S_DELAY_180);
   }

   extract_zip_and_load_game_cond = (extract_zip_cond && 
   g_extern.file_state.zip_extract_mode == ZIP_EXTRACT_TO_CURRENT_DIR_AND_LOAD_FIRST_FILE);
   load_game = (extract_zip_and_load_game_cond) || (!extract_zip_cond);

   if(extract_zip_and_load_game_cond)
      game_to_load = first_file_inzip;
   else
#endif
      game_to_load = path;

   if(load_game)
   {
      snprintf(g_extern.file_state.rom_path, sizeof(g_extern.file_state.rom_path), game_to_load);
      rarch_settings_change(S_START_RARCH);

      if(g_extern.console.rmenu.state.msg_info.enable)
         rarch_settings_msg(S_MSG_LOADING_ROM, delay);
   }
}

const char *rarch_console_get_rom_ext(void)
{
   const char *retval = NULL;

   struct retro_system_info info;
   retro_get_system_info(&info);

   if (info.valid_extensions)
      retval = info.valid_extensions;
   else
      retval = "ZIP|zip";

   return retval;
}
