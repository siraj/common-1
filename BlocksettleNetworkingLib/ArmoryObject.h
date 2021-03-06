/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef ARMORY_OBJECT_H
#define ARMORY_OBJECT_H

#include <QProcess>
#include "ArmoryConnection.h"
#include "ArmorySettings.h"
#include "CacheFile.h"


class ArmoryObject : public QObject, public ArmoryConnection
{
   Q_OBJECT
public:
   ArmoryObject(const std::shared_ptr<spdlog::logger> &, const std::string &txCacheFN
      , bool cbInMainThread = true);
   ~ArmoryObject() noexcept override = default;

   void setupConnection(const ArmorySettings &settings
      , const BIP151Cb &bip150PromptUserCb = [](const BinaryData&, const std::string&) { return true; });

   bool getWalletsHistory(const std::vector<std::string> &walletIDs, const WalletsHistoryCb &) override;

   bool getWalletsLedgerDelegate(const LedgerDelegateCb &) override;

   bool getTxByHash(const BinaryData &hash, const TxCb &, bool allowCachedResult = true) override;
   bool getTXsByHash(const std::set<BinaryData> &hashes, const TXsCb &, bool allowCachedResult = true) override;
   bool getRawHeaderForTxHash(const BinaryData& inHash, const BinaryDataCb &) override;
   bool getHeaderByHeight(const unsigned int inHeight, const BinaryDataCb &) override;

   bool estimateFee(unsigned int nbBlocks, const FloatCb &) override;
   bool getFeeSchedule(const FloatMapCb &) override;

private:
   bool startLocalArmoryProcess(const ArmorySettings &);

   bool needInvokeCb() const;

   std::shared_ptr<const Tx> getFromCache(const BinaryData &hash);
   // Will store only transactions with >= 6 confirmations
   void putToCacheIfNeeded(const BinaryData &hash, const std::shared_ptr<const Tx> &tx);

private:
   const bool     cbInMainThread_;
   TxCacheFile    txCache_;
   std::shared_ptr<QProcess>  armoryProcess_;
};

#endif // ARMORY_OBJECT_H
