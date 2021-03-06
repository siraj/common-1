/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef __CACHE_FILE_H__
#define __CACHE_FILE_H__

#include <unordered_map>
#include <atomic>
#include <QObject>
#include <QReadWriteLock>
#include <QWaitCondition>
#include <QMutex>
#include <QThreadPool>
#include <QTimer>
#include <lmdbpp.h>
#include "AsyncClient.h"
#include "BinaryData.h"
#include "TxClasses.h"


class CacheFile : public QObject
{
   Q_OBJECT

public:
   CacheFile(const std::string &filename, size_t nbElemLimit = 10000);
   ~CacheFile();

   void put(const BinaryData &key, const BinaryData &val);
   BinaryData get(const BinaryData &key) const;
   void stop();

protected:
   void read();
   void write();
   void saver();
   void purge();

private:
   const bool  inMem_;
   size_t      nbMaxElems_;
   LMDB     *  db_ = nullptr;
   std::shared_ptr<LMDBEnv>  dbEnv_;
   std::map<BinaryData, BinaryData> map_, mapModified_;
   mutable QReadWriteLock  rwLock_;
   mutable QWaitCondition  wcModified_;
   mutable QMutex          mtxModified_;
   std::atomic_bool        stopped_;
   QThreadPool             threadPool_;
   QTimer                  saveTimer_;
};


class TxCacheFile : protected CacheFile
{
public:
   TxCacheFile(const std::string &filename, size_t nbElemLimit = 10000)
      : CacheFile(filename, nbElemLimit) {}

   void put(const BinaryData &key, const std::shared_ptr<const Tx> &tx);
   std::shared_ptr<const Tx> get(const BinaryData &key);

   void stop() { CacheFile::stop(); }

private:
   AsyncClient::TxBatchResult txMap_;
   std::mutex txMapMutex_;
};

#endif // __CACHE_FILE_H__
