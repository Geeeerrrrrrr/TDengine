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

#define _DEFAULT_SOURCE
#include "os.h"
#include "ihash.h"
#include "taoserror.h"
#include "taosmsg.h"
#include "tutil.h"
#include "trpc.h"
#include "tsdb.h"
#include "ttime.h"
#include "ttimer.h"
#include "cJSON.h"
#include "twal.h"
#include "tglobal.h"
#include "dnode.h"
#include "vnode.h"
#include "vnodeInt.h"
#include "vnodeLog.h"

static int32_t  tsOpennedVnodes;
static void    *tsDnodeVnodesHash;
static void     vnodeCleanUp(SVnodeObj *pVnode);
static void     vnodeBuildVloadMsg(char *pNode, void * param);
static int      vnodeWalCallback(void *arg);
static int32_t  vnodeSaveCfg(SMDCreateVnodeMsg *pVnodeCfg);
static int32_t  vnodeReadCfg(SVnodeObj *pVnode);
static int32_t  vnodeSaveVersion(SVnodeObj *pVnode);
static int32_t  vnodeReadVersion(SVnodeObj *pVnode);
static int      vnodeWalCallback(void *arg);
static uint32_t vnodeGetFileInfo(void *ahandle, char *name, uint32_t *index, int32_t *size);
static int      vnodeGetWalInfo(void *ahandle, char *name, uint32_t *index);
static void     vnodeNotifyRole(void *ahandle, int8_t role);

static pthread_once_t  vnodeModuleInit = PTHREAD_ONCE_INIT;

#ifndef _SYNC
tsync_h syncStart(const SSyncInfo *info) { return NULL; }
int     syncForwardToPeer(tsync_h shandle, void *pHead, void *mhandle) { return 0; }
void    syncStop(tsync_h shandle) {}
int     syncReconfig(tsync_h shandle, const SSyncCfg * cfg) { return 0; }
int     syncGetNodesRole(tsync_h shandle, SNodesRole * cfg) { return 0; }
void    syncConfirmForward(tsync_h shandle, uint64_t version, int32_t code) {}
#endif

static void vnodeInit() {
  vnodeInitWriteFp();
  vnodeInitReadFp();

  tsDnodeVnodesHash = taosInitIntHash(TSDB_MAX_VNODES, sizeof(SVnodeObj *), taosHashInt);
  if (tsDnodeVnodesHash == NULL) {
    dError("failed to init vnode list");
  }
}

int32_t vnodeCreate(SMDCreateVnodeMsg *pVnodeCfg) {
  int32_t code;
  pthread_once(&vnodeModuleInit, vnodeInit);

  SVnodeObj *pTemp = (SVnodeObj *)taosGetIntHashData(tsDnodeVnodesHash, pVnodeCfg->cfg.vgId);
  if (pTemp != NULL) {
    dPrint("vgId:%d, vnode already exist, pVnode:%p", pVnodeCfg->cfg.vgId, pTemp);
    return TSDB_CODE_SUCCESS;
  }

  char rootDir[TSDB_FILENAME_LEN] = {0};
  sprintf(rootDir, "%s/vnode%d", tsVnodeDir, pVnodeCfg->cfg.vgId);
  if (mkdir(rootDir, 0755) != 0) {
    if (errno == EACCES) {
      return TSDB_CODE_NO_DISK_PERMISSIONS;
    } else if (errno == ENOSPC) {
      return TSDB_CODE_SERV_NO_DISKSPACE;
    } else if (errno == EEXIST) {
    } else {
      return TSDB_CODE_VG_INIT_FAILED;
    }
  }

  code = vnodeSaveCfg(pVnodeCfg);
  if (code != TSDB_CODE_SUCCESS) {
    dError("vgId:%d, failed to save vnode cfg, reason:%s", pVnodeCfg->cfg.vgId, tstrerror(code));
    return code;
  }

  STsdbCfg tsdbCfg = {0};
  tsdbCfg.precision           = pVnodeCfg->cfg.precision;
  tsdbCfg.compression         = pVnodeCfg->cfg.compression;;
  tsdbCfg.tsdbId              = pVnodeCfg->cfg.vgId;
  tsdbCfg.maxTables           = pVnodeCfg->cfg.maxTables;
  tsdbCfg.daysPerFile         = pVnodeCfg->cfg.daysPerFile;
  tsdbCfg.minRowsPerFileBlock = pVnodeCfg->cfg.minRowsPerFileBlock;
  tsdbCfg.maxRowsPerFileBlock = pVnodeCfg->cfg.maxRowsPerFileBlock;
  tsdbCfg.keep                = pVnodeCfg->cfg.daysToKeep;
  tsdbCfg.maxCacheSize        = pVnodeCfg->cfg.maxCacheSize;

  char tsdbDir[TSDB_FILENAME_LEN] = {0};
  sprintf(tsdbDir, "%s/vnode%d/tsdb", tsVnodeDir, pVnodeCfg->cfg.vgId);
  code = tsdbCreateRepo(tsdbDir, &tsdbCfg, NULL);
  if (code != TSDB_CODE_SUCCESS) {
    dError("vgId:%d, failed to create tsdb in vnode, reason:%s", pVnodeCfg->cfg.vgId, tstrerror(code));
    return TSDB_CODE_VG_INIT_FAILED;
  }

  dPrint("vgId:%d, vnode is created, clog:%d", pVnodeCfg->cfg.vgId, pVnodeCfg->cfg.commitLog);
  code = vnodeOpen(pVnodeCfg->cfg.vgId, rootDir);

  return code;
}

