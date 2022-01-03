/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
p *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

//#include <sys/types.h>
//#include <dirent.h>
#include "index_tfile.h"
#include "index.h"
#include "index_fst.h"
#include "index_fst_counting_writer.h"
#include "index_util.h"
#include "taosdef.h"
#include "tcompare.h"

typedef struct TFileFstIter {
  FstStreamBuilder* fb;
  StreamWithState*  st;
  AutomationCtx*    ctx;
  TFileReader*      rdr;
} TFileFstIter;

#define TF_TABLE_TATOAL_SIZE(sz) (sizeof(sz) + sz * sizeof(uint64_t))

static int  tfileUidCompare(const void* a, const void* b);
static int  tfileStrCompare(const void* a, const void* b);
static int  tfileValueCompare(const void* a, const void* b, const void* param);
static void tfileSerialTableIdsToBuf(char* buf, SArray* tableIds);

static int tfileWriteHeader(TFileWriter* writer);
static int tfileWriteFstOffset(TFileWriter* tw, int32_t offset);
static int tfileWriteData(TFileWriter* write, TFileValue* tval);

static int tfileReaderLoadHeader(TFileReader* reader);
static int tfileReaderLoadFst(TFileReader* reader);
static int tfileReaderLoadTableIds(TFileReader* reader, int32_t offset, SArray* result);

static SArray* tfileGetFileList(const char* path);
static int     tfileRmExpireFile(SArray* result);
static void    tfileDestroyFileName(void* elem);
static int     tfileCompare(const void* a, const void* b);
static int     tfileParseFileName(const char* filename, uint64_t* suid, char* col, int* version);
static void    tfileGenFileName(char* filename, uint64_t suid, const char* col, int version);
static void    tfileGenFileFullName(char* fullname, const char* path, uint64_t suid, const char* col, int32_t version);

TFileCache* tfileCacheCreate(const char* path) {
  TFileCache* tcache = calloc(1, sizeof(TFileCache));
  if (tcache == NULL) { return NULL; }

  tcache->tableCache = taosHashInit(8, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BINARY), true, HASH_ENTRY_LOCK);
  tcache->capacity = 64;

  SArray* files = tfileGetFileList(path);

  uint64_t suid;
  int32_t  colId, version;
  for (size_t i = 0; i < taosArrayGetSize(files); i++) {
    char* file = taosArrayGetP(files, i);
    char  colName[256] = {0};
    if (0 != tfileParseFileName(file, &suid, colName, (int*)&version)) {
      indexInfo("try parse invalid file:  %s, skip it", file);
      continue;
    }
    // use version info later
    char fullName[256] = {0};
    sprintf(fullName, "%s/%s", path, file);

    WriterCtx* wc = writerCtxCreate(TFile, fullName, true, 1024 * 1024 * 64);
    if (wc == NULL) {
      indexError("failed to open index:%s", file);
      goto End;
    }

    char         buf[128] = {0};
    TFileReader* reader = tfileReaderCreate(wc);
    TFileHeader* header = &reader->header;
    ICacheKey    key = {.suid = header->suid,
                     .colName = header->colName,
                     .nColName = strlen(header->colName),
                     .colType = header->colType};

    int32_t sz = indexSerialCacheKey(&key, buf);
    assert(sz < sizeof(buf));
    taosHashPut(tcache->tableCache, buf, sz, &reader, sizeof(void*));
    tfileReaderRef(reader);
  }
  taosArrayDestroyEx(files, tfileDestroyFileName);
  return tcache;
End:
  tfileCacheDestroy(tcache);
  taosArrayDestroyEx(files, tfileDestroyFileName);
  return NULL;
}
void tfileCacheDestroy(TFileCache* tcache) {
  if (tcache == NULL) { return; }

  // free table cache
  TFileReader** reader = taosHashIterate(tcache->tableCache, NULL);
  while (reader) {
    TFileReader* p = *reader;
    indexInfo("drop table cache suid: %" PRIu64 ", colName: %s, colType: %d", p->header.suid, p->header.colName,
              p->header.colType);

    tfileReaderUnRef(p);
    reader = taosHashIterate(tcache->tableCache, reader);
  }
  taosHashCleanup(tcache->tableCache);
  free(tcache);
}

