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

#include "tq.h"

int64_t tqFetchLog(STQ* pTq, STqHandle* pHandle, int64_t* fetchOffset, SWalHead** ppHeadWithCkSum) {
  int32_t code = 0;
  taosThreadMutexLock(&pHandle->pWalReader->mutex);
  int64_t offset = *fetchOffset;

  while (1) {
    if (walFetchHead(pHandle->pWalReader, offset, *ppHeadWithCkSum) < 0) {
      tqDebug("tmq poll: consumer %ld (epoch %d) vg %d offset %ld, no more log to return", pHandle->consumerId,
              pHandle->epoch, TD_VID(pTq->pVnode), offset);
      *fetchOffset = offset - 1;
      code = -1;
      goto END;
    }

    if ((*ppHeadWithCkSum)->head.msgType == TDMT_VND_SUBMIT) {
      code = walFetchBody(pHandle->pWalReader, ppHeadWithCkSum);

      if (code < 0) {
        ASSERT(0);
        *fetchOffset = offset;
        code = -1;
        goto END;
      }
      *fetchOffset = offset;
      code = 0;
      goto END;
    } else {
      if (pHandle->fetchMeta) {
        SWalReadHead* pHead = &((*ppHeadWithCkSum)->head);
        if (pHead->msgType == TDMT_VND_CREATE_STB || pHead->msgType == TDMT_VND_ALTER_STB ||
            pHead->msgType == TDMT_VND_DROP_STB || pHead->msgType == TDMT_VND_CREATE_TABLE ||
            pHead->msgType == TDMT_VND_ALTER_TABLE || pHead->msgType == TDMT_VND_DROP_TABLE ||
            pHead->msgType == TDMT_VND_DROP_TTL_TABLE) {
          code = walFetchBody(pHandle->pWalReader, ppHeadWithCkSum);

          if (code < 0) {
            ASSERT(0);
            *fetchOffset = offset;
            code = -1;
            goto END;
          }
          *fetchOffset = offset;
          code = 0;
          goto END;
        }
      }
      code = walSkipFetchBody(pHandle->pWalReader, *ppHeadWithCkSum);
      if (code < 0) {
        ASSERT(0);
        *fetchOffset = offset;
        code = -1;
        goto END;
      }
      offset++;
    }
  }
END:
  taosThreadMutexUnlock(&pHandle->pWalReader->mutex);
  return code;
}

STqReadHandle* tqInitSubmitMsgScanner(SMeta* pMeta) {
  STqReadHandle* pReadHandle = taosMemoryMalloc(sizeof(STqReadHandle));
  if (pReadHandle == NULL) {
    return NULL;
  }
  pReadHandle->pVnodeMeta = pMeta;
  pReadHandle->pMsg = NULL;
  pReadHandle->ver = -1;
  pReadHandle->pColIdList = NULL;
  pReadHandle->cachedSchemaVer = -1;
  pReadHandle->cachedSchemaSuid = -1;
  pReadHandle->pSchema = NULL;
  pReadHandle->pSchemaWrapper = NULL;
  pReadHandle->tbIdHash = NULL;
  return pReadHandle;
}

int32_t tqReadHandleSetMsg(STqReadHandle* pReadHandle, SSubmitReq* pMsg, int64_t ver) {
  pReadHandle->pMsg = pMsg;

  if (tInitSubmitMsgIter(pMsg, &pReadHandle->msgIter) < 0) return -1;
  while (true) {
    if (tGetSubmitMsgNext(&pReadHandle->msgIter, &pReadHandle->pBlock) < 0) return -1;
    if (pReadHandle->pBlock == NULL) break;
  }

  if (tInitSubmitMsgIter(pMsg, &pReadHandle->msgIter) < 0) return -1;
  pReadHandle->ver = ver;
  memset(&pReadHandle->blkIter, 0, sizeof(SSubmitBlkIter));
  return 0;
}

bool tqNextDataBlock(STqReadHandle* pHandle) {
  while (1) {
    if (tGetSubmitMsgNext(&pHandle->msgIter, &pHandle->pBlock) < 0) {
      return false;
    }
    if (pHandle->pBlock == NULL) return false;

    if (pHandle->tbIdHash == NULL) {
      return true;
    }
    void* ret = taosHashGet(pHandle->tbIdHash, &pHandle->msgIter.uid, sizeof(int64_t));
    if (ret != NULL) {
      return true;
    }
  }
  return false;
}

