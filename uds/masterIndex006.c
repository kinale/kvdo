/*
 * Copyright (c) 2020 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA. 
 *
 * $Id: //eng/uds-releases/krusty/src/uds/masterIndex006.c#18 $
 */
#include "masterIndex006.h"

#include "buffer.h"
#include "compiler.h"
#include "errors.h"
#include "hashUtils.h"
#include "logger.h"
#include "masterIndex005.h"
#include "memoryAlloc.h"
#include "permassert.h"
#include "threads.h"
#include "uds.h"

/*
 * The master index is a kept as a wrapper around 2 master index
 * implementations, one for dense chapters and one for sparse chapters.
 * Methods will be routed to one or the other, or both, depending on the
 * method and data passed in.
 *
 * The master index is divided into zones, and in normal operation there is
 * one thread operating on each zone.  Any operation that operates on all
 * the zones needs to do its operation at a safe point that ensures that
 * only one thread is operating on the master index.
 *
 * The only multithreaded operation supported by the sparse master index is
 * the lookupMasterIndexName() method.  It is called by the thread that
 * assigns an index request to the proper zone, and needs to do a master
 * index query for sampled chunk names.  The zone mutexes are used to make
 * this lookup operation safe.
 */

struct master_index_zone {
  Mutex hookMutex;          // Protects the sampled index in this zone
} __attribute__((aligned(CACHE_LINE_BYTES)));

struct master_index6 {
  struct master_index       common;           // Common master index methods
  unsigned int              sparseSampleRate; // The sparse sample rate
  unsigned int              numZones;         // The number of zones
  struct master_index      *miNonHook;        // The non-hook index
  struct master_index      *miHook;           // The hook index == sample index
  struct master_index_zone *masterZones;      // The zones
};

/**
 * Determine whether a given chunk name is a hook.
 *
 * @param masterIndex    The master index
 * @param name           The block name
 *
 * @return whether to use as sample
 **/
static INLINE bool
isMasterIndexSample_006(const struct master_index *masterIndex,
                        const struct uds_chunk_name *name)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  return (extract_sampling_bytes(name) % mi6->sparseSampleRate) == 0;
}

/***********************************************************************/
/**
 * Get the subindex for the given chunk name
 *
 * @param masterIndex    The master index
 * @param name           The block name
 *
 * @return the subindex
 **/
static INLINE struct master_index *
getSubIndex(const struct master_index *masterIndex,
            const struct uds_chunk_name *name)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  return (isMasterIndexSample_006(masterIndex, name)
          ? mi6->miHook
          : mi6->miNonHook);
}

/***********************************************************************/
/**
 * Terminate and clean up the master index
 *
 * @param masterIndex The master index to terminate
 **/
static void freeMasterIndex_006(struct master_index *masterIndex)
{
  if (masterIndex != NULL) {
    struct master_index6 *mi6 = container_of(masterIndex,
                                             struct master_index6,
                                             common);
    if (mi6->masterZones != NULL) {
      unsigned int zone;
      for (zone = 0; zone < mi6->numZones; zone++) {
        destroyMutex(&mi6->masterZones[zone].hookMutex);
      }
      FREE(mi6->masterZones);
      mi6->masterZones = NULL;
    }
    if (mi6->miNonHook != NULL) {
      freeMasterIndex(mi6->miNonHook);
      mi6->miNonHook = NULL;
    }
    if (mi6->miHook != NULL) {
      freeMasterIndex(mi6->miHook);
      mi6->miHook = NULL;
    }
    FREE(masterIndex);
  }
}

/***********************************************************************/
/**
 * Constants and structures for the saved master index file.  "MI6" is for
 * masterIndex006, and "-XXXX" is a number to increment when the format of
 * the data changes.
 **/
enum { MAGIC_SIZE = 8 };
static const char MAGIC_MI_START[] = "MI6-0001";

struct mi006_data {
  char         magic[MAGIC_SIZE]; // MAGIC_MI_START
  unsigned int sparseSampleRate;
};

/***********************************************************************/
/**
 * Set the tag value used when saving and/or restoring a master index.
 *
 * @param masterIndex  The master index
 * @param tag          The tag value
 **/
static void setMasterIndexTag_006(struct master_index *masterIndex
                                  __attribute__((unused)),
                                  byte tag __attribute__((unused)))
{
}