int32_t vnodeDrop(int32_t vgId) {
  SVnodeObj **ppVnode = (SVnodeObj **)taosGetIntHashData(tsDnodeVnodesHash, vgId);
  if (ppVnode == NULL || *ppVnode == NULL) {
    dTrace("vgId:%d, failed to drop, vgId not exist", vgId);
    return TSDB_CODE_INVALID_VGROUP_ID;
  }

  SVnodeObj *pVnode = *ppVnode;
  dTrace("pVnode:%p vgId:%d, vnode will be dropped", pVnode, pVnode->vgId);
  pVnode->status = TAOS_VN_STATUS_DELETING;
  vnodeCleanUp(pVnode);
 
  return TSDB_CODE_SUCCESS;
}

int32_t vnodeAlter(void *param, SMDCreateVnodeMsg *pVnodeCfg) {
  SVnodeObj *pVnode = param;
  int32_t code = vnodeSaveCfg(pVnodeCfg);
  if (code != TSDB_CODE_SUCCESS) {
    dError("vgId:%d, failed to save vnode cfg, reason:%s", pVnodeCfg->cfg.vgId, tstrerror(code));
    return code;
  }

  code = vnodeReadCfg(pVnode);
  if (code != TSDB_CODE_SUCCESS) {
    dError("pVnode:%p vgId:%d, failed to read cfg file", pVnode, pVnode->vgId);
    taosDeleteIntHash(tsDnodeVnodesHash, pVnode->vgId);
    return code;
  }

  code = syncReconfig(pVnode->sync, &pVnode->syncCfg);
  if (code != TSDB_CODE_SUCCESS) {
    dTrace("pVnode:%p vgId:%d, failed to alter vnode, canot reconfig sync, result:%s", pVnode, pVnode->vgId,
           tstrerror(code));
    return code;
  }

  code = tsdbConfigRepo(pVnode->tsdb, &pVnode->tsdbCfg);
  if (code != TSDB_CODE_SUCCESS) {
    dTrace("pVnode:%p vgId:%d, failed to alter vnode, canot reconfig tsdb, result:%s", pVnode, pVnode->vgId,
           tstrerror(code));
    return code;
  }

  dTrace("pVnode:%p vgId:%d, vnode is altered", pVnode, pVnode->vgId);
  return TSDB_CODE_SUCCESS;
}