TFileReader* tfileCacheGet(TFileCache* tcache, ICacheKey* key) {
  char    buf[128] = {0};
  int32_t sz = indexSerialCacheKey(key, buf);
  assert(sz < sizeof(buf));
  TFileReader** reader = taosHashGet(tcache->tableCache, buf, sz);
  if (reader == NULL) { return NULL; }
  tfileReaderRef(*reader);

  return *reader;
}
void tfileCachePut(TFileCache* tcache, ICacheKey* key, TFileReader* reader) {
  char    buf[128] = {0};
  int32_t sz = indexSerialCacheKey(key, buf);
  // remove last version index reader
  TFileReader** p = taosHashGet(tcache->tableCache, buf, sz);
  if (p != NULL) {
    TFileReader* oldReader = *p;
    taosHashRemove(tcache->tableCache, buf, sz);
    oldReader->remove = true;
    tfileReaderUnRef(oldReader);
  }

  taosHashPut(tcache->tableCache, buf, sz, &reader, sizeof(void*));
  tfileReaderRef(reader);
  return;
}
TFileReader* tfileReaderCreate(WriterCtx* ctx) {
  TFileReader* reader = calloc(1, sizeof(TFileReader));
  if (reader == NULL) { return NULL; }

  // T_REF_INC(reader);
  reader->ctx = ctx;
  if (0 != tfileReaderLoadHeader(reader)) {
    tfileReaderDestroy(reader);
    indexError("failed to load index header, suid: %" PRIu64 ", colName: %s", reader->header.suid,
               reader->header.colName);
    return NULL;
  }

  if (0 != tfileReaderLoadFst(reader)) {
    tfileReaderDestroy(reader);
    indexError("failed to load index fst, suid: %" PRIu64 ", colName: %s", reader->header.suid, reader->header.colName);
    return NULL;
  }

  return reader;
}
void tfileReaderDestroy(TFileReader* reader) {
  if (reader == NULL) { return; }
  // T_REF_INC(reader);
  fstDestroy(reader->fst);
  writerCtxDestroy(reader->ctx, reader->remove);
  free(reader);
}

int tfileReaderSearch(TFileReader* reader, SIndexTermQuery* query, SArray* result) {
  SIndexTerm*     term = query->term;
  EIndexQueryType qtype = query->qType;

  int ret = -1;
  // refactor to callback later
  if (qtype == QUERY_TERM) {
    uint64_t offset;
    FstSlice key = fstSliceCreate(term->colVal, term->nColVal);
    if (fstGet(reader->fst, &key, &offset)) {
      indexInfo("index: %" PRIu64 ", col: %s, colVal: %s, found table info in tindex", term->suid, term->colName,
                term->colVal);
      ret = tfileReaderLoadTableIds(reader, offset, result);
    } else {
      indexInfo("index: %" PRIu64 ", col: %s, colVal: %s, not found table info in tindex", term->suid, term->colName,
                term->colVal);
    }
    fstSliceDestroy(&key);
  } else if (qtype == QUERY_PREFIX) {
    // handle later
    //
  } else {
    // handle later
  }
  tfileReaderUnRef(reader);
  return ret;
}

TFileWriter* tfileWriterOpen(char* path, uint64_t suid, int32_t version, const char* colName, uint8_t colType) {
  char fullname[256] = {0};
  tfileGenFileFullName(fullname, path, suid, colName, version);
  indexInfo("open write file name %s", fullname);
  WriterCtx* wcx = writerCtxCreate(TFile, fullname, false, 1024 * 1024 * 64);
  if (wcx == NULL) { return NULL; }

  TFileHeader tfh = {0};
  tfh.suid = suid;
  tfh.version = version;
  memcpy(tfh.colName, colName, strlen(colName));
  tfh.colType = colType;

  return tfileWriterCreate(wcx, &tfh);
}
TFileReader* tfileReaderOpen(char* path, uint64_t suid, int32_t version, const char* colName) {
  char fullname[256] = {0};
  tfileGenFileFullName(fullname, path, suid, colName, version);

  WriterCtx* wc = writerCtxCreate(TFile, fullname, true, 1024 * 1024 * 1024);
  indexInfo("open read file name:%s, size: %d", wc->file.buf, wc->file.size);
  if (wc == NULL) { return NULL; }

  TFileReader* reader = tfileReaderCreate(wc);
  return reader;
}
TFileWriter* tfileWriterCreate(WriterCtx* ctx, TFileHeader* header) {
  // char pathBuf[128] = {0};
  // sprintf(pathBuf, "%s/% " PRIu64 "-%d-%d.tindex", path, suid, colId, version);
  // TFileHeader header = {.suid = suid, .version = version, .colName = {0}, colType = colType};
  // memcpy(header.colName, );

  // char buf[TFILE_HADER_PRE_SIZE];
  // int  len = TFILE_HADER_PRE_SIZE;
  // if (len != ctx->write(ctx, buf, len)) {
  //  indexError("index: %" PRIu64 " failed to write header info", header->suid);
  //  return NULL;
  //}
  TFileWriter* tw = calloc(1, sizeof(TFileWriter));
  if (tw == NULL) {
    indexError("index: %" PRIu64 " failed to alloc TFilerWriter", header->suid);
    return NULL;
  }
  tw->ctx = ctx;
  tw->header = *header;
  tfileWriteHeader(tw);
  return tw;
}

