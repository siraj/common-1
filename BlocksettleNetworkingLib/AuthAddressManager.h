/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef __AUTH_ADDRESS_MANAGER_H__
#define __AUTH_ADDRESS_MANAGER_H__

#include "AuthAddress.h"

#include <atomic>
#include <memory>
#include <set>
#include <unordered_set>
#include <vector>
#include <QObject>
#include <QThreadPool>

#include "ArmoryConnection.h"
#include "AutheIDClient.h"
#include "BSErrorCode.h"
#include "BSErrorCodeStrings.h"
#include "CommonTypes.h"
#include "WalletEncryption.h"
#include "ZMQ_BIP15X_Helpers.h"

#include "bs_communication.pb.h"

namespace spdlog {
   class logger;
}
namespace bs {
   namespace sync {
      namespace hd {
         class Leaf;
         class SettlementLeaf;
      }
      class Wallet;
      class WalletsManager;
   }
}
class AddressVerificator;
class ApplicationSettings;
class ArmoryConnection;
class BsClient;
class BaseCelerClient;
class RequestReplyCommand;
class ResolverFeed_AuthAddress;
class SignContainer;


class AuthAddressManager : public QObject, public ArmoryCallbackTarget
{
   Q_OBJECT

public:
   enum class ReadyError
   {
      NoError,
      MissingAuthAddr,
      MissingAddressList,
      MissingArmoryPtr,
      ArmoryOffline,
   };

   AuthAddressManager(const std::shared_ptr<spdlog::logger> &
      , const std::shared_ptr<ArmoryConnection> &);
   ~AuthAddressManager() noexcept override;

   AuthAddressManager(const AuthAddressManager&) = delete;
   AuthAddressManager& operator = (const AuthAddressManager&) = delete;
   AuthAddressManager(AuthAddressManager&&) = delete;
   AuthAddressManager& operator = (AuthAddressManager&&) = delete;

   void init(const std::shared_ptr<ApplicationSettings> &
      , const std::shared_ptr<bs::sync::WalletsManager> &
      , const std::shared_ptr<SignContainer> &);
   void setCelerClient(const std::shared_ptr<BaseCelerClient> &);

   size_t GetAddressCount();
   bs::Address GetAddress(size_t index);
//   virtual BinaryData GetPublicKey(size_t index);

   AddressVerificationState GetState(const bs::Address &addr) const;
   void SetState(const bs::Address &addr, AddressVerificationState state);

   void setDefault(const bs::Address &addr);
   const bs::Address &getDefault() const { return defaultAddr_; }
   virtual size_t getDefaultIndex() const;

   virtual bool HaveAuthWallet() const;
   virtual bool HasAuthAddr() const;

   void createAuthWallet(const std::function<void()> &);
   virtual bool CreateNewAuthAddress();

   bool hasSettlementLeaf(const bs::Address &) const;
   void createSettlementLeaf(const bs::Address &, const std::function<void()> &);

   virtual void SubmitForVerification(BsClient *bsClient, const bs::Address &address);
   virtual void ConfirmSubmitForVerification(BsClient *bsClient, const bs::Address &address);

   virtual bool RevokeAddress(const bs::Address &address);

   virtual ReadyError readyError() const;

   virtual void OnDisconnectedFromCeler();

   virtual std::vector<bs::Address> GetVerifiedAddressList() const;
   bool isAtLeastOneAwaitingVerification() const;
   bool isAllLoadded() const;
   size_t FromVerifiedIndex(size_t index) const;
   const std::unordered_set<std::string> &GetBSAddresses() const;

   void setAuthAddressesSigned(const BinaryData &data);

   static std::string readyErrorStr(ReadyError error);

private slots:
   void tryVerifyWalletAddresses();
   void onAuthWalletChanged();
   void onWalletChanged(const std::string &walletId);
   void onTXSigned(unsigned int id, BinaryData signedTX, bs::error::ErrorCode result, const std::string &errorReason);

   void onWalletCreated();

signals:
   void AddressListUpdated();
   void VerifiedAddressListUpdated();
   void AddrVerifiedOrRevoked(const QString &addr, const QString &state);
   void AddrStateChanged();
   void AuthWalletChanged();
   void AuthWalletCreated(const QString &walletId);
   void ConnectionComplete();
   void Error(const QString &errorText) const;
   void Info(const QString &info);
   void AuthAddrSubmitError(const QString &address, const QString &error);
   void AuthConfirmSubmitError(const QString &address, const QString &error);
   void AuthAddrSubmitSuccess(const QString &address);
   void AuthAddressSubmitCancelled(const QString &address);
   void AuthRevokeTxSent();
   void gotBsAddressList();
   void AuthAddressConfirmationRequired(float validationAmount);

private:
   void SetAuthWallet();
   void ClearAddressList();
   bool setup();
   void OnDataReceived(const std::string& data);

   void ProcessBSAddressListResponse(const std::string& response, bool sigVerified);

   bool HaveBSAddressList() const;

   void VerifyWalletAddressesFunction();
   bool WalletAddressesLoaded();
   void AddAddress(const bs::Address &addr);

   template <typename TVal> TVal lookup(const bs::Address &key, const std::map<bs::Address, TVal> &container) const;

   void SubmitToCeler(const bs::Address &);
   bool BroadcastTransaction(const BinaryData& transactionData);
   void SetBSAddressList(const std::unordered_set<std::string>& bsAddressList);

   // From ArmoryCallbackTarget
   void onStateChanged(ArmoryState) override;

   void markAsSubmitted(const bs::Address &address);

protected:
   std::shared_ptr<spdlog::logger>        logger_;
   std::shared_ptr<ArmoryConnection>      armory_;
   std::shared_ptr<ApplicationSettings>   settings_;
   std::shared_ptr<bs::sync::WalletsManager> walletsManager_;
   std::shared_ptr<BaseCelerClient>       celerClient_;
   std::shared_ptr<AddressVerificator>    addressVerificator_;

   mutable std::atomic_flag                  lockList_ = ATOMIC_FLAG_INIT;
   std::vector<bs::Address>                  addresses_;

   std::map<bs::Address, AddressVerificationState> states_;
   mutable std::atomic_flag                        statesLock_ = ATOMIC_FLAG_INIT;

   using HashMap = std::map<bs::Address, BinaryData>;
   bs::Address                               defaultAddr_;

   std::unordered_set<std::string>           bsAddressList_;
   std::shared_ptr<bs::sync::Wallet>         authWallet_;

   std::shared_ptr<SignContainer>      signingContainer_;
   std::unordered_set<unsigned int>    signIdsRevoke_;

};

#endif // __AUTH_ADDRESS_MANAGER_H__
