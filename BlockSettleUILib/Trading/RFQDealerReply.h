#ifndef __RFQ_DEALER_REPLY_H__
#define __RFQ_DEALER_REPLY_H__

#include <QTimer>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "BSErrorCode.h"
#include "CommonTypes.h"
#include "EncryptionUtils.h"
#include "QWalletInfo.h"

namespace Ui {
    class RFQDealerReply;
}
namespace spdlog {
   class logger;
}
namespace bs {
   class DealerUtxoResAdapter;
   namespace sync {
      namespace hd {
         class Leaf;
      }
      class Wallet;
      class WalletsManager;
   }
}
class ApplicationSettings;
class ArmoryConnection;
class AssetManager;
class AuthAddressManager;
class QuoteProvider;
class SelectedTransactionInputs;
class SignContainer;
class TransactionData;
class CustomDoubleSpinBox;
class MarketDataProvider;
class ConnectionManager;

QT_BEGIN_NAMESPACE
class QDoubleSpinBox;
class QPushButton;
QT_END_NAMESPACE

class UserScriptRunner;

namespace bs {
   namespace network {
      struct QuoteNotification;
   }

   namespace ui {
      class RFQDealerReply : public QWidget
      {
         Q_OBJECT

      public:
         RFQDealerReply(QWidget* parent = nullptr);
         ~RFQDealerReply() override;

         void init(const std::shared_ptr<spdlog::logger> logger
            , const std::shared_ptr<AuthAddressManager> &
            , const std::shared_ptr<AssetManager>& assetManager
            , const std::shared_ptr<QuoteProvider>& quoteProvider
            , const std::shared_ptr<ApplicationSettings> &
            , const std::shared_ptr<ConnectionManager> &connectionManager
            , const std::shared_ptr<SignContainer> &
            , const std::shared_ptr<ArmoryConnection> &
            , std::shared_ptr<MarketDataProvider> mdProvider);
         void setWalletsManager(const std::shared_ptr<bs::sync::WalletsManager> &);

         std::shared_ptr<TransactionData> getTransactionData(const std::string &reqId) const;
         bool autoSign() const;

         CustomDoubleSpinBox* bidSpinBox() const;
         CustomDoubleSpinBox* offerSpinBox() const;

         QPushButton* pullButton() const;
         QPushButton* quoteButton() const;

      signals:
         void autoSignActivated(const QString &hdWalletId, bool active);
         void submitQuoteNotif(network::QuoteNotification);
         void pullQuoteNotif(const QString &reqId, const QString &reqSessToken);

      public slots:
         void setQuoteReqNotification(const network::QuoteReqNotification &, double indicBid, double indicAsk);
         void quoteReqNotifStatusChanged(const network::QuoteReqNotification &);
         void onMDUpdate(bs::network::Asset::Type, const QString &security, bs::network::MDFields);
         void onBestQuotePrice(const QString reqId, double price, bool own);
         void onAutoSignStateChanged(const std::string &walletId, bool active);
         void onCelerConnected();
         void onCelerDisconnected();

      private slots:
         void initUi();
         void priceChanged();
         void updateSubmitButton();
         void submitButtonClicked();
         void pullButtonClicked();
         void showCoinControl();
         void aqFillHistory();
         void aqScriptChanged(int curIndex);
         void onAqScriptLoaded(const QString &filename);
         void onAqScriptFailed(const QString &filename, const QString &error);
         void walletSelected(int index);
         void onTransactionDataChanged();
         void checkBoxAQClicked();
         void onAQReply(const bs::network::QuoteReqNotification &qrn, double price);
         void onReservedUtxosChanged(const std::string &walletId, const std::vector<UTXO> &);
         void onOrderUpdated(const bs::network::Order &);
         void onHDLeafCreated(const std::string& ccName);
         void onCreateHDWalletError(const std::string& ccName, bs::error::ErrorCode result);
         void onSignerStateUpdated();
         void onAutoSignActivated();
         void onAuthAddrChanged(int);

      protected:
         bool eventFilter(QObject *watched, QEvent *evt) override;
         QString askForAQScript();

      private:
         std::unique_ptr<Ui::RFQDealerReply> ui_;
         std::shared_ptr<spdlog::logger>        logger_;
         std::shared_ptr<bs::sync::WalletsManager> walletsManager_;
         std::shared_ptr<AuthAddressManager>    authAddressManager_;
         std::shared_ptr<AssetManager>          assetManager_;
         std::shared_ptr<QuoteProvider>         quoteProvider_;
         std::shared_ptr<ApplicationSettings>   appSettings_;
         std::shared_ptr<ConnectionManager>     connectionManager_;
         std::shared_ptr<SignContainer>         signingContainer_;
         std::shared_ptr<ArmoryConnection>      armory_;
         std::shared_ptr<MarketDataProvider>    mdProvider_;

         std::shared_ptr<bs::sync::Wallet>   curWallet_;
         std::shared_ptr<bs::sync::Wallet>   prevWallet_;
         std::shared_ptr<bs::sync::Wallet>   ccWallet_;
         std::shared_ptr<bs::sync::Wallet>   xbtWallet_;
         std::string authKey_;
         bs::Address authAddr_;

         std::unordered_map<std::string, double>   sentNotifs_;
         network::QuoteReqNotification    currentQRN_;
         std::shared_ptr<TransactionData> transactionData_;
         unsigned int   payInRecipId_{UINT_MAX};
         bool dealerSellXBT_{false};
         std::shared_ptr<SelectedTransactionInputs>   ccCoinSel_;

         double   indicBid_{};
         double   indicAsk_{};
         std::atomic_bool     autoUpdatePrices_{true};

         std::string          autoSignWalletId_;

         std::string product_;
         std::string baseProduct_;

         UserScriptRunner *aq_{};

         bool           aqLoaded_{false};
         bool           celerConnected_{false};
         bool           newLoaded_{false};

         std::unordered_map<std::string, double>   bestQPrices_;

         struct MDInfo {
            double   bidPrice{};
            double   askPrice{};
            double   lastPrice{};
         };
         std::unordered_map<std::string, MDInfo>  mdInfo_;

         std::shared_ptr<DealerUtxoResAdapter>  utxoAdapter_;

      private:
         void reset();
         void validateGUI();
         void updateRespQuantity();
         void updateQuoteReqNotification(const network::QuoteReqNotification &);
         double getPrice() const;
         double getValue() const;
         double getAmount() const;
         std::shared_ptr<bs::sync::Wallet> getCurrentWallet() const { return curWallet_; }
         void setCurrentWallet(const std::shared_ptr<bs::sync::Wallet> &);
         std::shared_ptr<bs::sync::Wallet> getCCWallet(const std::string &cc);
         std::shared_ptr<bs::sync::Wallet> getXbtWallet();
         bs::Address getRecvAddress() const;
         void initAQ(const QString &filename);
         void deinitAQ();
         void setBalanceOk(bool ok);
         void updateRecvAddresses();
         bool checkBalance() const;
         QDoubleSpinBox *getActivePriceWidget() const;
         void updateUiWalletFor(const bs::network::QuoteReqNotification &qrn);
         bool submitReply(const std::shared_ptr<TransactionData> transData
            , const network::QuoteReqNotification &qrn, double price
            , std::function<void(bs::network::QuoteNotification)>);
         void tryEnableAutoSign();
         void disableAutoSign();
         void updateAutoSignState();
      };

   }  //namespace ui
}  //namespace bs

#endif // __RFQ_TICKET_XBT_H__