int32_t vnodeOpen(int32_t vnode, char *rootDir) {
  char temp[TSDB_FILENAME_LEN];
  pthread_once(&vnodeModuleInit, vnodeInit);

  SVnodeObj *pVnode = calloc(sizeof(SVnodeObj), 1);
  pVnode->vgId     = vnode;
  pVnode->status   = TAOS_VN_STATUS_INIT;
  pVnode->refCount = 1;
  pVnode->version  = 0;  
  taosAddIntHash(tsDnodeVnodesHash, pVnode->vgId, (char *)(&pVnode));

  int32_t code = vnodeReadCfg(pVnode);
  if (code != TSDB_CODE_SUCCESS) {
    dError("pVnode:%p vgId:%d, failed to read cfg file", pVnode, pVnode->vgId);
    taosDeleteIntHash(tsDnodeVnodesHash, pVnode->vgId);
    return code;
  }

  vnodeReadVersion(pVnode);
  
  pVnode->wqueue = dnodeAllocateWqueue(pVnode);
  pVnode->rqueue = dnodeAllocateRqueue(pVnode);

  sprintf(temp, "%s/wal", rootDir);
  pVnode->wal = walOpen(temp, &pVnode->walCfg);

  SSyncInfo syncInfo;
  syncInfo.vgId = pVnode->vgId;
  syncInfo.version = pVnode->version;
  syncInfo.syncCfg = pVnode->syncCfg;
  sprintf(syncInfo.path, "%s/tsdb/", rootDir);
  syncInfo.ahandle = pVnode;
  syncInfo.getWalInfo = vnodeGetWalInfo;
  syncInfo.getFileInfo = vnodeGetFileInfo;
  syncInfo.writeToCache = vnodeWriteToQueue;
  syncInfo.confirmForward = dnodeSendRpcWriteRsp; 
  syncInfo.notifyRole = vnodeNotifyRole;
  pVnode->sync = syncStart(&syncInfo);

  pVnode->events = NULL;
  pVnode->cq     = NULL;

  STsdbAppH appH = {0};
  appH.appH = (void *)pVnode;
  appH.walCallBack = vnodeWalCallback;

  sprintf(temp, "%s/tsdb", rootDir);
  void *pTsdb = tsdbOpenRepo(temp, &appH);
  if (pTsdb == NULL) {
    dError("pVnode:%p vgId:%d, failed to open tsdb at %s(%s)", pVnode, pVnode->vgId, temp, tstrerror(terrno));
    taosDeleteIntHash(tsDnodeVnodesHash, pVnode->vgId);
    return terrno;
  }

  pVnode->tsdb = pTsdb;

  walRestore(pVnode->wal, pVnode, vnodeWriteToQueue);

  pVnode->status = TAOS_VN_STATUS_READY;
  dTrace("pVnode:%p vgId:%d, vnode is opened in %s", pVnode, pVnode->vgId, rootDir);

  atomic_add_fetch_32(&tsOpennedVnodes, 1);
  return TSDB_CODE_SUCCESS;
}

int32_t vnodeClose(int32_t vgId) {
  SVnodeObj **ppVnode = (SVnodeObj **)taosGetIntHashData(tsDnodeVnodesHash, vgId);
  if (ppVnode == NULL || *ppVnode == NULL) return 0;

  SVnodeObj *pVnode = *ppVnode;
  dTrace("pVnode:%p vgId:%d, vnode will be closed", pVnode, pVnode->vgId);
  pVnode->status = TAOS_VN_STATUS_CLOSING;
  vnodeCleanUp(pVnode);

  return 0;
}

void vnodeRelease(void *pVnodeRaw) {
  SVnodeObj *pVnode = pVnodeRaw;
  int32_t    vgId = pVnode->vgId;

  int32_t refCount = atomic_sub_fetch_32(&pVnode->refCount, 1);
  assert(refCount >= 0);

  if (refCount > 0) {
    dTrace("pVnode:%p vgId:%d, release vnode, refCount:%d", pVnode, vgId, refCount);
    return;
  }

  // remove read queue
  dnodeFreeRqueue(pVnode->rqueue);
  pVnode->rqueue = NULL;

  // remove write queue
  dnodeFreeWqueue(pVnode->wqueue);
  pVnode->wqueue = NULL;

  if (pVnode->status == TAOS_VN_STATUS_DELETING) {
    char rootDir[TSDB_FILENAME_LEN] = {0};
    sprintf(rootDir, "%s/vnode%d", tsVnodeDir, vgId);
    taosRemoveDir(rootDir);
  }

  free(pVnode);

  int32_t count = atomic_sub_fetch_32(&tsOpennedVnodes, 1);
  dTrace("pVnode:%p vgId:%d, vnode is released, vnodes:%d", pVnode, vgId, count);

  if (count <= 0) {
    taosCleanUpIntHash(tsDnodeVnodesHash);
    vnodeModuleInit = PTHREAD_ONCE_INIT;
    tsDnodeVnodesHash = NULL;
  }
}