/***********************************************************************/
static int __must_check
encodeMasterIndexHeader(struct buffer *buffer, struct mi006_data *header)
{
  int result = put_bytes(buffer, MAGIC_SIZE, MAGIC_MI_START);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = put_uint32_le_into_buffer(buffer, header->sparseSampleRate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(content_length(buffer) == sizeof(struct mi006_data),
                           "%zu bytes of config written, of %zu expected",
                           content_length(buffer), sizeof(struct mi006_data));
  return result;
}

/**
 * Start saving a master index to a buffered output stream.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The number of the zone to save
 * @param bufferedWriter  The index state component being written
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
startSavingMasterIndex_006(const struct master_index *masterIndex,
                           unsigned int               zoneNumber,
                           struct buffered_writer    *bufferedWriter)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  struct buffer *buffer;
  int result = make_buffer(sizeof(struct mi006_data), &buffer);
  if (result != UDS_SUCCESS) {
    return result;
  }
  struct mi006_data header;
  memset(&header, 0, sizeof(header));
  memcpy(header.magic, MAGIC_MI_START, MAGIC_SIZE);
  header.sparseSampleRate = mi6->sparseSampleRate;
  result = encodeMasterIndexHeader(buffer, &header);
  if (result != UDS_SUCCESS) {
    free_buffer(&buffer);
    return result;
  }
  result = write_to_buffered_writer(bufferedWriter, get_buffer_contents(buffer),
                                    content_length(buffer));
  free_buffer(&buffer);
  if (result != UDS_SUCCESS) {
    logWarningWithStringError(result, "failed to write master index header");
    return result;
  }

  result = startSavingMasterIndex(mi6->miNonHook, zoneNumber, bufferedWriter);
  if (result != UDS_SUCCESS) {
    return result;
  }

  result = startSavingMasterIndex(mi6->miHook, zoneNumber, bufferedWriter);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return UDS_SUCCESS;
}

/***********************************************************************/
/**
 * Have all the data been written while saving a master index to an output
 * stream?  If the answer is yes, it is still necessary to call
 * finishSavingMasterIndex(), which will return quickly.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return true if all the data are written
 **/
static bool isSavingMasterIndexDone_006(const struct master_index *masterIndex,
                                        unsigned int zoneNumber)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  return (isSavingMasterIndexDone(mi6->miNonHook, zoneNumber)
          && isSavingMasterIndexDone(mi6->miHook, zoneNumber));
}

/***********************************************************************/
/**
 * Finish saving a master index to an output stream.  Force the writing of
 * all of the remaining data.  If an error occurred asynchronously during
 * the save operation, it will be returned here.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int finishSavingMasterIndex_006(const struct master_index *masterIndex,
                                       unsigned int zoneNumber)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  int result = finishSavingMasterIndex(mi6->miNonHook, zoneNumber);
  if (result == UDS_SUCCESS) {
    result = finishSavingMasterIndex(mi6->miHook, zoneNumber);
  }
  return result;
}

/***********************************************************************/
/**
 * Abort saving a master index to an output stream.  If an error occurred
 * asynchronously during the save operation, it will be dropped.
 *
 * @param masterIndex  The master index
 * @param zoneNumber   The number of the zone to save
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int abortSavingMasterIndex_006(const struct master_index *masterIndex,
                                      unsigned int zoneNumber)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  int result = abortSavingMasterIndex(mi6->miNonHook, zoneNumber);
  int result2 = abortSavingMasterIndex(mi6->miHook, zoneNumber);
  if (result == UDS_SUCCESS) {
    result = result2;
  }
  return result;
}

/***********************************************************************/
static int __must_check
decodeMasterIndexHeader(struct buffer *buffer, struct mi006_data *header)
{
  int result = get_bytes_from_buffer(buffer, sizeof(header->magic),
                                     &header->magic);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = get_uint32_le_from_buffer(buffer, &header->sparseSampleRate);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_LOG_ONLY(content_length(buffer) == 0,
                           "%zu bytes decoded of %zu expected",
                           buffer_length(buffer) - content_length(buffer),
                           buffer_length(buffer));
  if (result != UDS_SUCCESS) {
    result = UDS_CORRUPT_COMPONENT;
  }
  return result;
}

/**
 * Start restoring the master index from multiple buffered readers
 *
 * @param masterIndex      The master index to restore into
 * @param bufferedReaders  The buffered reader to read the master index from
 * @param numReaders       The number of buffered readers
 *
 * @return UDS_SUCCESS on success, or an error code on failure
 **/
