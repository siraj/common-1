#ifndef BS_SYNC_WALLETS_MANAGER_H
#define BS_SYNC_WALLETS_MANAGER_H

#include <memory>
#include <vector>
#include <set>
#include <unordered_map>
#include <QString>
#include <QObject>
#include <QPointer>
#include <QMutex>
#include <QDateTime>
#include "ArmoryConnection.h"
#include "BTCNumericTypes.h"
#include "CoreWallet.h"
#include "SyncWallet.h"


namespace spdlog {
   class logger;
}
class ApplicationSettings;
class SignContainer;

namespace bs {
   namespace hd {
      class Wallet;
   }
   namespace sync {
      namespace hd {
         class Wallet;
         class DummyWallet;
      }
      class Wallet;
      class SettlementWallet;

      class WalletsManager : public QObject, public ArmoryCallbackTarget
      {
         Q_OBJECT
      public:
         using CbProgress = std::function<void(int, int)>;
         using WalletPtr = std::shared_ptr<Wallet>;     // Generic wallet interface
         using HDWalletPtr = std::shared_ptr<hd::Wallet>;

         WalletsManager(const std::shared_ptr<spdlog::logger> &, const std::shared_ptr<ApplicationSettings>& appSettings
            , const std::shared_ptr<ArmoryConnection> &);
         ~WalletsManager() noexcept;

         WalletsManager(const WalletsManager&) = delete;
         WalletsManager& operator = (const WalletsManager&) = delete;
         WalletsManager(WalletsManager&&) = delete;
         WalletsManager& operator = (WalletsManager&&) = delete;

         void setSignContainer(const std::shared_ptr<SignContainer> &container);
         void reset();

         void syncWallets(const CbProgress &cb = nullptr);

         bool isWalletsReady() const;

         size_t walletsCount() const { return wallets_.size(); }
         bool hasPrimaryWallet() const;
         bool hasSettlementWallet() const { return (settlementWallet_ != nullptr); }
         HDWalletPtr getPrimaryWallet() const;
         std::shared_ptr<hd::DummyWallet> getDummyWallet() const { return hdDummyWallet_; }
         std::vector<WalletPtr> getAllWallets() const;
         WalletPtr getWalletById(const std::string& walletId) const;
         WalletPtr getWalletByAddress(const bs::Address &addr) const;
         WalletPtr getDefaultWallet() const;
         WalletPtr getCCWallet(const std::string &cc);

         const std::shared_ptr<SettlementWallet> getSettlementWallet() const { return settlementWallet_; }
         const WalletPtr getAuthWallet() const { return authAddressWallet_; }

         size_t hdWalletsCount() const { return hdWalletsId_.size(); }
         const HDWalletPtr getHDWallet(unsigned) const;
         const HDWalletPtr getHDWalletById(const std::string &walletId) const;
         const HDWalletPtr getHDRootForLeaf(const std::string &walletId) const;
         bool walletNameExists(const std::string &walletName) const;
         bool isWatchingOnly(const std::string &walletId) const;

         bool deleteWallet(const WalletPtr &);
         bool deleteWallet(const HDWalletPtr &);

         void setUserId(const BinaryData &userId);

         bool isArmoryReady() const;
         bool isReadyForTrading() const;

         BTCNumericTypes::balance_type getSpendableBalance() const;
         BTCNumericTypes::balance_type getUnconfirmedBalance() const;
         BTCNumericTypes::balance_type getTotalBalance() const;

         void registerWallets();
         void unregisterWallets();

         bool getTransactionDirection(Tx, const std::string &walletId
            , const std::function<void(Transaction::Direction, std::vector<bs::Address> inAddrs)> &);
         bool getTransactionMainAddress(const Tx &, const std::string &walletId
            , bool isReceiving, const std::function<void(QString, int)> &);

         void createWallet(const std::string &name, const std::string &desc, bs::core::wallet::Seed
            , bool primary = false, const std::vector<bs::wallet::PasswordData> &pwdData = {}
            , bs::wallet::KeyRank keyRank = { 0, 0 });
         bool createSettlementWallet() { return true; }
         void adoptNewWallet(const HDWalletPtr &);

         bool estimatedFeePerByte(const unsigned int blocksToWait, std::function<void(float)>, QObject *obj = nullptr);
         bool getFeeSchedule(const std::function<void(const std::map<unsigned int, float> &)> &);

         //run after registration to update address chain usage counters
         void trackAddressChainUse(std::function<void(bool)>);