void *vnodeGetVnode(int32_t vgId) {
  SVnodeObj **ppVnode = (SVnodeObj **)taosGetIntHashData(tsDnodeVnodesHash, vgId);
  if (ppVnode == NULL || *ppVnode == NULL) {
    terrno = TSDB_CODE_INVALID_VGROUP_ID;
    dError("vgId:%d not exist");
    return NULL;
  }

  return *ppVnode;
}

void *vnodeAccquireVnode(int32_t vgId) {
  SVnodeObj *pVnode = vnodeGetVnode(vgId);
  if (pVnode == NULL) return pVnode;

  atomic_add_fetch_32(&pVnode->refCount, 1);
  dTrace("pVnode:%p vgId:%d, get vnode, refCount:%d", pVnode, pVnode->vgId, pVnode->refCount);

  return pVnode;
}

void *vnodeGetRqueue(void *pVnode) {
  return ((SVnodeObj *)pVnode)->rqueue; 
}

void *vnodeGetWqueue(int32_t vgId) {
  SVnodeObj *pVnode = vnodeAccquireVnode(vgId);
  if (pVnode == NULL) return NULL;
  return pVnode->wqueue;
} 

void *vnodeGetWal(void *pVnode) {
  return ((SVnodeObj *)pVnode)->wal; 
}

void vnodeBuildStatusMsg(void *param) {
  SDMStatusMsg *pStatus = param;
  taosVisitIntHashWithFp(tsDnodeVnodesHash, vnodeBuildVloadMsg, pStatus);
}

static void vnodeBuildVloadMsg(char *pNode, void * param) {
  SVnodeObj *pVnode = *(SVnodeObj **) pNode;
  if (pVnode->status == TAOS_VN_STATUS_DELETING) return;

  SDMStatusMsg *pStatus = param;
  if (pStatus->openVnodes >= TSDB_MAX_VNODES) return;

  SVnodeLoad *pLoad = &pStatus->load[pStatus->openVnodes++];
  pLoad->vgId = htonl(pVnode->vgId);
  pLoad->status = pVnode->status;
  pLoad->role = pVnode->role;
  pLoad->replica = pVnode->syncCfg.replica;
}

static void vnodeCleanUp(SVnodeObj *pVnode) {
  
  taosDeleteIntHash(tsDnodeVnodesHash, pVnode->vgId);

  //syncStop(pVnode->sync);
  tsdbCloseRepo(pVnode->tsdb);
  walClose(pVnode->wal);
  vnodeSaveVersion(pVnode);

  vnodeRelease(pVnode);
}

// TODO: this is a simple implement
static int vnodeWalCallback(void *arg) {
  SVnodeObj *pVnode = arg;
  return walRenew(pVnode->wal);
}

static uint32_t vnodeGetFileInfo(void *ahandle, char *name, uint32_t *index, int32_t *size) {
  // SVnodeObj *pVnode = ahandle;
  //tsdbGetFileInfo(pVnode->tsdb, name, index, size);
  return 0;
}

static int vnodeGetWalInfo(void *ahandle, char *name, uint32_t *index) {
  SVnodeObj *pVnode = ahandle;
  return walGetWalFile(pVnode->wal, name, index);
}

static void vnodeNotifyRole(void *ahandle, int8_t role) {
  SVnodeObj *pVnode = ahandle;
  pVnode->role = role;
}