int tfileWriterPut(TFileWriter* tw, void* data, bool order) {
  // sort by coltype and write to tindex
  if (order == false) {
    __compar_fn_t fn;
    int8_t        colType = tw->header.colType;
    if (colType == TSDB_DATA_TYPE_BINARY || colType == TSDB_DATA_TYPE_NCHAR) {
      fn = tfileStrCompare;
    } else {
      fn = getComparFunc(colType, 0);
    }
    taosArraySortPWithExt((SArray*)(data), tfileValueCompare, &fn);
  }

  int32_t bufLimit = 64 * 4096, offset = 0;
  // char*   buf = calloc(1, sizeof(char) * bufLimit);
  // char*   p = buf;
  int32_t sz = taosArrayGetSize((SArray*)data);
  int32_t fstOffset = tw->offset;

  // ugly code, refactor later
  for (size_t i = 0; i < sz; i++) {
    TFileValue* v = taosArrayGetP((SArray*)data, i);
    // taosArrayRemoveDuplicate(v->tablId, tfileUidCompare, NULL);
    int32_t tbsz = taosArrayGetSize(v->tableId);
    fstOffset += TF_TABLE_TATOAL_SIZE(tbsz);
  }
  tfileWriteFstOffset(tw, fstOffset);

  for (size_t i = 0; i < sz; i++) {
    TFileValue* v = taosArrayGetP((SArray*)data, i);

    int32_t tbsz = taosArrayGetSize(v->tableId);
    // check buf has enough space or not
    int32_t ttsz = TF_TABLE_TATOAL_SIZE(tbsz);

    // if (offset + ttsz >= bufLimit) {
    //  // batch write
    //  indexInfo("offset: %d, ttsz: %d", offset, ttsz);
    //  // std::cout << "offset: " << offset << std::endl;
    //  // std::cout << "ttsz:" << ttsz < < < std::endl;
    //  tw->ctx->write(tw->ctx, buf, offset);
    //  offset = 0;
    //  memset(buf, 0, bufLimit);
    //  p = buf;
    //}
    // if (ttsz >= bufLimit) {
    //}
    char* buf = calloc(1, ttsz * sizeof(char));
    char* p = buf;
    tfileSerialTableIdsToBuf(p, v->tableId);
    tw->ctx->write(tw->ctx, buf, ttsz);
    // offset += ttsz;
    // p = buf + offset;
    // set up value offset
    v->offset = tw->offset;
    tw->offset += ttsz;
    free(buf);
  }
  // if (offset != 0) {
  // write reversed data in buf to tindex
  // tw->ctx->write(tw->ctx, buf, offset);
  //}
  // tfree(buf);

  tw->fb = fstBuilderCreate(tw->ctx, 0);
  if (tw->fb == NULL) {
    tfileWriterClose(tw);
    return -1;
  }

  // write data
  for (size_t i = 0; i < sz; i++) {
    // TODO, fst batch write later
    TFileValue* v = taosArrayGetP((SArray*)data, i);
    if (tfileWriteData(tw, v) != 0) {
      indexError("failed to write data: %s, offset: %d len: %d", v->colVal, v->offset,
                 (int)taosArrayGetSize(v->tableId));
    } else {
      // indexInfo("success to write data: %s, offset: %d len: %d", v->colVal, v->offset,
      //          (int)taosArrayGetSize(v->tableId));
    }
  }
  fstBuilderFinish(tw->fb);
  fstBuilderDestroy(tw->fb);
  tw->fb = NULL;
  return 0;
}
void tfileWriterClose(TFileWriter* tw) {
  if (tw == NULL) { return; }
  writerCtxDestroy(tw->ctx, false);
  free(tw);
}
void tfileWriterDestroy(TFileWriter* tw) {
  if (tw == NULL) { return; }
  writerCtxDestroy(tw->ctx, false);
  free(tw);
}

