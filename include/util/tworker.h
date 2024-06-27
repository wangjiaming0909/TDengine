/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
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

#ifndef _TD_UTIL_WORKER_H_
#define _TD_UTIL_WORKER_H_

#include "tlist.h"
#include "tqueue.h"
#include "tarray.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SWWorkerPool SWWorkerPool;

typedef enum SWorkerPoolType {
  QWORKER_POOL = 0,
  QUERY_AUTO_QWORKER_POOL,
  WORKER_POOL_TYPE_MAX,
} SWorkerPoolType;

struct SQWorkerPoolBase;
typedef struct SQWorkerPoolOper {
  SWorkerPoolType type;
  struct SQWorkerPoolBase *(*poolCreate)();
  int32_t (*poolInit)(struct SQWorkerPoolBase *pool);
  void (*poolCleanup)(struct SQWorkerPoolBase *pool);
  void (*poolDestroy)(void *pool);
  STaosQueue *(*poolAllocQueue)(struct SQWorkerPoolBase *pool, void *ahandle, FItem fp);
  void (*poolFreeQueue)(struct SQWorkerPoolBase *pool, STaosQueue *queue);
} SQWorkerPoolOper;

typedef struct SQWorkerPoolBase {
  SQWorkerPoolOper *oper;
  const char       *name;
  int32_t           min;
  int32_t           max;
} SQWorkerPoolBase;

typedef struct SQueueWorker {
  int32_t  id;      // worker id
  int64_t  pid;     // thread pid
  TdThread thread;  // thread id
  void    *pool;
} SQueueWorker;

typedef struct SQWorkerPool {
  SQWorkerPoolBase base;
  int32_t          num;  // current number of workers
  STaosQset       *qset;
  SQueueWorker    *workers;
  TdThreadMutex    mutex;
} SQWorkerPool;

typedef struct SAutoQWorkerPool {
  float         ratio;
  STaosQset    *qset;
  const char   *name;
  SArray       *workers;
  TdThreadMutex mutex;
} SAutoQWorkerPool;

typedef struct SWWorker {
  int32_t       id;      // worker id
  int64_t       pid;     // thread pid
  TdThread      thread;  // thread id
  STaosQall    *qall;
  STaosQset    *qset;
  SWWorkerPool *pool;
} SWWorker;

struct SWWorkerPool {
  int32_t       max;  // max number of workers
  int32_t       num;
  int32_t       nextId;  // from 0 to max-1, cyclic
  const char   *name;
  SWWorker     *workers;
  TdThreadMutex mutex;
};

SQWorkerPoolBase *tQWorkerPoolCreate();
int32_t           tQWorkerInit(SQWorkerPoolBase *pool);
void              tQWorkerCleanup(SQWorkerPoolBase *pool);
STaosQueue       *tQWorkerAllocQueue(SQWorkerPoolBase *pool, void *ahandle, FItem fp);
void              tQWorkerFreeQueue(SQWorkerPoolBase *pool, STaosQueue *queue);

int32_t     tAutoQWorkerInit(SAutoQWorkerPool *pool);
void        tAutoQWorkerCleanup(SAutoQWorkerPool *pool);
STaosQueue *tAutoQWorkerAllocQueue(SAutoQWorkerPool *pool, void *ahandle, FItem fp);
void        tAutoQWorkerFreeQueue(SAutoQWorkerPool *pool, STaosQueue *queue);

int32_t     tWWorkerInit(SWWorkerPool *pool);
void        tWWorkerCleanup(SWWorkerPool *pool);
STaosQueue *tWWorkerAllocQueue(SWWorkerPool *pool, void *ahandle, FItems fp);
void        tWWorkerFreeQueue(SWWorkerPool *pool, STaosQueue *queue);

typedef struct {
  const char     *name;
  int32_t         min;
  int32_t         max;
  FItem           fp;
  void           *param;
  SWorkerPoolType poolType;
} SSingleWorkerCfg;

typedef struct {
  const char       *name;
  STaosQueue       *queue;
  SQWorkerPoolBase *pool;
} SSingleWorker;

typedef struct {
  const char *name;
  int32_t     max;
  FItems      fp;
  void       *param;
} SMultiWorkerCfg;

typedef struct {
  const char  *name;
  STaosQueue  *queue;
  SWWorkerPool pool;
} SMultiWorker;

int32_t tSingleWorkerInit(SSingleWorker *pWorker, const SSingleWorkerCfg *pCfg);
void    tSingleWorkerCleanup(SSingleWorker *pWorker);
int32_t tMultiWorkerInit(SMultiWorker *pWorker, const SMultiWorkerCfg *pCfg);
void    tMultiWorkerCleanup(SMultiWorker *pWorker);

struct SQueryAutoQWorkerPoolCB;

typedef struct SQueryAutoQWorker {
  int32_t  id;      // worker id
  int32_t  backupIdx;// the idx when put into backup pool
  int64_t  pid;     // thread pid
  TdThread thread;  // thread id
  void    *pool;
} SQueryAutoQWorker;

typedef struct SQueryAutoQWorkerPool {
  SQWorkerPoolBase base;
  int32_t          num;
  int32_t          maxInUse;

  int32_t       activeN; // running workers and workers waiting at reading new queue msg
  int32_t       runningN; // workers processing queue msgs, not include blocking/waitingA/waitingB workers.

  int32_t       waitingAfterBlockN; // workers that recovered from blocking but waiting for too many running workers
  TdThreadMutex waitingAfterBlockLock;
  TdThreadCond  waitingAfterBlockCond;

  int32_t       waitingBeforeProcessMsgN; // workers that get msg from queue, but waiting for too many running workers
  TdThreadMutex waitingBeforeProcessMsgLock;
  TdThreadCond  waitingBeforeProcessMsgCond;

  int32_t       backupNum; // workers that are in backup pool, not reading msg from queue
  TdThreadMutex backupLock;
  TdThreadCond  backupCond;

  TdThreadMutex                   poolLock;
  SList                          *workers;
  SList                          *backupWorkers;
  SList                          *exitedWorkers;
  STaosQset                      *qset;
  struct SQueryAutoQWorkerPoolCB *pCb;
  bool                            exit;
} SQueryAutoQWorkerPool;

SQWorkerPoolBase* tQueryAutoQWorkerCreate();
int32_t     tQueryAutoQWorkerInit(SQWorkerPoolBase *pPool);
void        tQueryAutoQWorkerCleanup(SQWorkerPoolBase *pPool);
STaosQueue *tQueryAutoQWorkerAllocQueue(SQWorkerPoolBase *pPool, void *ahandle, FItem fp);
void        tQueryAutoQWorkerFreeQueue(SQWorkerPoolBase* pPool, STaosQueue* pQ);

typedef struct SQueryAutoQWorkerPoolCB {
  void *pPool;
  int32_t (*beforeBlocking)(void *pPool);
  int32_t (*afterRecoverFromBlocking)(void *pPool);
} SQueryAutoQWorkerPoolCB;

#ifdef __cplusplus
}
#endif

#endif /*_TD_UTIL_WORKER_H_*/