static int32_t vnodeSaveCfg(SMDCreateVnodeMsg *pVnodeCfg) {
  char cfgFile[TSDB_FILENAME_LEN + 30] = {0};
  sprintf(cfgFile, "%s/vnode%d/config.json", tsVnodeDir, pVnodeCfg->cfg.vgId);
  FILE *fp = fopen(cfgFile, "w");
  if (!fp) {
    dError("vgId:%d, failed to open vnode cfg file for write, error:%s", pVnodeCfg->cfg.vgId, strerror(errno));
    return errno;
  }

  char    ipStr[20];
  int32_t len = 0;
  int32_t maxLen = 1000;
  char *  content = calloc(1, maxLen + 1);

  len += snprintf(content + len, maxLen - len, "{\n");

  len += snprintf(content + len, maxLen - len, "  \"precision\": %d,\n", pVnodeCfg->cfg.precision);
  len += snprintf(content + len, maxLen - len, "  \"compression\": %d,\n", pVnodeCfg->cfg.compression);
  len += snprintf(content + len, maxLen - len, "  \"maxTables\": %d,\n", pVnodeCfg->cfg.maxTables);
  len += snprintf(content + len, maxLen - len, "  \"daysPerFile\": %d,\n", pVnodeCfg->cfg.daysPerFile);
  len += snprintf(content + len, maxLen - len, "  \"minRowsPerFileBlock\": %d,\n", pVnodeCfg->cfg.minRowsPerFileBlock);
  len += snprintf(content + len, maxLen - len, "  \"maxRowsPerFileBlock\": %d,\n", pVnodeCfg->cfg.maxRowsPerFileBlock);
  len += snprintf(content + len, maxLen - len, "  \"daysToKeep\": %d,\n", pVnodeCfg->cfg.daysToKeep);

  len += snprintf(content + len, maxLen - len, "  \"maxCacheSize\": %" PRId64 ",\n", pVnodeCfg->cfg.maxCacheSize);
  
  len += snprintf(content + len, maxLen - len, "  \"commitLog\": %d,\n", pVnodeCfg->cfg.commitLog);
  len += snprintf(content + len, maxLen - len, "  \"wals\": %d,\n", pVnodeCfg->cfg.wals);

  uint32_t ipInt =  pVnodeCfg->cfg.arbitratorIp;
  sprintf(ipStr, "%u.%u.%u.%u", ipInt & 0xFF, (ipInt >> 8) & 0xFF, (ipInt >> 16) & 0xFF, (uint8_t)(ipInt >> 24));
  len += snprintf(content + len, maxLen - len, "  \"arbitratorIp\": \"%s\",\n", ipStr);

  len += snprintf(content + len, maxLen - len, "  \"quorum\": %d,\n", pVnodeCfg->cfg.quorum);
  len += snprintf(content + len, maxLen - len, "  \"replica\": %d,\n", pVnodeCfg->cfg.replications);

  len += snprintf(content + len, maxLen - len, "  \"nodeInfos\": [{\n");
  for (int32_t i = 0; i < pVnodeCfg->cfg.replications; i++) {
    len += snprintf(content + len, maxLen - len, "    \"nodeId\": %d,\n", pVnodeCfg->nodes[i].nodeId);

    uint32_t ipInt = pVnodeCfg->nodes[i].nodeIp;
    sprintf(ipStr, "%u.%u.%u.%u", ipInt & 0xFF, (ipInt >> 8) & 0xFF, (ipInt >> 16) & 0xFF, (uint8_t)(ipInt >> 24));
    len += snprintf(content + len, maxLen - len, "    \"nodeIp\": \"%s\",\n", ipStr);

    len += snprintf(content + len, maxLen - len, "    \"nodeName\": \"%s\"\n", pVnodeCfg->nodes[i].nodeName);

    if (i < pVnodeCfg->cfg.replications - 1) {
      len += snprintf(content + len, maxLen - len, "  },{\n");
    } else {
      len += snprintf(content + len, maxLen - len, "  }]\n");
    }
  }
  len += snprintf(content + len, maxLen - len, "}\n");

  fwrite(content, 1, len, fp);
  fclose(fp);
  free(content);

  dPrint("vgId:%d, save vnode cfg successed", pVnodeCfg->cfg.vgId);

  return 0;
}