IndexTFile* indexTFileCreate(const char* path) {
  IndexTFile* tfile = calloc(1, sizeof(IndexTFile));
  if (tfile == NULL) { return NULL; }

  tfile->cache = tfileCacheCreate(path);
  return tfile;
}
void indexTFileDestroy(IndexTFile* tfile) {
  tfileCacheDestroy(tfile->cache);
  free(tfile);
}

int indexTFileSearch(void* tfile, SIndexTermQuery* query, SArray* result) {
  int ret = -1;
  if (tfile == NULL) { return ret; }
  IndexTFile* pTfile = (IndexTFile*)tfile;

  SIndexTerm* term = query->term;
  ICacheKey key = {.suid = term->suid, .colType = term->colType, .colName = term->colName, .nColName = term->nColName};
  TFileReader* reader = tfileCacheGet(pTfile->cache, &key);
  if (reader == NULL) { return 0; }

  return tfileReaderSearch(reader, query, result);
}
int indexTFilePut(void* tfile, SIndexTerm* term, uint64_t uid) {
  // TFileWriterOpt wOpt = {.suid = term->suid, .colType = term->colType, .colName = term->colName, .nColName =
  // term->nColName, .version = 1};

  return 0;
}
static bool tfileIteratorNext(Iterate* iiter) {
  IterateValue* iv = &iiter->val;
  if (iv->colVal != NULL && iv->val != NULL) {
    // indexError("value in fst: colVal: %s, size: %d", iv->colVal, (int)taosArrayGetSize(iv->val));
  }
  iterateValueDestroy(iv, false);

  char*    colVal = NULL;
  uint64_t offset = 0;

  TFileFstIter*          tIter = iiter->iter;
  StreamWithStateResult* rt = streamWithStateNextWith(tIter->st, NULL);
  if (rt == NULL) { return false; }

  int32_t sz = 0;
  char*   ch = (char*)fstSliceData(&rt->data, &sz);
  colVal = calloc(1, sz + 1);
  memcpy(colVal, ch, sz);

  offset = (uint64_t)(rt->out.out);
  swsResultDestroy(rt);
  // set up iterate value
  if (tfileReaderLoadTableIds(tIter->rdr, offset, iv->val) != 0) { return false; }

  iv->colVal = colVal;
  return true;
  // std::string key(ch, sz);
}

static IterateValue* tifileIterateGetValue(Iterate* iter) { return &iter->val; }

static TFileFstIter* tfileFstIteratorCreate(TFileReader* reader) {
  TFileFstIter* tIter = calloc(1, sizeof(TFileFstIter));
  if (tIter == NULL) { return NULL; }

  tIter->ctx = automCtxCreate(NULL, AUTOMATION_ALWAYS);
  tIter->fb = fstSearch(reader->fst, tIter->ctx);
  tIter->st = streamBuilderIntoStream(tIter->fb);
  tIter->rdr = reader;
  return tIter;
}

Iterate* tfileIteratorCreate(TFileReader* reader) {
  if (reader == NULL) { return NULL; }

  Iterate* iter = calloc(1, sizeof(Iterate));
  iter->iter = tfileFstIteratorCreate(reader);
  if (iter->iter == NULL) {
    free(iter);
    return NULL;
  }
  iter->next = tfileIteratorNext;
  iter->getValue = tifileIterateGetValue;
  iter->val.val = taosArrayInit(1, sizeof(uint64_t));
  iter->val.colVal = NULL;
  return iter;
}
void tfileIteratorDestroy(Iterate* iter) {
  if (iter == NULL) { return; }

  IterateValue* iv = &iter->val;
  iterateValueDestroy(iv, true);

  TFileFstIter* tIter = iter->iter;
  streamWithStateDestroy(tIter->st);
  fstStreamBuilderDestroy(tIter->fb);
  automCtxDestroy(tIter->ctx);
  free(tIter);

  free(iter);
}