bool tqNextDataBlockFilterOut(STqReadHandle* pHandle, SHashObj* filterOutUids) {
  while (1) {
    if (tGetSubmitMsgNext(&pHandle->msgIter, &pHandle->pBlock) < 0) {
      return false;
    }
    if (pHandle->pBlock == NULL) return false;

    ASSERT(pHandle->tbIdHash == NULL);
    void* ret = taosHashGet(filterOutUids, &pHandle->msgIter.uid, sizeof(int64_t));
    if (ret == NULL) {
      return true;
    }
  }
  return false;
}

int32_t tqRetrieveDataBlock(SSDataBlock* pBlock, STqReadHandle* pHandle, uint64_t* pGroupId, uint64_t* pUid,
                            int32_t* pNumOfRows) {
  *pUid = 0;

  // TODO set to real sversion
  /*int32_t sversion = 1;*/
  int32_t sversion = htonl(pHandle->pBlock->sversion);
  if (pHandle->cachedSchemaSuid == 0 || pHandle->cachedSchemaVer != sversion ||
      pHandle->cachedSchemaSuid != pHandle->msgIter.suid) {
    pHandle->pSchema = metaGetTbTSchema(pHandle->pVnodeMeta, pHandle->msgIter.uid, sversion);
    if (pHandle->pSchema == NULL) {
      tqWarn("cannot found tsschema for table: uid: %ld (suid: %ld), version %d, possibly dropped table",
             pHandle->msgIter.uid, pHandle->msgIter.suid, pHandle->cachedSchemaVer);
      /*ASSERT(0);*/
      terrno = TSDB_CODE_TQ_TABLE_SCHEMA_NOT_FOUND;
      return -1;
    }

    // this interface use suid instead of uid
    pHandle->pSchemaWrapper = metaGetTableSchema(pHandle->pVnodeMeta, pHandle->msgIter.uid, sversion, true);
    if (pHandle->pSchemaWrapper == NULL) {
      tqWarn("cannot found schema wrapper for table: suid: %ld, version %d, possibly dropped table",
             pHandle->msgIter.uid, pHandle->cachedSchemaVer);
      /*ASSERT(0);*/
      terrno = TSDB_CODE_TQ_TABLE_SCHEMA_NOT_FOUND;
      return -1;
    }
    pHandle->cachedSchemaVer = sversion;
    pHandle->cachedSchemaSuid = pHandle->msgIter.suid;
  }

  STSchema*       pTschema = pHandle->pSchema;
  SSchemaWrapper* pSchemaWrapper = pHandle->pSchemaWrapper;

  *pNumOfRows = pHandle->msgIter.numOfRows;
  int32_t colNumNeed = taosArrayGetSize(pHandle->pColIdList);

  if (colNumNeed == 0) {
    int32_t colMeta = 0;
    while (colMeta < pSchemaWrapper->nCols) {
      SSchema*        pColSchema = &pSchemaWrapper->pSchema[colMeta];
      SColumnInfoData colInfo = createColumnInfoData(pColSchema->type, pColSchema->bytes, pColSchema->colId);
      int32_t code = blockDataAppendColInfo(pBlock, &colInfo);
      if (code != TSDB_CODE_SUCCESS) {
        goto FAIL;
      }
      colMeta++;
    }
  } else {
    if (colNumNeed > pSchemaWrapper->nCols) {
      colNumNeed = pSchemaWrapper->nCols;
    }

    int32_t colMeta = 0;
    int32_t colNeed = 0;
    while (colMeta < pSchemaWrapper->nCols && colNeed < colNumNeed) {
      SSchema* pColSchema = &pSchemaWrapper->pSchema[colMeta];
      col_id_t colIdSchema = pColSchema->colId;
      col_id_t colIdNeed = *(col_id_t*)taosArrayGet(pHandle->pColIdList, colNeed);
      if (colIdSchema < colIdNeed) {
        colMeta++;
      } else if (colIdSchema > colIdNeed) {
        colNeed++;
      } else {
        SColumnInfoData colInfo = createColumnInfoData(pColSchema->type, pColSchema->bytes, pColSchema->colId);
        int32_t code = blockDataAppendColInfo(pBlock, &colInfo);
        if (code != TSDB_CODE_SUCCESS) {
          goto FAIL;
        }
        colMeta++;
        colNeed++;
      }
    }
  }

  if (blockDataEnsureCapacity(pBlock, *pNumOfRows) < 0) {
    goto FAIL;
  }

  int32_t colActual = blockDataGetNumOfCols(pBlock);

  // TODO in stream shuffle case, fetch groupId
  *pGroupId = 0;

  STSRowIter iter = {0};
  tdSTSRowIterInit(&iter, pTschema);
  STSRow* row;
  int32_t curRow = 0;

  tInitSubmitBlkIter(&pHandle->msgIter, pHandle->pBlock, &pHandle->blkIter);
  *pUid = pHandle->msgIter.uid;  // set the uid of table for submit block

  while ((row = tGetSubmitBlkNext(&pHandle->blkIter)) != NULL) {
    tdSTSRowIterReset(&iter, row);
    // get all wanted col of that block
    for (int32_t i = 0; i < colActual; i++) {
      SColumnInfoData* pColData = taosArrayGet(pBlock->pDataBlock, i);
      SCellVal         sVal = {0};
      if (!tdSTSRowIterNext(&iter, pColData->info.colId, pColData->info.type, &sVal)) {
        break;
      }
      if (colDataAppend(pColData, curRow, sVal.val, sVal.valType == TD_VTYPE_NULL) < 0) {
        goto FAIL;
      }
    }
    curRow++;
  }
  return 0;

FAIL: // todo refactor here
//  if (*ppCols) taosArrayDestroy(*ppCols);
  return -1;
}