      signals:
         void walletChanged();
         void walletDeleted(std::string walletId);
         void walletCreated(HDWalletPtr);
         void walletsReady();
         void walletsSynchronized();
         void walletBalanceUpdated(std::string walletId);
         void walletBalanceChanged(std::string walletId);
         void walletReady(const QString &walletId);
         void newWalletAdded(const std::string &walletId);
         void authWalletChanged();
         void blockchainEvent();
         void info(const QString &title, const QString &text) const;
         void error(const QString &title, const QString &text) const;
         void walletImportStarted(const std::string &walletId);
         void walletImportFinished(const std::string &walletId);
         void newTransactions(std::vector<bs::TXEntry>) const;
         void invalidatedZCs(std::vector<bs::TXEntry>) const;

      public slots:
         void onCCSecurityInfo(QString ccProd, QString ccDesc, unsigned long nbSatoshis, QString genesisAddr);
         void onCCInfoLoaded();

      private:
         void onZCReceived(const std::vector<bs::TXEntry> &) override;
         void onZCInvalidated(const std::vector<bs::TXEntry> &) override;
         void onTxBroadcastError(const std::string &txHash, const std::string &errMsg) override;
         void onRefresh(const std::vector<BinaryData> &ids, bool online) override;
         void onNewBlock(unsigned int) override;
         void onStateChanged(ArmoryState) override;

      private slots:
         void onWalletReady(const QString &walletId);
         void onHDLeafAdded(QString id);
         void onHDLeafDeleted(QString id);
         void onWalletImported(const std::string &walletId);
         void onHDWalletCreated(unsigned int id, std::shared_ptr<bs::sync::hd::Wallet>);
         void onWalletsListUpdated();

      private:
         bool empty() const { return (wallets_.empty() && !settlementWallet_); }

         void registerSettlementWallet();

         void addWallet(const WalletPtr &, bool isHDLeaf = false);
         void addWallet(const HDWalletPtr &);
         void saveWallet(const WalletPtr &);
         void saveWallet(const HDWalletPtr &);
         void eraseWallet(const WalletPtr &);
         bool setAuthWalletFrom(const HDWalletPtr &);
         void setSettlementWallet(const std::shared_ptr<bs::sync::SettlementWallet> &);

         void updateTxDirCache(const std::string &txKey, Transaction::Direction
            , const std::vector<bs::Address> &inAddrs
            , std::function<void(Transaction::Direction, std::vector<bs::Address>)>);
         void updateTxDescCache(const std::string &txKey, const QString &, int
            , std::function<void(QString, int)>);

         void invokeFeeCallbacks(unsigned int blocks, float fee);

         BTCNumericTypes::balance_type getBalanceSum(
            const std::function<BTCNumericTypes::balance_type(const WalletPtr &)> &) const;

         void startWalletRescan(const HDWalletPtr &);

      private:
         std::shared_ptr<SignContainer>         signContainer_;
         std::shared_ptr<spdlog::logger>        logger_;
         std::shared_ptr<ApplicationSettings>   appSettings_;
         std::shared_ptr<ArmoryConnection>      armoryPtr_;

         using wallet_container_type = std::unordered_map<std::string, WalletPtr>;
         using hd_wallet_container_type = std::unordered_map<std::string, HDWalletPtr>;

         hd_wallet_container_type            hdWallets_;
         std::shared_ptr<hd::DummyWallet>    hdDummyWallet_;
         std::unordered_set<std::string>     walletNames_;
         wallet_container_type               wallets_;
         mutable QMutex                      mtxWallets_;
         std::set<QString>                   readyWallets_;
         std::set<BinaryData>                walletsId_;
         std::set<std::string>               hdWalletsId_;
         WalletPtr                           authAddressWallet_;
         BinaryData                          userId_;
         std::shared_ptr<SettlementWallet>   settlementWallet_;
         std::set<std::string>               newWallets_;

         struct CCInfo {
            std::string desc;
            uint64_t    lotSize;
            std::string genesisAddr;
         };
         std::unordered_map<std::string, CCInfo>   ccSecurities_;

         std::unordered_map<std::string, std::pair<Transaction::Direction, std::vector<bs::Address>>> txDirections_;
         mutable std::atomic_flag      txDirLock_ = ATOMIC_FLAG_INIT;
         std::unordered_map<std::string, std::pair<QString, int>> txDesc_;
         mutable std::atomic_flag      txDescLock_ = ATOMIC_FLAG_INIT;

         mutable std::map<unsigned int, float>     feePerByte_;
         mutable std::map<unsigned int, QDateTime> lastFeePerByte_;
         std::map<QObject *, std::map<unsigned int
            , std::pair<QPointer<QObject>, std::function<void(float)>>>> feeCallbacks_;

         unsigned int createHdReqId_ = 0;

         std::atomic_bool  synchronized_{ false };
      };

   }  //namespace sync
}  //namespace bs

#endif // BS_SYNC_WALLETS_MANAGER_H
