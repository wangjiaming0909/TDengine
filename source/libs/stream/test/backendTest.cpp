#include <gtest/gtest.h>

#include <taoserror.h>
#include <tglobal.h>
#include <iostream>
#include <vector>
#include "streamBackendRocksdb.h"
#include "streamSnapshot.h"
#include "streamState.h"
#include "tstream.h"
#include "tstreamFileState.h"
#include "tstreamUpdate.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"

class BackendEnv : public ::testing::Test {
 protected:
  virtual void SetUp() {}
  virtual void TearDown() {}
};

void *backendCreate() {
  const char *streamPath = "/tmp";
  void       *p = NULL;

  // char *absPath = NULL;
  // // SBackendWrapper *p = (SBackendWrapper *)streamBackendInit(streamPath, -1, 2);
  // STaskDbWrapper *p = taskDbOpen((char *)streamPath, (char *)"stream-backend", -1);
  // ASSERT(p != NULL);
  return p;
}

SStreamState *stateCreate(const char *path) {
  SStreamTask *pTask = (SStreamTask *)taosMemoryCalloc(1, sizeof(SStreamTask));
  pTask->ver = 1024;
  pTask->id.streamId = 1023;
  pTask->id.taskId = 1111111;
  SStreamMeta *pMeta = streamMetaOpen((path), NULL, NULL, 0, 0, NULL);
  pTask->pMeta = pMeta;

  SStreamState *p = streamStateOpen((char *)path, pTask, true, 32, 32 * 1024);
  ASSERT(p != NULL);
  return p;
}
void *backendOpen() {
  streamMetaInit();
  const char   *path = "/tmp/backend";
  SStreamState *p = stateCreate(path);
  ASSERT(p != NULL);

  // write bacth
  // default/state/fill/sess/func/parname/partag
  int32_t              size = 100;
  std::vector<int64_t> tsArray;
  for (int i = 0; i < size; i++) {
    int64_t     ts = taosGetTimestampMs();
    SWinKey     key = {.groupId = (uint64_t)(i), .ts = ts};
    const char *val = "value data";
    int32_t     vlen = strlen(val);
    streamStatePut_rocksdb(p, &key, (char *)val, vlen);

    tsArray.push_back(ts);
  }
  for (int i = 0; i < size; i++) {
    int64_t ts = tsArray[i];
    SWinKey key = {.groupId = (uint64_t)(i), .ts = ts};

    const char *val = "value data";
    int32_t     len = 0;
    char       *newVal = NULL;
    streamStateGet_rocksdb(p, &key, (void **)&newVal, &len);
    ASSERT(len == strlen(val));
  }
  int64_t ts = tsArray[0];
  SWinKey key = {.groupId = (uint64_t)(0), .ts = ts};
  streamStateDel_rocksdb(p, &key);

  streamStateClear_rocksdb(p);

  for (int i = 0; i < size; i++) {
    int64_t     ts = tsArray[i];
    SWinKey     key = {.groupId = (uint64_t)(i), .ts = ts};
    const char *val = "value data";
    int32_t     len = 0;
    char       *newVal = NULL;
    int32_t     code = streamStateGet_rocksdb(p, &key, (void **)&newVal, &len);
    ASSERT(code != 0);
  }
  tsArray.clear();

  for (int i = 0; i < size; i++) {
    int64_t ts = taosGetTimestampMs();
    tsArray.push_back(ts);

    SWinKey     key = {.groupId = (uint64_t)(i), .ts = ts};
    const char *val = "value data";
    int32_t     vlen = strlen(val);
    streamStatePut_rocksdb(p, &key, (char *)val, vlen);
  }

  SWinKey winkey;
  int32_t code = streamStateGetFirst_rocksdb(p, &key);
  ASSERT(code == 0);
  ASSERT(key.ts == tsArray[0]);

  SStreamStateCur *pCurr = streamStateSeekToLast_rocksdb(p);
  ASSERT(pCurr != NULL);
  streamStateFreeCur(pCurr);

  winkey.groupId = 0;
  winkey.ts = tsArray[0];
  char   *val = NULL;
  int32_t len = 0;

  pCurr = streamStateSeekKeyNext_rocksdb(p, &winkey);
  ASSERT(pCurr != NULL);

  streamStateFreeCur(pCurr);

  tsArray.clear();
  for (int i = 0; i < size; i++) {
    int64_t ts = taosGetTimestampMs();
    tsArray.push_back(ts);
    STupleKey key = {.groupId = (uint64_t)(0), .ts = ts, .exprIdx = i};

    const char *val = "Value";
    int32_t     len = strlen(val);
    streamStateFuncPut_rocksdb(p, &key, val, len);
  }
  for (int i = 0; i < size; i++) {
    STupleKey key = {.groupId = (uint64_t)(0), .ts = tsArray[i], .exprIdx = i};
    char     *val = NULL;
    int32_t   len = 0;
    streamStateFuncGet_rocksdb(p, &key, (void **)&val, &len);
    ASSERT(len == strlen("Value"));
  }
  for (int i = 0; i < size; i++) {
    STupleKey key = {.groupId = (uint64_t)(0), .ts = tsArray[i], .exprIdx = i};
    char     *val = NULL;
    int32_t   len = 0;
    streamStateFuncDel_rocksdb(p, &key);
  }

  // session put
  tsArray.clear();

  for (int i = 0; i < size; i++) {
    SSessionKey key = {.win = {.skey = i, .ekey = i}, .groupId = (uint64_t)(0)};
    tsArray.push_back(i);

    const char *val = "Value";
    int32_t     len = strlen(val);
    streamStateSessionPut_rocksdb(p, &key, val, len);

    char *pval = NULL;
    ASSERT(0 == streamStateSessionGet_rocksdb(p, &key, (void **)&pval, &len));
    ASSERT(strncmp(pval, val, len) == 0);
  }

  for (int i = 0; i < size; i++) {
    SSessionKey key = {.win = {.skey = tsArray[i], .ekey = tsArray[i]}, .groupId = (uint64_t)(0)};

    const char *val = "Value";
    int32_t     len = strlen(val);

    char *pval = NULL;
    ASSERT(0 == streamStateSessionGet_rocksdb(p, &key, (void **)&pval, &len));
    ASSERT(strncmp(pval, val, len) == 0);
    taosMemoryFreeClear(pval);
  }

  pCurr = streamStateSessionSeekToLast_rocksdb(p, 0);
  ASSERT(pCurr != NULL);

  {
    SSessionKey key;
    char       *val = NULL;
    int32_t     vlen = 0;
    code = streamStateSessionGetKVByCur_rocksdb(pCurr, &key, (void **)&val, &vlen);
    ASSERT(code == 0);
    pCurr = streamStateSessionSeekKeyPrev_rocksdb(p, &key);

    code = streamStateSessionGetKVByCur_rocksdb(pCurr, &key, (void **)&val, &vlen);
    ASSERT(code == 0);

    ASSERT(key.groupId == 0 && key.win.ekey == tsArray[tsArray.size() - 2]);

    pCurr = streamStateSessionSeekKeyNext_rocksdb(p, &key);
    code = streamStateSessionGetKVByCur_rocksdb(pCurr, &key, (void **)&val, &vlen);
    ASSERT(code == 0);
    ASSERT(vlen == strlen("Value"));
    ASSERT(key.groupId == 0 && key.win.skey == tsArray[tsArray.size() - 1]);

    ASSERT(0 == streamStateSessionAddIfNotExist_rocksdb(p, &key, 10, (void **)&val, &len));

    ASSERT(0 ==
           streamStateStateAddIfNotExist_rocksdb(p, &key, (char *)"key", strlen("key"), NULL, (void **)&val, &len));
  }
  for (int i = 0; i < size; i++) {
    SSessionKey key = {.win = {.skey = tsArray[i], .ekey = tsArray[i]}, .groupId = (uint64_t)(0)};

    const char *val = "Value";
    int32_t     len = strlen(val);

    char *pval = NULL;
    ASSERT(0 == streamStateSessionDel_rocksdb(p, &key));
  }

  for (int i = 0; i < size; i++) {
    SWinKey     key = {.groupId = (uint64_t)(i), .ts = tsArray[i]};
    const char *val = "Value";
    int32_t     vlen = strlen(val);
    ASSERT(streamStateFillPut_rocksdb(p, &key, val, vlen) == 0);
  }
  for (int i = 0; i < size; i++) {
    SWinKey key = {.groupId = (uint64_t)(i), .ts = tsArray[i]};
    char   *val = NULL;
    int32_t vlen = 0;
    ASSERT(streamStateFillGet_rocksdb(p, &key, (void **)&val, &vlen) == 0);
    taosMemoryFreeClear(val);
  }
  {
    SWinKey          key = {.groupId = (uint64_t)(0), .ts = tsArray[0]};
    SStreamStateCur *pCurr = streamStateFillGetCur_rocksdb(p, &key);
    ASSERT(pCurr != NULL);

    char   *val = NULL;
    int32_t vlen = 0;
    ASSERT(0 == streamStateFillGetKVByCur_rocksdb(pCurr, &key, (const void **)&val, &vlen));
    ASSERT(vlen == strlen("Value"));
    streamStateFreeCur(pCurr);

    pCurr = streamStateFillSeekKeyNext_rocksdb(p, &key);
    ASSERT(0 == streamStateFillGetKVByCur_rocksdb(pCurr, &key, (const void **)&val, &vlen));
    ASSERT(vlen == strlen("Value") && key.groupId == 1 && key.ts == tsArray[1]);

    key.groupId = 1;
    key.ts = tsArray[1];

    pCurr = streamStateFillSeekKeyPrev_rocksdb(p, &key);
    ASSERT(pCurr != NULL);
    ASSERT(0 == streamStateFillGetKVByCur_rocksdb(pCurr, &key, (const void **)&val, &vlen));

    ASSERT(vlen == strlen("Value") && key.groupId == 0 && key.ts == tsArray[0]);
  }

  for (int i = 0; i < size - 1; i++) {
    SWinKey key = {.groupId = (uint64_t)(i), .ts = tsArray[i]};
    char   *val = NULL;
    int32_t vlen = 0;
    ASSERT(streamStateFillDel_rocksdb(p, &key) == 0);
    taosMemoryFreeClear(val);
  }
  streamStateSessionClear_rocksdb(p);

  for (int i = 0; i < size; i++) {
    char tbname[TSDB_TABLE_NAME_LEN] = {0};
    sprintf(tbname, "%s_%d", "tbname", i);
    ASSERT(0 == streamStatePutParName_rocksdb(p, i, tbname));
  }
  for (int i = 0; i < size; i++) {
    char *val = NULL;
    ASSERT(0 == streamStateGetParName_rocksdb(p, i, (void **)&val));
    ASSERT(strncmp(val, "tbname", strlen("tbname")) == 0);
    taosMemoryFree(val);
  }

  for (int i = 0; i < size; i++) {
    char tbname[TSDB_TABLE_NAME_LEN] = {0};
    sprintf(tbname, "%s_%d", "tbname", i);
    ASSERT(0 == streamStatePutParName_rocksdb(p, i, tbname));
  }
  for (int i = 0; i < size; i++) {
    char *val = NULL;
    ASSERT(0 == streamStateGetParName_rocksdb(p, i, (void **)&val));
    ASSERT(strncmp(val, "tbname", strlen("tbname")) == 0);
    taosMemoryFree(val);
  }
  for (int i = 0; i < size; i++) {
    char key[128] = {0};
    sprintf(key, "tbname_%d", i);
    char val[128] = {0};
    sprintf(val, "val_%d", i);
    code = streamDefaultPut_rocksdb(p, key, val, strlen(val));
    ASSERT(code == 0);
  }
  for (int i = 0; i < size; i++) {
    char key[128] = {0};
    sprintf(key, "tbname_%d", i);

    char   *val = NULL;
    int32_t len = 0;
    code = streamDefaultGet_rocksdb(p, key, (void **)&val, &len);
    ASSERT(code == 0);
  }
  SArray *result = taosArrayInit(8, sizeof(void *));
  streamDefaultIterGet_rocksdb(p, "tbname", "tbname_99", result);
  ASSERT(taosArrayGetSize(result) >= 0);

  return p;
  // streamStateClose((SStreamState *)p, true);
}
TEST_F(BackendEnv, checkOpen) {
  SStreamState *p = (SStreamState *)backendOpen();
  int64_t       tsStart = taosGetTimestampMs();
  {
    void   *pBatch = streamStateCreateBatch();
    int32_t size = 0;
    for (int i = 0; i < size; i++) {
      char key[128] = {0};
      sprintf(key, "key_%d", i);
      char val[128] = {0};
      sprintf(val, "val_%d", i);
      streamStatePutBatch(p, "default", (rocksdb_writebatch_t *)pBatch, (void *)key, (void *)val,
                          (int32_t)(strlen(val)), tsStart + 100000);
    }
    streamStatePutBatch_rocksdb(p, pBatch);
    streamStateDestroyBatch(pBatch);
  }
  {
    void   *pBatch = streamStateCreateBatch();
    int32_t size = 0;
    char    valBuf[256] = {0};
    for (int i = 0; i < size; i++) {
      char key[128] = {0};
      sprintf(key, "key_%d", i);
      char val[128] = {0};
      sprintf(val, "val_%d", i);
      streamStatePutBatchOptimize(p, 0, (rocksdb_writebatch_t *)pBatch, (void *)key, (void *)val,
                                  (int32_t)(strlen(val)), tsStart + 100000, (void *)valBuf);
    }
    streamStatePutBatch_rocksdb(p, pBatch);
    streamStateDestroyBatch(pBatch);
  }
  // do checkpoint 2
  taskDbDoCheckpoint(p->pTdbState->pOwner->pBackend, 2);
  {
    void   *pBatch = streamStateCreateBatch();
    int32_t size = 0;
    char    valBuf[256] = {0};
    for (int i = 0; i < size; i++) {
      char key[128] = {0};
      sprintf(key, "key_%d", i);
      char val[128] = {0};
      sprintf(val, "val_%d", i);
      streamStatePutBatchOptimize(p, 0, (rocksdb_writebatch_t *)pBatch, (void *)key, (void *)val,
                                  (int32_t)(strlen(val)), tsStart + 100000, (void *)valBuf);
    }
    streamStatePutBatch_rocksdb(p, pBatch);
    streamStateDestroyBatch(pBatch);
  }

  taskDbDoCheckpoint(p->pTdbState->pOwner->pBackend, 3);

  const char *path = "/tmp/backend/stream";
  const char *dump = "/tmp/backend/stream/dump";
  // taosMkDir(dump);
  taosMulMkDir(dump);
  SBkdMgt *mgt = bkdMgtCreate((char *)path);
  SArray  *result = taosArrayInit(4, sizeof(void *));
  bkdMgtGetDelta(mgt, p->pTdbState->idstr, 3, result, (char *)dump);

  bkdMgtDestroy(mgt);
  streamStateClose((SStreamState *)p, true);
  taosRemoveDir(path);
}

TEST_F(BackendEnv, backendChkp) { const char *path = "/tmp"; }

typedef struct BdKV {
  uint32_t k;
  uint32_t v;
} BdKV;

BdKV kvDict[] = {{0, 2},
                 {1, 2},
                 {.k = 15, .v = 16},
                 {.k = 31, .v = 32},
                 {.k = 56, .v = 64},
                 {.k = 100, .v = 128},
                 {.k = 200, .v = 256},
                 {.k = 500, .v = 512},
                 {.k = 1000, .v = 1024},
                 {.k = 2000, .v = 2048},
                 {.k = 3000, .v = 4096}};

TEST_F(BackendEnv, backendUtil) {
  for (int i = 0; i < sizeof(kvDict) / sizeof(kvDict[0]); i++) {
    ASSERT_EQ(nextPow2((uint32_t)(kvDict[i].k)), kvDict[i].v);
  }
}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}