void tqReadHandleSetColIdList(STqReadHandle* pReadHandle, SArray* pColIdList) { pReadHandle->pColIdList = pColIdList; }

int tqReadHandleSetTbUidList(STqReadHandle* pHandle, const SArray* tbUidList) {
  if (pHandle->tbIdHash) {
    taosHashClear(pHandle->tbIdHash);
  }

  pHandle->tbIdHash = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_NO_LOCK);
  if (pHandle->tbIdHash == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return -1;
  }

  for (int i = 0; i < taosArrayGetSize(tbUidList); i++) {
    int64_t* pKey = (int64_t*)taosArrayGet(tbUidList, i);
    taosHashPut(pHandle->tbIdHash, pKey, sizeof(int64_t), NULL, 0);
  }

  return 0;
}

int tqReadHandleAddTbUidList(STqReadHandle* pHandle, const SArray* tbUidList) {
  if (pHandle->tbIdHash == NULL) {
    pHandle->tbIdHash = taosHashInit(64, taosGetDefaultHashFunction(TSDB_DATA_TYPE_BIGINT), true, HASH_NO_LOCK);
    if (pHandle->tbIdHash == NULL) {
      terrno = TSDB_CODE_OUT_OF_MEMORY;
      return -1;
    }
  }

  for (int i = 0; i < taosArrayGetSize(tbUidList); i++) {
    int64_t* pKey = (int64_t*)taosArrayGet(tbUidList, i);
    taosHashPut(pHandle->tbIdHash, pKey, sizeof(int64_t), NULL, 0);
  }

  return 0;
}

int tqReadHandleRemoveTbUidList(STqReadHandle* pHandle, const SArray* tbUidList) {
  ASSERT(pHandle->tbIdHash != NULL);

  for (int32_t i = 0; i < taosArrayGetSize(tbUidList); i++) {
    int64_t* pKey = (int64_t*)taosArrayGet(tbUidList, i);
    taosHashRemove(pHandle->tbIdHash, pKey, sizeof(int64_t));
  }

  return 0;
}

int32_t tqUpdateTbUidList(STQ* pTq, const SArray* tbUidList, bool isAdd) {
  void* pIter = NULL;
  while (1) {
    pIter = taosHashIterate(pTq->handles, pIter);
    if (pIter == NULL) break;
    STqHandle* pExec = (STqHandle*)pIter;
    if (pExec->execHandle.subType == TOPIC_SUB_TYPE__COLUMN) {
      for (int32_t i = 0; i < 5; i++) {
        int32_t code = qUpdateQualifiedTableId(pExec->execHandle.execCol.task[i], tbUidList, isAdd);
        ASSERT(code == 0);
      }
    } else if (pExec->execHandle.subType == TOPIC_SUB_TYPE__DB) {
      if (!isAdd) {
        int32_t sz = taosArrayGetSize(tbUidList);
        for (int32_t i = 0; i < sz; i++) {
          int64_t tbUid = *(int64_t*)taosArrayGet(tbUidList, i);
          taosHashPut(pExec->execHandle.execDb.pFilterOutTbUid, &tbUid, sizeof(int64_t), NULL, 0);
        }
      }
    } else {
      // tq update id
    }
  }
  while (1) {
    pIter = taosHashIterate(pTq->pStreamTasks, pIter);
    if (pIter == NULL) break;
    SStreamTask* pTask = (SStreamTask*)pIter;
    if (pTask->inputType == STREAM_INPUT__DATA_SUBMIT) {
      int32_t code = qUpdateQualifiedTableId(pTask->exec.executor, tbUidList, isAdd);
      ASSERT(code == 0);
    }
  }
  return 0;
}
