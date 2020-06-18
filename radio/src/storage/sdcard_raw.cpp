/*
 * Copyright (C) OpenTX
 *
 * Based on code named
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"
#include "modelslist.h"
#include "conversions/conversions.h"

#if defined(EEPROM_SDCARD)
ModelHeader modelHeaders[MAX_MODELS];
#endif

void getModelPath(char * path, const char * filename)
{
  strcpy(path, STR_MODELS_PATH);
  path[sizeof(MODELS_PATH)-1] = '/';
  strcpy(&path[sizeof(MODELS_PATH)], filename);
}

const char * writeFile(const char * filename, const uint8_t * data, uint16_t size)
{
  TRACE("writeFile(%s)", filename);

  FIL file;
  unsigned char buf[8];
  UINT written;

  FRESULT result = f_open(&file, filename, FA_CREATE_ALWAYS | FA_WRITE);
  if (result != FR_OK) {
    return SDCARD_ERROR(result);
  }

  *(uint32_t*)&buf[0] = OTX_FOURCC;
  buf[4] = EEPROM_VER;
  buf[5] = 'M';
  *(uint16_t*)&buf[6] = size;

  result = f_write(&file, buf, 8, &written);
  if (result != FR_OK || written != 8) {
    f_close(&file);
    return SDCARD_ERROR(result);
  }

  result = f_write(&file, data, size, &written);
  if (result != FR_OK || written != size) {
    f_close(&file);
    return SDCARD_ERROR(result);
  }

  f_close(&file);
  return NULL;
}

const char * writeModel()
{
  char path[256];
  getModelPath(path, g_eeGeneral.currModelFilename);
  return writeFile(path, (uint8_t *)&g_model, sizeof(g_model));
}

const char * openFile(const char * fullpath, FIL * file, uint16_t * size, uint8_t * version)
{
  FRESULT result = f_open(file, fullpath, FA_OPEN_EXISTING | FA_READ);
  if (result != FR_OK) {
    return SDCARD_ERROR(result);
  }

  if (f_size(file) < 8) {
    f_close(file);
    return STR_INCOMPATIBLE;
  }

  UINT read;
  char buf[8];

  result = f_read(file, (uint8_t *)buf, sizeof(buf), &read);
  if ((result != FR_OK) || (read != sizeof(buf))) {
    f_close(file);
    return SDCARD_ERROR(result);
  }

  *version = (uint8_t)buf[4];
  if (*(uint32_t*)&buf[0] != OTX_FOURCC || *version < FIRST_CONV_EEPROM_VER || *version > EEPROM_VER || buf[5] != 'M') {
    f_close(file);
    return STR_INCOMPATIBLE;
  }

  *size = *(uint16_t*)&buf[6];
  return nullptr;
}

const char * loadFile(const char * fullpath, uint8_t * data, uint16_t maxsize, uint8_t * version)
{
  FIL      file;
  UINT     read;
  uint16_t size;

  TRACE("loadFile(%s)", fullpath);

   const char * err = openFile(fullpath, &file, &size, version);
  if (err)
    return err;

  size = min<uint16_t>(maxsize, size);
  FRESULT result = f_read(&file, data, size, &read);
  if (result != FR_OK || read != size) {
    f_close(&file);
    return SDCARD_ERROR(result);
  }

  f_close(&file);
  return nullptr;
}

const char * readModel(const char * filename, uint8_t * buffer, uint32_t size, uint8_t * version)
{
  char path[256];
  getModelPath(path, filename);
  return loadFile(path, buffer, size, version);
}

#if defined(PCBTANGO)
uint16_t getModelSize(const char * filename)
{
  char path[256];
  FIL      file;
  uint16_t size;
  uint8_t  version;

  memset(path, 0, sizeof(path));
  getModelPath(path, filename);

  const char * err = openFile(path, &file, &size, &version);
  if (err)
    return -1;

  f_close(&file);

  return size;
}
#endif

const char * loadModel(const char * filename, bool alarms)
{
  uint8_t version;

  preModelLoad();

  const char * error = readModel(filename, (uint8_t *)&g_model, sizeof(g_model), &version);
  if (error) {
    TRACE("loadModel error=%s", error);
  }
#if !defined(PCBTANGO)
  if (error) {
    modelDefault(0) ;
    storageCheck(true);
    alarms = false;
  }
#endif

#if defined(TANGO_CONVERT_VERSION_101)
  uint32_t model_size;

  model_size = getModelSize(filename);
  if (model_size == MODEL_DATA_SIZE_101)
  {
    convertModelData(version);
    writeModel();
    model_size = getModelSize(filename);
    if (model_size == MODEL_DATA_SIZE_110)
    {
      TRACE("convert model data of v1.0.1 success");
    }
    else
    {
      TRACE("convert model data of v1.0.1 failed");
    }
  }
#endif

  if (version < EEPROM_VER) {
    convertModelData(version);
  }

  postModelLoad(alarms);

  return error;
}

const char * writeGeneralSettings()
{
  return writeFile(RADIO_SETTINGS_PATH, (uint8_t *)&g_eeGeneral, sizeof(g_eeGeneral));
}

const char * loadRadioSettings(const char * path)
{
  uint8_t version;
  const char * error = loadFile(path, (uint8_t *)&g_eeGeneral, sizeof(g_eeGeneral), &version);
  if (error) {
    TRACE("loadRadioSettings error=%s", error);
    return error;
  }

#if defined(TANGO_CONVERT_VERSION_101)
  convertRadioData(version);
  writeGeneralSettings();
  TRACE("convert radio data of v1.0.1 success");
#endif

  if (version < EEPROM_VER) {
    convertRadioData(version);
  }

  postRadioSettingsLoad();
  return nullptr;
}

const char * loadRadioSettings()
{
  return loadRadioSettings(RADIO_SETTINGS_PATH);
}


void storageCheck(bool immediately)
{
  if (storageDirtyMsk & EE_GENERAL) {
    TRACE("Storage write general");
    storageDirtyMsk -= EE_GENERAL;
    const char * error = writeGeneralSettings();
    if (error) {
      TRACE("writeGeneralSettings error=%s", error);
    }
  }

  if (storageDirtyMsk & EE_MODEL) {
    TRACE("Storage write current model");
    storageDirtyMsk -= EE_MODEL;
    const char * error = writeModel();
    if (error) {
      TRACE("writeModel error=%s", error);
    }
  }
}

void storageReadAll()
{
  TRACE("storageReadAll");

  if (loadRadioSettings() != nullptr) {
    storageEraseAll(true);
#if defined(PCBTANGO)
    setStatusFlag(STORAGE_ERASE_STATUS);
#endif
  }

  for (uint8_t i = 0; languagePacks[i] != nullptr; i++) {
    if (!strncmp(g_eeGeneral.ttsLanguage, languagePacks[i]->id, 2)) {
      currentLanguagePackIdx = i;
      currentLanguagePack = languagePacks[i];
    }
  }

  if (loadModel(g_eeGeneral.currModelFilename, false) != nullptr) {
    sdCheckAndCreateDirectory(MODELS_PATH);
    createModel();
  }

  // Wipe models list in case
  // it's being reloaded after USB connection
  modelslist.clear();

  // and reload the list
  modelslist.load();
}

void storageCreateModelsList()
{
  FIL file;

  FRESULT result = f_open(&file, RADIO_MODELSLIST_PATH, FA_CREATE_ALWAYS | FA_WRITE);
  if (result == FR_OK) {
    f_puts("[" DEFAULT_CATEGORY "]\n" DEFAULT_MODEL_FILENAME "\n", &file);
    f_close(&file);
  }
}

void storageFormat()
{
  sdCheckAndCreateDirectory(RADIO_PATH);
  sdCheckAndCreateDirectory(MODELS_PATH);
  storageCreateModelsList();
}

const char * createModel()
{
  preModelLoad();

  char filename[LEN_MODEL_FILENAME+1];
  memset(filename, 0, sizeof(filename));
  strcpy(filename, "model.bin");

  int index = findNextFileIndex(filename, LEN_MODEL_FILENAME, MODELS_PATH);
  if (index > 0) {
    modelDefault(index);
    memcpy(g_eeGeneral.currModelFilename, filename, sizeof(g_eeGeneral.currModelFilename));
    storageDirty(EE_GENERAL);
    storageDirty(EE_MODEL);
    storageCheck(true);
  }
  postModelLoad(false);

  return g_eeGeneral.currModelFilename;
}

void storageEraseAll(bool warn)
{
  TRACE("storageEraseAll");

#if defined(COLORLCD)
  // the theme has not been loaded before
  theme->load();
#endif

  generalDefault();
  modelDefault(1);

  if (warn) {
    ALERT(STR_STORAGE_WARNING, STR_BAD_RADIO_DATA, AU_BAD_RADIODATA);
  }

  RAISE_ALERT(STR_STORAGE_WARNING, STR_STORAGE_FORMAT, NULL, AU_NONE);

  storageFormat();
  storageDirty(EE_GENERAL|EE_MODEL);
  storageCheck(true);
}