TFileReader* tfileGetReaderByCol(IndexTFile* tf, uint64_t suid, char* colName) {
  if (tf == NULL) { return NULL; }
  ICacheKey key = {.suid = suid, .colType = TSDB_DATA_TYPE_BINARY, .colName = colName, .nColName = strlen(colName)};
  return tfileCacheGet(tf->cache, &key);
}

static int tfileUidCompare(const void* a, const void* b) {
  uint64_t l = *(uint64_t*)a;
  uint64_t r = *(uint64_t*)b;
  return l - r;
}
static int tfileStrCompare(const void* a, const void* b) {
  int ret = strcmp((char*)a, (char*)b);
  if (ret == 0) { return ret; }
  return ret < 0 ? -1 : 1;
}

static int tfileValueCompare(const void* a, const void* b, const void* param) {
  __compar_fn_t fn = *(__compar_fn_t*)param;

  TFileValue* av = (TFileValue*)a;
  TFileValue* bv = (TFileValue*)b;

  return fn(av->colVal, bv->colVal);
}

TFileValue* tfileValueCreate(char* val) {
  TFileValue* tf = calloc(1, sizeof(TFileValue));
  if (tf == NULL) { return NULL; }
  tf->colVal = tstrdup(val);
  tf->tableId = taosArrayInit(32, sizeof(uint64_t));
  return tf;
}
int tfileValuePush(TFileValue* tf, uint64_t val) {
  if (tf == NULL) { return -1; }
  taosArrayPush(tf->tableId, &val);
  return 0;
}
void tfileValueDestroy(TFileValue* tf) {
  taosArrayDestroy(tf->tableId);
  free(tf->colVal);
  free(tf);
}
static void tfileSerialTableIdsToBuf(char* buf, SArray* ids) {
  int sz = taosArrayGetSize(ids);
  SERIALIZE_VAR_TO_BUF(buf, sz, int32_t);
  for (size_t i = 0; i < sz; i++) {
    uint64_t* v = taosArrayGet(ids, i);
    SERIALIZE_VAR_TO_BUF(buf, *v, uint64_t);
  }
}

static int tfileWriteFstOffset(TFileWriter* tw, int32_t offset) {
  int32_t fstOffset = offset + sizeof(tw->header.fstOffset);
  tw->header.fstOffset = fstOffset;
  if (sizeof(fstOffset) != tw->ctx->write(tw->ctx, (char*)&fstOffset, sizeof(fstOffset))) { return -1; }
  tw->offset += sizeof(fstOffset);
  return 0;
}
static int tfileWriteHeader(TFileWriter* writer) {
  char buf[TFILE_HEADER_NO_FST] = {0};

  TFileHeader* header = &writer->header;
  memcpy(buf, (char*)header, sizeof(buf));

  int nwrite = writer->ctx->write(writer->ctx, buf, sizeof(buf));
  if (sizeof(buf) != nwrite) { return -1; }
  writer->offset = nwrite;
  return 0;
}
static int tfileWriteData(TFileWriter* write, TFileValue* tval) {
  TFileHeader* header = &write->header;
  uint8_t      colType = header->colType;
  if (colType == TSDB_DATA_TYPE_BINARY || colType == TSDB_DATA_TYPE_NCHAR) {
    FstSlice key = fstSliceCreate((uint8_t*)(tval->colVal), (size_t)strlen(tval->colVal));
    if (fstBuilderInsert(write->fb, key, tval->offset)) {
      fstSliceDestroy(&key);
      return 0;
    }
    fstSliceDestroy(&key);
    return -1;
  } else {
    // handle other type later
  }
  return 0;
}
static int tfileReaderLoadHeader(TFileReader* reader) {
  // TODO simple tfile header later
  char buf[TFILE_HEADER_SIZE] = {0};

  int64_t nread = reader->ctx->readFrom(reader->ctx, buf, sizeof(buf), 0);
  if (nread == -1) {
    //
    indexError("actual Read: %d, to read: %d, errno: %d, filefd: %d, filename: %s", (int)(nread), (int)sizeof(buf),
               errno, reader->ctx->file.fd, reader->ctx->file.buf);
  } else {
    indexError("actual Read: %d, to read: %d, errno: %d, filefd: %d, filename: %s", (int)(nread), (int)sizeof(buf),
               errno, reader->ctx->file.fd, reader->ctx->file.buf);
  }
  // assert(nread == sizeof(buf));
  memcpy(&reader->header, buf, sizeof(buf));

  return 0;
}
static int tfileReaderLoadFst(TFileReader* reader) {
  // current load fst into memory, refactor it later
  static int FST_MAX_SIZE = 64 * 1024 * 1024;

  char* buf = calloc(1, sizeof(char) * FST_MAX_SIZE);
  if (buf == NULL) { return -1; }

  WriterCtx* ctx = reader->ctx;
  int32_t    nread = ctx->readFrom(ctx, buf, FST_MAX_SIZE, reader->header.fstOffset);
  indexError("nread = %d, and fst offset=%d, filename: %s ", nread, reader->header.fstOffset, ctx->file.buf);
  // we assuse fst size less than FST_MAX_SIZE
  assert(nread > 0 && nread < FST_MAX_SIZE);

  FstSlice st = fstSliceCreate((uint8_t*)buf, nread);
  reader->fst = fstCreate(&st);
  free(buf);
  fstSliceDestroy(&st);

  return reader->fst != NULL ? 0 : -1;
}
static int tfileReaderLoadTableIds(TFileReader* reader, int32_t offset, SArray* result) {
  int32_t    nid;
  WriterCtx* ctx = reader->ctx;

  int32_t nread = ctx->readFrom(ctx, (char*)&nid, sizeof(nid), offset);
  assert(sizeof(nid) == nread);

  int32_t total = sizeof(uint64_t) * nid;
  char*   buf = calloc(1, total);
  if (buf == NULL) { return -1; }

  nread = ctx->readFrom(ctx, buf, total, offset + sizeof(nid));
  assert(total == nread);

  for (int32_t i = 0; i < nid; i++) { taosArrayPush(result, (uint64_t*)buf + i); }
  free(buf);
  return 0;
}
void tfileReaderRef(TFileReader* reader) {
  if (reader == NULL) { return; }
  int ref = T_REF_INC(reader);
  UNUSED(ref);
}