static int
startRestoringMasterIndex_006(struct master_index     *masterIndex,
                              struct buffered_reader **bufferedReaders,
                              int                      numReaders)
{
  struct master_index6 *mi6 = container_of(masterIndex,
                                           struct master_index6,
                                           common);
  int result = ASSERT_WITH_ERROR_CODE(masterIndex != NULL, UDS_BAD_STATE,
                                      "cannot restore to null master index");
  if (result != UDS_SUCCESS) {
    return result;
  }

  int i;
  for (i = 0; i < numReaders; i++) {
    struct buffer *buffer;
    result = make_buffer(sizeof(struct mi006_data), &buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    result = read_from_buffered_reader(bufferedReaders[i],
                                       get_buffer_contents(buffer),
                                       buffer_length(buffer));
    if (result != UDS_SUCCESS) {
      free_buffer(&buffer);
      return logWarningWithStringError(result,
                                       "failed to read master index header");
    }
    result = reset_buffer_end(buffer, buffer_length(buffer));
    if (result != UDS_SUCCESS) {
      free_buffer(&buffer);
      return result;
    }
    struct mi006_data header;
    result = decodeMasterIndexHeader(buffer, &header);
    free_buffer(&buffer);
    if (result != UDS_SUCCESS) {
      return result;
    }
    if (memcmp(header.magic, MAGIC_MI_START, MAGIC_SIZE) != 0) {
      return logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                       "master index file had bad magic"
                                       " number");
    }
    if (i == 0) {
      mi6->sparseSampleRate = header.sparseSampleRate;
    } else if (mi6->sparseSampleRate != header.sparseSampleRate) {
      logWarningWithStringError(UDS_CORRUPT_COMPONENT,
                                "Inconsistent sparse sample rate in delta"
                                " index zone files: %u vs. %u",
                                mi6->sparseSampleRate,
                                header.sparseSampleRate);
        return UDS_CORRUPT_COMPONENT;
    }
  }

  result = startRestoringMasterIndex(mi6->miNonHook, bufferedReaders,
                                     numReaders);
  if (result != UDS_SUCCESS) {
    return result;
  }
  return startRestoringMasterIndex(mi6->miHook, bufferedReaders, numReaders);
}

/***********************************************************************/
/**
 * Have all the data been read while restoring a master index from an
 * input stream?
 *
 * @param masterIndex  The master index to restore into
 *
 * @return true if all the data are read
 **/
static bool
isRestoringMasterIndexDone_006(const struct master_index *masterIndex)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  return (isRestoringMasterIndexDone(mi6->miNonHook)
          && isRestoringMasterIndexDone(mi6->miHook));
}

/***********************************************************************/
/**
 * Restore a saved delta list
 *
 * @param masterIndex  The master index to restore into
 * @param dlsi         The delta_list_save_info describing the delta list
 * @param data         The saved delta list bit stream
 *
 * @return error code or UDS_SUCCESS
 **/
static int
restoreDeltaListToMasterIndex_006(struct master_index *masterIndex,
                                  const struct delta_list_save_info *dlsi,
                                  const byte data[DELTA_LIST_MAX_BYTE_COUNT])
{
  struct master_index6 *mi6 = container_of(masterIndex,
                                           struct master_index6,
                                           common);
  int result = restoreDeltaListToMasterIndex(mi6->miNonHook, dlsi, data);
  if (result != UDS_SUCCESS) {
    result = restoreDeltaListToMasterIndex(mi6->miHook, dlsi, data);
  }
  return result;
}

/***********************************************************************/
/**
 * Abort restoring a master index from an input stream.
 *
 * @param masterIndex  The master index
 **/
static void abortRestoringMasterIndex_006(struct master_index *masterIndex)
{
  struct master_index6 *mi6 = container_of(masterIndex,
                                           struct master_index6,
                                           common);
  abortRestoringMasterIndex(mi6->miNonHook);
  abortRestoringMasterIndex(mi6->miHook);
}

/***********************************************************************/
/**
 * Set the open chapter number on a zone.  The master index zone will be
 * modified to index the proper number of chapters ending with the new open
 * chapter.
 *
 * @param masterIndex     The master index
 * @param zoneNumber      The zone number
 * @param virtualChapter  The new open chapter number
 **/