static int32_t vnodeReadCfg(SVnodeObj *pVnode) {
  char cfgFile[TSDB_FILENAME_LEN + 30] = {0};
  sprintf(cfgFile, "%s/vnode%d/config.json", tsVnodeDir, pVnode->vgId);
  FILE *fp = fopen(cfgFile, "r");
  if (!fp) {
    dError("pVnode:%p vgId:%d, failed to open vnode cfg file for read, error:%s", pVnode, pVnode->vgId, strerror(errno));
    return errno;
  }

  int   ret = TSDB_CODE_OTHERS;
  int   maxLen = 1000;
  char *content = calloc(1, maxLen + 1);
  int   len = fread(content, 1, maxLen, fp);
  if (len <= 0) {
    free(content);
    fclose(fp);
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, content is null", pVnode, pVnode->vgId);
    return false;
  }

  cJSON *root = cJSON_Parse(content);
  if (root == NULL) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, invalid json format", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }

  cJSON *precision = cJSON_GetObjectItem(root, "precision");
  if (!precision || precision->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, precision not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.precision = (int8_t)precision->valueint;

  cJSON *compression = cJSON_GetObjectItem(root, "compression");
  if (!compression || compression->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, compression not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.compression = (int8_t)compression->valueint;

  cJSON *maxTables = cJSON_GetObjectItem(root, "maxTables");
  if (!maxTables || maxTables->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, maxTables not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.maxTables = maxTables->valueint;

  cJSON *daysPerFile = cJSON_GetObjectItem(root, "daysPerFile");
  if (!daysPerFile || daysPerFile->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, daysPerFile not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.daysPerFile = daysPerFile->valueint;

  cJSON *minRowsPerFileBlock = cJSON_GetObjectItem(root, "minRowsPerFileBlock");
  if (!minRowsPerFileBlock || minRowsPerFileBlock->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, minRowsPerFileBlock not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.minRowsPerFileBlock = minRowsPerFileBlock->valueint;

  cJSON *maxRowsPerFileBlock = cJSON_GetObjectItem(root, "maxRowsPerFileBlock");
  if (!maxRowsPerFileBlock || maxRowsPerFileBlock->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, maxRowsPerFileBlock not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.maxRowsPerFileBlock = maxRowsPerFileBlock->valueint;

  cJSON *daysToKeep = cJSON_GetObjectItem(root, "daysToKeep");
  if (!daysToKeep || daysToKeep->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, daysToKeep not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.keep = daysToKeep->valueint;

  cJSON *maxCacheSize = cJSON_GetObjectItem(root, "maxCacheSize");
  if (!maxCacheSize || maxCacheSize->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, maxCacheSize not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->tsdbCfg.maxCacheSize = maxCacheSize->valueint;

  cJSON *commitLog = cJSON_GetObjectItem(root, "commitLog");
  if (!commitLog || commitLog->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, commitLog not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->walCfg.commitLog = (int8_t)commitLog->valueint;

  cJSON *wals = cJSON_GetObjectItem(root, "wals");
  if (!wals || wals->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, wals not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->walCfg.wals = (int8_t)wals->valueint;
  pVnode->walCfg.keep = 0;

  cJSON *arbitratorIp = cJSON_GetObjectItem(root, "arbitratorIp");
  if (!arbitratorIp || arbitratorIp->type != cJSON_String || arbitratorIp->valuestring == NULL) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, arbitratorIp not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->syncCfg.arbitratorIp = inet_addr(arbitratorIp->valuestring);

  cJSON *quorum = cJSON_GetObjectItem(root, "quorum");
  if (!quorum || quorum->type != cJSON_Number) {
    dError("failed to read vnode cfg, quorum not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->syncCfg.quorum = (int8_t)quorum->valueint;

  cJSON *replica = cJSON_GetObjectItem(root, "replica");
  if (!replica || replica->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, replica not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->syncCfg.replica = (int8_t)replica->valueint;

  cJSON *nodeInfos = cJSON_GetObjectItem(root, "nodeInfos");
  if (!nodeInfos || nodeInfos->type != cJSON_Array) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, nodeInfos not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }

  int size = cJSON_GetArraySize(nodeInfos);
  if (size != pVnode->syncCfg.replica) {
    dError("pVnode:%p vgId:%d, failed to read vnode cfg, nodeInfos size not matched", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }

  for (int i = 0; i < size; ++i) {
    cJSON *nodeInfo = cJSON_GetArrayItem(nodeInfos, i);
    if (nodeInfo == NULL) continue;

    cJSON *nodeId = cJSON_GetObjectItem(nodeInfo, "nodeId");
    if (!nodeId || nodeId->type != cJSON_Number) {
      dError("pVnode:%p vgId:%d, failed to read vnode cfg, nodeId not found", pVnode, pVnode->vgId);
      goto PARSE_OVER;
    }
    pVnode->syncCfg.nodeInfo[i].nodeId = nodeId->valueint;

    cJSON *nodeIp = cJSON_GetObjectItem(nodeInfo, "nodeIp");
    if (!nodeIp || nodeIp->type != cJSON_String || nodeIp->valuestring == NULL) {
      dError("pVnode:%p vgId:%d, failed to read vnode cfg, nodeIp not found", pVnode, pVnode->vgId);
      goto PARSE_OVER;
    }
    pVnode->syncCfg.nodeInfo[i].nodeIp = inet_addr(nodeIp->valuestring);

    cJSON *nodeName = cJSON_GetObjectItem(nodeInfo, "nodeName");
    if (!nodeName || nodeName->type != cJSON_String || nodeName->valuestring == NULL) {
      dError("pVnode:%p vgId:%d, failed to read vnode cfg, nodeName not found", pVnode, pVnode->vgId);
      goto PARSE_OVER;
    }
    strncpy(pVnode->syncCfg.nodeInfo[i].name, nodeName->valuestring, TSDB_NODE_NAME_LEN);
  }

  ret = 0;

  dPrint("pVnode:%p vgId:%d, read vnode cfg successed, replcia:%d", pVnode, pVnode->vgId, pVnode->syncCfg.replica);
  for (int32_t i = 0; i < pVnode->syncCfg.replica; i++) {
    dPrint("pVnode:%p vgId:%d, dnode:%d, ip:%s name:%s", pVnode, pVnode->vgId, pVnode->syncCfg.nodeInfo[i].nodeId,
           taosIpStr(pVnode->syncCfg.nodeInfo[i].nodeIp), pVnode->syncCfg.nodeInfo[i].name);
  }

PARSE_OVER:
  free(content);
  cJSON_Delete(root);
  fclose(fp);
  return ret;
}


static int32_t vnodeSaveVersion(SVnodeObj *pVnode) {
  char versionFile[TSDB_FILENAME_LEN + 30] = {0};
  sprintf(versionFile, "%s/vnode%d/version.json", tsVnodeDir, pVnode->vgId);
  FILE *fp = fopen(versionFile, "w");
  if (!fp) {
    dError("pVnode:%p vgId:%d, failed to open vnode version file for write, error:%s", pVnode, pVnode->vgId);
    return errno;
  }

  int32_t len = 0;
  int32_t maxLen = 30;
  char *  content = calloc(1, maxLen + 1);

  len += snprintf(content + len, maxLen - len, "{\n");
  len += snprintf(content + len, maxLen - len, "  \"version\": %" PRId64 "\n", pVnode->version);
  len += snprintf(content + len, maxLen - len, "}\n");

  fwrite(content, 1, len, fp);
  fclose(fp);
  free(content);

  dPrint("pVnode:%p vgId:%d, save vnode version successed", pVnode, pVnode->vgId);

  return 0;
}

static int32_t vnodeReadVersion(SVnodeObj *pVnode) {
  char versionFile[TSDB_FILENAME_LEN + 30] = {0};
  sprintf(versionFile, "%s/vnode%d/version.json", tsVnodeDir, pVnode->vgId);
  FILE *fp = fopen(versionFile, "w");
  if (!fp) {
    dError("pVnode:%p vgId:%d, failed to open vnode version file for write, error:%s", pVnode, pVnode->vgId);
    return errno;
  }

  int   ret = TSDB_CODE_OTHERS;
  int   maxLen = 100;
  char *content = calloc(1, maxLen + 1);
  int   len = fread(content, 1, maxLen, fp);
  if (len <= 0) {
    free(content);
    fclose(fp);
    dError("pVnode:%p vgId:%d, failed to read vnode version, content is null", pVnode, pVnode->vgId);
    return false;
  }

  cJSON *root = cJSON_Parse(content);
  if (root == NULL) {
    dError("pVnode:%p vgId:%d, failed to read vnode version, invalid json format", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }

  cJSON *version = cJSON_GetObjectItem(root, "version");
  if (!version || version->type != cJSON_Number) {
    dError("pVnode:%p vgId:%d, failed to read vnode version, version not found", pVnode, pVnode->vgId);
    goto PARSE_OVER;
  }
  pVnode->version = version->valueint;

  ret = 0;

  dPrint("pVnode:%p vgId:%d, read vnode version successed, version:%%" PRId64, pVnode, pVnode->vgId, pVnode->version);

PARSE_OVER:
  free(content);
  cJSON_Delete(root);
  fclose(fp);
  return ret;
}