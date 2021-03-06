/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef __CC_FILE_MANAGER_H__
#define __CC_FILE_MANAGER_H__

#include "CCPubConnection.h"

#include <functional>
#include <memory>
#include <vector>

#include <QString>
#include <QVariant>
#include <QPointer>

#include "BsClient.h"
#include "Wallets/SyncWallet.h"

namespace Blocksettle {
   namespace Communication {
      class GetCCGenesisAddressesResponse;
   }
}

class ApplicationSettings;
class BaseCelerClient;

class CCPubResolver : public bs::sync::CCDataResolver
{
public:
   using CCSecLoadedCb = std::function<void(const bs::network::CCSecurityDef &)>;
   using CCLoadCompleteCb = std::function<void(unsigned int)>;
   CCPubResolver(const std::shared_ptr<spdlog::logger> &logger
      , const SecureBinaryData &bsPubKey, const CCSecLoadedCb &cbSec
      , const CCLoadCompleteCb &cbLoad)
      : logger_(logger), pubKey_(bsPubKey), cbSecLoaded_(cbSec)
      , cbLoadComplete_(cbLoad) {}

   std::string nameByWalletIndex(bs::hd::Path::Elem) const override;
   uint64_t lotSizeFor(const std::string &cc) const override;
   bs::Address genesisAddrFor(const std::string &cc) const override;
   std::string descriptionFor(const std::string &cc) const override;
   std::vector<std::string> securities() const override;

   void fillFrom(Blocksettle::Communication::GetCCGenesisAddressesResponse *resp);
   bool loadFromFile(const std::string &path, NetworkType netType);
   bool saveToFile(const std::string &path, const std::string &response
      , const std::string &signature);

   bool verifySignature(const std::string& data, const std::string& signature) const;
private:
   void add(const bs::network::CCSecurityDef &);
   void clear();

private:
   std::shared_ptr<spdlog::logger>  logger_;
   const SecureBinaryData           pubKey_;
   std::map<std::string, bs::network::CCSecurityDef>  securities_;
   std::map<bs::hd::Path::Elem, std::string>          walletIdxMap_;
   const CCSecLoadedCb     cbSecLoaded_;
   const CCLoadCompleteCb  cbLoadComplete_;
};

class CCFileManager : public CCPubConnection
{
Q_OBJECT
public:
   CCFileManager(const std::shared_ptr<spdlog::logger> &logger, const std::shared_ptr<ApplicationSettings> &appSettings);
   ~CCFileManager() noexcept override = default;

   CCFileManager(const CCFileManager&) = delete;
   CCFileManager& operator = (const CCFileManager&) = delete;
   CCFileManager(CCFileManager&&) = delete;
   CCFileManager& operator = (CCFileManager&&) = delete;

   std::shared_ptr<bs::sync::CCDataResolver> getResolver() const { return resolver_; }

   void LoadSavedCCDefinitions();
   void ConnectToCelerClient(const std::shared_ptr<BaseCelerClient> &);

   bool submitAddress(const bs::Address &, uint32_t seed, const std::string &ccProduct);
   bool wasAddressSubmitted(const bs::Address &);
   void cancelActiveSign();

   bool hasLocalFile() const;

   void setBsClient(BsClient *bsClient);

   void setCcAddressesSigned(const BinaryData &data);

signals:
   void CCSecurityDef(bs::network::CCSecurityDef);
   void CCSecurityId(const std::string& securityId);
   void CCSecurityInfo(QString ccProd, QString ccDesc, unsigned long nbSatoshis, QString genesisAddr);

   void CCAddressSubmitted(const QString);
   void CCInitialSubmitted(const QString);
   void CCSubmitFailed(const QString address, const QString &err);
   void Loaded();
   void LoadingFailed();
   void definitionsLoadedFromPub();

protected:
   void ProcessGenAddressesResponse(const std::string& response, const std::string &sig) override;

   bool IsTestNet() const override;

private:
   std::shared_ptr<ApplicationSettings>   appSettings_;
   std::shared_ptr<BaseCelerClient>       celerClient_;

   std::shared_ptr<CCPubResolver>   resolver_;
   QPointer<BsClient> bsClient_;
   QString ccFilePath_;
};

#endif // __CC_FILE_MANAGER_H__