static void setMasterIndexZoneOpenChapter_006(struct master_index *masterIndex,
                                              unsigned int zoneNumber,
                                              uint64_t virtualChapter)
{
  struct master_index6 *mi6 = container_of(masterIndex,
                                           struct master_index6,
                                           common);
  setMasterIndexZoneOpenChapter(mi6->miNonHook, zoneNumber, virtualChapter);

  // We need to prevent a lookupMasterIndexName() happening while we are
  // changing the open chapter number
  Mutex *mutex = &mi6->masterZones[zoneNumber].hookMutex;
  lockMutex(mutex);
  setMasterIndexZoneOpenChapter(mi6->miHook, zoneNumber, virtualChapter);
  unlockMutex(mutex);
}

/***********************************************************************/
/**
 * Set the open chapter number.  The master index will be modified to index
 * the proper number of chapters ending with the new open chapter.
 *
 * @param masterIndex     The master index
 * @param virtualChapter  The new open chapter number
 **/
static void setMasterIndexOpenChapter_006(struct master_index *masterIndex,
                                          uint64_t virtualChapter)
{
  struct master_index6 *mi6 = container_of(masterIndex,
                                           struct master_index6,
                                           common);
  unsigned int zone;
  for (zone = 0; zone < mi6->numZones; zone++) {
    setMasterIndexZoneOpenChapter_006(masterIndex, zone, virtualChapter);
  }
}

/***********************************************************************/
/**
 * Find the master index zone associated with a chunk name
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 *
 * @return the zone that the chunk name belongs to
 **/
static unsigned int
getMasterIndexZone_006(const struct master_index *masterIndex,
                       const struct uds_chunk_name *name)
{
  return getMasterIndexZone(getSubIndex(masterIndex, name), name);
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the chunk name and return information
 * needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name
 *
 * @return UDS_SUCCESS or an error code
 **/
static int lookupMasterIndexName_006(const struct master_index *masterIndex,
                                     const struct uds_chunk_name *name,
                                     struct master_index_triage *triage)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  triage->isSample = isMasterIndexSample_006(masterIndex, name);
  triage->inSampledChapter = false;
  triage->zone = getMasterIndexZone_006(masterIndex, name);
  int result = UDS_SUCCESS;
  if (triage->isSample) {
    Mutex *mutex = &mi6->masterZones[triage->zone].hookMutex;
    lockMutex(mutex);
    result = lookupMasterIndexSampledName(mi6->miHook, name, triage);
    unlockMutex(mutex);
  }
  return result;
}

/***********************************************************************/
/**
 * Do a quick read-only lookup of the sampled chunk name and return
 * information needed by the index code to process the chunk name.
 *
 * @param masterIndex The master index
 * @param name        The chunk name
 * @param triage      Information about the chunk name.  The zone and
 *                    isSample fields are already filled in.  Set
 *                    inSampledChapter and virtualChapter if the chunk
 *                    name is found in the index.
 *
 * @return UDS_SUCCESS or an error code
 **/
static int
lookupMasterIndexSampledName_006(const struct master_index *masterIndex
                                     __attribute__((unused)),
                                 const struct uds_chunk_name *name
                                     __attribute__((unused)),
                                  struct master_index_triage *triage
                                      __attribute__((unused)))
{
  return ASSERT_WITH_ERROR_CODE(false, UDS_BAD_STATE,
                                "%s should not be called", __func__);
}

/***********************************************************************/
/**
 * Find the master index record associated with a block name
 *
 * This is always the first routine to be called when dealing with a delta
 * master index entry.  The fields of the record parameter should be
 * examined to determine the state of the record:
 *
 * If isFound is false, then we did not find an entry for the block
 * name.  Information is saved in the master_index_record so that
 * putMasterIndexRecord() will insert an entry for that block name at
 * the proper place.
 *
 * If isFound is true, then we did find an entry for the block name.
 * Information is saved in the master_index_record so that the "chapter"
 * and "isCollision" fields reflect the entry found.
 * Calls to removeMasterIndexRecord() will remove the entry, calls to
 * setMasterIndexRecordChapter() can modify the entry, and calls to
 * putMasterIndexRecord() can insert a collision record with this
 * entry.
 *
 * @param masterIndex The master index to search
 * @param name        The chunk name
 * @param record      Set to the info about the record searched for
 *
 * @return UDS_SUCCESS or an error code
 **/