void tfileReaderUnRef(TFileReader* reader) {
  if (reader == NULL) { return; }
  int ref = T_REF_DEC(reader);
  if (ref == 0) {
    // do nothing
    tfileReaderDestroy(reader);
  }
}

static SArray* tfileGetFileList(const char* path) {
  SArray* files = taosArrayInit(4, sizeof(void*));

  DIR* dir = opendir(path);
  if (NULL == dir) { return NULL; }

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_type && DT_DIR) { continue; }
    size_t len = strlen(entry->d_name);
    char*  buf = calloc(1, len + 1);
    memcpy(buf, entry->d_name, len);
    taosArrayPush(files, &buf);
  }
  closedir(dir);

  taosArraySort(files, tfileCompare);
  tfileRmExpireFile(files);

  return files;
}
static int tfileRmExpireFile(SArray* result) {
  // TODO(yihao): remove expire tindex after restart
  return 0;
}
static void tfileDestroyFileName(void* elem) {
  char* p = *(char**)elem;
  free(p);
}
static int tfileCompare(const void* a, const void* b) {
  const char* aName = *(char**)a;
  const char* bName = *(char**)b;

  size_t aLen = strlen(aName);
  size_t bLen = strlen(bName);

  int ret = strncmp(aName, bName, aLen > bLen ? aLen : bLen);
  if (ret == 0) { return ret; }
  return ret < 0 ? -1 : 1;
}

static int tfileParseFileName(const char* filename, uint64_t* suid, char* col, int* version) {
  if (3 == sscanf(filename, "%" PRIu64 "-%[^-]-%d.tindex", suid, col, version)) {
    // read suid & colid & version  success
    return 0;
  }
  return -1;
}
// tfile name suid-colId-version.tindex
static void tfileGenFileName(char* filename, uint64_t suid, const char* col, int version) {
  sprintf(filename, "%" PRIu64 "-%s-%d.tindex", suid, col, version);
  return;
}
static void tfileGenFileFullName(char* fullname, const char* path, uint64_t suid, const char* col, int32_t version) {
  char filename[128] = {0};
  tfileGenFileName(filename, suid, col, version);
  sprintf(fullname, "%s/%s", path, filename);
}