static int getMasterIndexRecord_006(struct master_index *masterIndex,
                                    const struct uds_chunk_name *name,
                                    struct master_index_record *record)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  int result;
  if (isMasterIndexSample_006(masterIndex, name)) {
    /*
     * We need to prevent a lookupMasterIndexName() happening while we are
     * finding the master index record.  Remember that because of lazy LRU
     * flushing of the master index, getMasterIndexRecord() is not a
     * read-only operation.
     */
    unsigned int zone = getMasterIndexZone(mi6->miHook, name);
    Mutex *mutex = &mi6->masterZones[zone].hookMutex;
    lockMutex(mutex);
    result = getMasterIndexRecord(mi6->miHook, name, record);
    unlockMutex(mutex);
    // Remember the mutex so that other operations on the master_index_record
    // can use it
    record->mutex = mutex;
  } else {
    result = getMasterIndexRecord(mi6->miNonHook, name, record);
  }
  return result;
}

/***********************************************************************/
/**
 * Get the number of bytes used for master index entries.
 *
 * @param masterIndex The master index
 *
 * @return The number of bytes in use
 **/
static size_t
getMasterIndexMemoryUsed_006(const struct master_index *masterIndex)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  return (getMasterIndexMemoryUsed(mi6->miNonHook)
          + getMasterIndexMemoryUsed(mi6->miHook));
}

/***********************************************************************/
/**
 * Return the master index stats.  There is only one portion of the master
 * index in this implementation, and we call it the dense portion of the
 * index.
 *
 * @param masterIndex The master index
 * @param dense       Stats for the dense portion of the index
 * @param sparse      Stats for the sparse portion of the index
 **/
static void getMasterIndexStats_006(const struct master_index *masterIndex,
                                    struct master_index_stats *dense,
                                    struct master_index_stats *sparse)
{
  const struct master_index6 *mi6 = const_container_of(masterIndex,
                                                       struct master_index6,
                                                       common);
  struct master_index_stats dummyStats;
  getMasterIndexStats(mi6->miNonHook, dense,  &dummyStats);
  getMasterIndexStats(mi6->miHook,    sparse, &dummyStats);
}

/***********************************************************************/
struct split_config {
  struct configuration hookConfig;     // Describe hook part of the index
  struct geometry      hookGeometry;
  struct configuration nonHookConfig;  // Describe non-hook part of the index
  struct geometry      nonHookGeometry;
};

/***********************************************************************/
static int splitConfiguration006(const struct configuration *config,
                                 struct split_config *split)
{
  int result
    = ASSERT_WITH_ERROR_CODE(config->geometry->sparse_chapters_per_volume != 0,
                             UDS_INVALID_ARGUMENT,
                             "cannot initialize sparse+dense master index"
                             " with no sparse chapters");
  if (result != UDS_SUCCESS) {
    return result;
  }
  result = ASSERT_WITH_ERROR_CODE(config->sparse_sample_rate != 0,
                                  UDS_INVALID_ARGUMENT,
                                  "cannot initialize sparse+dense master"
                                  " index with a sparse sample rate of %u",
                                  config->sparse_sample_rate);
  if (result != UDS_SUCCESS) {
    return result;
  }

  // Start with copies of the base configuration
  split->hookConfig = *config;
  split->hookGeometry = *config->geometry;
  split->hookConfig.geometry = &split->hookGeometry;
  split->nonHookConfig   = *config;
  split->nonHookGeometry = *config->geometry;
  split->nonHookConfig.geometry = &split->nonHookGeometry;

  uint64_t sampleRate        = config->sparse_sample_rate;
  uint64_t numChapters       = config->geometry->chapters_per_volume;
  uint64_t numSparseChapters = config->geometry->sparse_chapters_per_volume;
  uint64_t numDenseChapters  = numChapters - numSparseChapters;
  uint64_t sampleRecords = config->geometry->records_per_chapter / sampleRate;

  // Adjust the number of records indexed for each chapter
  split->hookGeometry.records_per_chapter     = sampleRecords;
  split->nonHookGeometry.records_per_chapter -= sampleRecords;

  // Adjust the number of chapters indexed
  split->hookGeometry.sparse_chapters_per_volume    = 0;
  split->nonHookGeometry.sparse_chapters_per_volume = 0;
  split->nonHookGeometry.chapters_per_volume        = numDenseChapters;
  return UDS_SUCCESS;
}

/***********************************************************************/
int computeMasterIndexSaveBytes006(const struct configuration *config,
                                   size_t *numBytes)
{
  struct split_config split;
  int result = splitConfiguration006(config, &split);
  if (result != UDS_SUCCESS) {
    return result;
  }
  size_t hookBytes, nonHookBytes;
  result = compute_master_index_save_bytes005(&split.hookConfig, &hookBytes);
  if (result != UDS_SUCCESS) {
    return result;
  }
  result
    = compute_master_index_save_bytes005(&split.nonHookConfig, &nonHookBytes);
  if (result != UDS_SUCCESS) {
    return result;
  }
  // Saving a MasterIndex006 needs a header plus the hook index plus the
  // non-hook index
  *numBytes = sizeof(struct mi006_data) + hookBytes + nonHookBytes;
  return UDS_SUCCESS;
}

/***********************************************************************/
int makeMasterIndex006(const struct configuration *config,
                       unsigned int numZones, uint64_t volumeNonce,
                       struct master_index **masterIndex)
{
  struct split_config split;
  int result = splitConfiguration006(config, &split);
  if (result != UDS_SUCCESS) {
    return result;
  }

  struct master_index6 *mi6;
  result = ALLOCATE(1, struct master_index6, "master index", &mi6);
  if (result != UDS_SUCCESS) {
    return result;
  }

  mi6->common.abortRestoringMasterIndex     = abortRestoringMasterIndex_006;
  mi6->common.abortSavingMasterIndex        = abortSavingMasterIndex_006;
  mi6->common.finishSavingMasterIndex       = finishSavingMasterIndex_006;
  mi6->common.freeMasterIndex               = freeMasterIndex_006;
  mi6->common.getMasterIndexMemoryUsed      = getMasterIndexMemoryUsed_006;
  mi6->common.getMasterIndexRecord          = getMasterIndexRecord_006;
  mi6->common.getMasterIndexStats           = getMasterIndexStats_006;
  mi6->common.getMasterIndexZone            = getMasterIndexZone_006;
  mi6->common.isMasterIndexSample           = isMasterIndexSample_006;
  mi6->common.isRestoringMasterIndexDone    = isRestoringMasterIndexDone_006;
  mi6->common.isSavingMasterIndexDone       = isSavingMasterIndexDone_006;
  mi6->common.lookupMasterIndexName         = lookupMasterIndexName_006;
  mi6->common.lookupMasterIndexSampledName  = lookupMasterIndexSampledName_006;
  mi6->common.restoreDeltaListToMasterIndex = restoreDeltaListToMasterIndex_006;
  mi6->common.setMasterIndexOpenChapter     = setMasterIndexOpenChapter_006;
  mi6->common.setMasterIndexTag             = setMasterIndexTag_006;
  mi6->common.setMasterIndexZoneOpenChapter = setMasterIndexZoneOpenChapter_006;
  mi6->common.startRestoringMasterIndex     = startRestoringMasterIndex_006;
  mi6->common.startSavingMasterIndex        = startSavingMasterIndex_006;

  mi6->numZones         = numZones;
  mi6->sparseSampleRate = config->sparse_sample_rate;

  result = ALLOCATE(numZones, struct master_index_zone, "master index zones",
                    &mi6->masterZones);
  unsigned int zone;
  for (zone = 0; zone < numZones; zone++) {
    if (result == UDS_SUCCESS) {
      result = initMutex(&mi6->masterZones[zone].hookMutex);
    }
  }
  if (result != UDS_SUCCESS) {
    freeMasterIndex_006(&mi6->common);
    return result;
  }

  result = make_master_index005(&split.nonHookConfig, numZones, volumeNonce,
                                &mi6->miNonHook);
  if (result != UDS_SUCCESS) {
    freeMasterIndex_006(&mi6->common);
    return logErrorWithStringError(result,
                                   "Error creating non hook master index");
  }
  setMasterIndexTag(mi6->miNonHook, 'd');

  result = make_master_index005(&split.hookConfig, numZones, volumeNonce,
                                &mi6->miHook);
  if (result != UDS_SUCCESS) {
    freeMasterIndex_006(&mi6->common);
    return logErrorWithStringError(result,
                                   "Error creating hook master index");
  }
  setMasterIndexTag(mi6->miHook, 's');

  *masterIndex = &mi6->common;
  return UDS_SUCCESS;
}
