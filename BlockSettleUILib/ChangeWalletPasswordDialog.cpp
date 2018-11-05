#include "ChangeWalletPasswordDialog.h"
#include "ui_ChangeWalletPasswordDialog.h"

#include <spdlog/spdlog.h>
#include "ApplicationSettings.h"
#include "EnterWalletPassword.h"
#include "AuthNotice.h"
#include "HDWallet.h"
#include "MessageBoxCritical.h"
#include "MessageBoxSuccess.h"
#include "SignContainer.h"
#include "WalletKeyWidget.h"


ChangeWalletPasswordDialog::ChangeWalletPasswordDialog(const std::shared_ptr<spdlog::logger> &logger
      , std::shared_ptr<SignContainer> signingContainer
      , const std::shared_ptr<bs::hd::Wallet> &wallet
      , const std::vector<bs::wallet::EncryptionType> &encTypes
      , const std::vector<SecureBinaryData> &encKeys
      , bs::wallet::KeyRank keyRank
      , const QString& username
      , const std::shared_ptr<ApplicationSettings> &appSettings
      , QWidget* parent)
   : QDialog(parent)
   , logger_(logger)
   , ui_(new Ui::ChangeWalletPasswordDialog())
   , signingContainer_(signingContainer)
   , wallet_(wallet)
   , oldKeyRank_(keyRank)
   , appSettings_(appSettings)
{
   ui_->setupUi(this);

   resetKeys();

   ui_->labelWalletId->setText(QString::fromStdString(wallet->getWalletId()));

   connect(ui_->tabWidget, &QTabWidget::currentChanged, this, &ChangeWalletPasswordDialog::onTabChanged);
   connect(ui_->pushButtonCancel, &QPushButton::clicked, this, &ChangeWalletPasswordDialog::reject);
   connect(ui_->pushButtonOk, &QPushButton::clicked, this, &ChangeWalletPasswordDialog::onContinueClicked);

   connect(signingContainer_.get(), &SignContainer::PasswordChanged, this, &ChangeWalletPasswordDialog::onPasswordChanged);

   deviceKeyOld_ = new WalletKeyWidget(MobileClientRequest::ActivateWalletOldDevice
      , wallet->getWalletId(), 0, false, appSettings->GetAuthKeys(), this);
   deviceKeyOld_->setFixedType(true);
   deviceKeyOld_->setEncryptionKeys(encKeys);
   deviceKeyOld_->setHideAuthConnect(true);
   deviceKeyOld_->setHideAuthCombobox(true);

   deviceKeyNew_ = new WalletKeyWidget(MobileClientRequest::ActivateWalletNewDevice
      , wallet->getWalletId(), 0, false, appSettings->GetAuthKeys(), this);
   deviceKeyNew_->setFixedType(true);
   deviceKeyNew_->setEncryptionKeys(encKeys);
   deviceKeyNew_->setHideAuthConnect(true);
   deviceKeyNew_->setHideAuthCombobox(true);
   
   QBoxLayout *deviceLayout = dynamic_cast<QBoxLayout*>(ui_->tabAddDevice->layout());
   deviceLayout->insertWidget(deviceLayout->indexOf(ui_->labelDeviceOldInfo) + 1, deviceKeyOld_);
   deviceLayout->insertWidget(deviceLayout->indexOf(ui_->labelDeviceNewInfo) + 1, deviceKeyNew_);

   //connect(ui_->widgetSubmitKeys, &WalletKeysSubmitWidget::keyChanged, [this] { updateState(); });
   //connect(ui_->widgetCreateKeys, &WalletKeysCreateWidget::keyCountChanged, [this] { adjustSize(); });
   //connect(ui_->widgetCreateKeys, &WalletKeysCreateWidget::keyChanged, [this] { updateState(); });
   connect(deviceKeyOld_, &WalletKeyWidget::keyChanged, this, &ChangeWalletPasswordDialog::onOldDeviceKeyChanged);
   connect(deviceKeyOld_, &WalletKeyWidget::failed, this, &ChangeWalletPasswordDialog::onOldDeviceFailed);

   connect(deviceKeyNew_, &WalletKeyWidget::encKeyChanged, this, &ChangeWalletPasswordDialog::onNewDeviceEncKeyChanged);
   connect(deviceKeyNew_, &WalletKeyWidget::keyChanged, this, &ChangeWalletPasswordDialog::onNewDeviceKeyChanged);
   connect(deviceKeyNew_, &WalletKeyWidget::failed, this, &ChangeWalletPasswordDialog::onNewDeviceFailed);

   QString usernameAuthApp;

   auto encTypesIt = encTypes.begin();
   auto encKeysIt = encKeys.begin();
   while (encTypesIt != encTypes.end() && encKeysIt != encKeys.end()) {
      bs::wallet::PasswordData passwordData{};
      passwordData.encType = *encTypesIt;
      passwordData.encKey = *encKeysIt;

      if (passwordData.encType == bs::wallet::EncryptionType::Auth) {
         usernameAuthApp = QString::fromStdString(passwordData.encKey.toBinStr());
      }

      oldPasswordData_.push_back(passwordData);
      ++encTypesIt;
      ++encKeysIt;
   }

   // For some reasons encTypes contains only distinct types.
   // But we need to pass all auth ids because they contain different deviceId's
   while (encKeysIt != encKeys.end()) {
      bs::wallet::PasswordData passwordData{};
      passwordData.encType = bs::wallet::EncryptionType::Auth;
      passwordData.encKey = *encKeysIt;

      oldPasswordData_.push_back(passwordData);
      ++encKeysIt;
   }

   if (!usernameAuthApp.isEmpty()) {
      deviceKeyOld_->init(appSettings, usernameAuthApp);
      deviceKeyNew_->init(appSettings, usernameAuthApp);
   }

   ui_->widgetSubmitKeys->setFlags(WalletKeysSubmitWidget::HideGroupboxCaption 
      | WalletKeysSubmitWidget::SetPasswordLabelAsOld
      | WalletKeysSubmitWidget::HideAuthConnectButton);
   ui_->widgetSubmitKeys->suspend();
   ui_->widgetSubmitKeys->init(MobileClientRequest::DeactivateWallet, wallet_->getWalletId(), keyRank, encTypes, encKeys, appSettings);

   ui_->widgetCreateKeys->setFlags(WalletKeysCreateWidget::HideGroupboxCaption
      | WalletKeysCreateWidget::SetPasswordLabelAsNew
      | WalletKeysCreateWidget::HideAuthConnectButton
      | WalletKeysCreateWidget::HideWidgetContol);
   ui_->widgetCreateKeys->init(MobileClientRequest::ActivateWallet, wallet_->getWalletId(), username, appSettings);

   ui_->widgetSubmitKeys->setFocus();

   ui_->tabWidget->setCurrentIndex(int(Pages::Basic));

   updateState();
}

ChangeWalletPasswordDialog::~ChangeWalletPasswordDialog() = default;

void ChangeWalletPasswordDialog::accept()
{
   onContinueClicked();
}

void ChangeWalletPasswordDialog::reject()
{
   ui_->widgetSubmitKeys->cancel();
   ui_->widgetCreateKeys->cancel();
   deviceKeyOld_->cancel();
   deviceKeyNew_->cancel();

   QDialog::reject();
}

void ChangeWalletPasswordDialog::onContinueClicked()
{
   resetKeys();

   if (ui_->tabWidget->currentIndex() == int(Pages::Basic)) {
      continueBasic();
   } else {
      continueAddDevice();
   }
}

void ChangeWalletPasswordDialog::continueBasic()
{
   std::vector<bs::wallet::PasswordData> newKeys = ui_->widgetCreateKeys->keys();

   bool isOldAuth = !oldPasswordData_.empty() && oldPasswordData_[0].encType == bs::wallet::EncryptionType::Auth;
   bool isNewAuth = !newKeys.empty() && newKeys[0].encType == bs::wallet::EncryptionType::Auth;

   if (!ui_->widgetSubmitKeys->isValid() && !isOldAuth) {
      MessageBoxCritical messageBox(tr("Invalid password"), tr("Please check old password"), this);
      messageBox.exec();
      return;
   }

   if (!ui_->widgetCreateKeys->isValid() && !isNewAuth) {
      MessageBoxCritical messageBox(tr("Invalid passwords"), tr("Please check new passwords"), this);
      messageBox.exec();
      return;
   }

   if (isOldAuth && isNewAuth && oldPasswordData_[0].encKey == newKeys[0].encKey) {
      MessageBoxCritical messageBox(tr("Invalid new Auth eID")
         , tr("Please use different Auth eID. New Freje eID is already used."), this);
      messageBox.exec();
      return;
   }

   bool showAuthUsageInfo = true;

   if (isOldAuth)
   {
      showAuthUsageInfo = false;

      if (oldPasswordData_[0].password.isNull()) {
         EnterWalletPassword enterWalletPassword(MobileClientRequest::DeactivateWallet, this);
         enterWalletPassword.init(wallet_->getWalletId(), oldKeyRank_
            , oldPasswordData_, appSettings_, tr("Change Password"));
         int result = enterWalletPassword.exec();
         if (result != QDialog::Accepted) {
            return;
         }

         oldKey_ = enterWalletPassword.getPassword();
      }
   }
   else {
      oldKey_ = ui_->widgetSubmitKeys->key();
   }

   if (isNewAuth) {
      if (showAuthUsageInfo) {
         AuthNotice authNotice(this);
         int result = authNotice.exec();
         if (result != QDialog::Accepted) {
            return;
         }
      }

      EnterWalletPassword enterWalletPassword(MobileClientRequest::ActivateWallet, this);
      enterWalletPassword.init(wallet_->getWalletId(), ui_->widgetCreateKeys->keyRank()
         , newKeys, appSettings_, tr("Activate Auth eID signing"));
      int result = enterWalletPassword.exec();
      if (result != QDialog::Accepted) {
         return;
      }

      newKeys[0].encKey = enterWalletPassword.getEncKey(0);
      newKeys[0].password = enterWalletPassword.getPassword();
   }

   newPasswordData_ = newKeys;
   newKeyRank_ = ui_->widgetCreateKeys->keyRank();
   changePassword();
}

void ChangeWalletPasswordDialog::continueAddDevice()
{
   if (state_ != State::Idle) {
      return;
   }

   if (oldKeyRank_.first != 1) {
      MessageBoxCritical messageBox(tr("Add Device error")
         , tr("Only 1-of-N AuthApp encryption supported"), this);
      messageBox.exec();
      return;
   }

   if (oldPasswordData_.empty() || oldPasswordData_[0].encType != bs::wallet::EncryptionType::Auth) {
      MessageBoxCritical messageBox(tr("Add Device error")
         , tr("Please switch to Auth encryption first"), this);
      messageBox.exec();
      return;
   }

   if (!deviceKeyOldValid_) {
      deviceKeyOld_->start();
      state_ = State::AddDeviceWaitOld;
   } else {
      deviceKeyNew_->start();
      state_ = State::AddDeviceWaitNew;
   }

   updateState();
}

void ChangeWalletPasswordDialog::changePassword()
{
   if (wallet_->isWatchingOnly()) {
      signingContainer_->ChangePassword(wallet_, newPasswordData_, newKeyRank_, oldKey_
         , addNew_, false);
   }
   else {
      bool result = wallet_->changePassword(logger_, newPasswordData_, newKeyRank_, oldKey_
         , addNew_, false);
      onPasswordChanged(wallet_->getWalletId(), result);
   }
}

void ChangeWalletPasswordDialog::resetKeys()
{
   oldKey_.clear();
   deviceKeyOldValid_ = false;
   deviceKeyNewValid_ = false;
   isLatestChangeAddDevice_ = false;
   addNew_ = false;
}

void ChangeWalletPasswordDialog::onOldDeviceKeyChanged(int, SecureBinaryData password)
{
   deviceKeyOldValid_ = true;
   state_ = State::AddDeviceWaitNew;
   deviceKeyNew_->start();
   oldKey_ = password;
   updateState();
}

void ChangeWalletPasswordDialog::onOldDeviceFailed()
{
   state_ = State::Idle;
   updateState();

   MessageBoxCritical(tr("Wallet Password")
      , tr("A problem occured requesting old device key")
      , this).exec();
}

void ChangeWalletPasswordDialog::onNewDeviceEncKeyChanged(int index, SecureBinaryData encKey)
{
   bs::wallet::PasswordData newPassword{};
   newPassword.encType = bs::wallet::EncryptionType::Auth;
   newPassword.encKey = encKey;
   newPasswordData_.clear();
   newPasswordData_.push_back(newPassword);
}

void ChangeWalletPasswordDialog::onNewDeviceKeyChanged(int index, SecureBinaryData password)
{
   if (newPasswordData_.empty()) {
      logger_->error("Internal error: newPasswordData_.empty()");
      return;
   }

   newPasswordData_.back().password = password;

   newKeyRank_ = oldKeyRank_;
   newKeyRank_.second += 1;

   isLatestChangeAddDevice_ = true;
   addNew_ = true;
   changePassword();
}

void ChangeWalletPasswordDialog::onNewDeviceFailed()
{
   state_ = State::Idle;
   updateState();

   MessageBoxCritical(tr("Wallet Password")
      , tr("A problem occured requesting new device key")
      , this).exec();
}

void ChangeWalletPasswordDialog::onPasswordChanged(const string &walletId, bool ok)
{
   if (walletId != wallet_->getWalletId()) {
      logger_->error("ChangeWalletPasswordDialog::onPasswordChanged: unknown walletId {}, expected: {}"
         , walletId, wallet_->getWalletId());
      return;
   }

   if (!ok) {
      logger_->error("ChangeWalletPassword failed for {}", walletId);

      if (isLatestChangeAddDevice_) {
         MessageBoxCritical(tr("Wallet Password")
            , tr("Device adding failed")
            , this).exec();
      } else {
         MessageBoxCritical(tr("Wallet Password")
            , tr("A problem occured when changing wallet password")
            , this).exec();
      }

      state_ = State::Idle;
      updateState();
      return;
   }

   if (isLatestChangeAddDevice_) {
      MessageBoxSuccess(tr("Wallet Password")
         , tr("Device successfully added")
         , this).exec();
   } else {
      MessageBoxSuccess(tr("Wallet Password")
         , tr("Wallet password successfully changed")
         , this).exec();
   }

   QDialog::accept();
}

void ChangeWalletPasswordDialog::onTabChanged(int index)
{
   state_ = State::Idle;
   updateState();
}

void ChangeWalletPasswordDialog::updateState()
{
   Pages currentPage = Pages(ui_->tabWidget->currentIndex());

   if (currentPage == Pages::Basic) {
      ui_->pushButtonOk->setText(tr("Continue"));
   } else {
      ui_->pushButtonOk->setText(tr("Add Device"));
   }

   ui_->labelAddDeviceInfo->setVisible(state_ == State::Idle);
   ui_->labelDeviceOldInfo->setVisible(state_ == State::AddDeviceWaitOld || state_ == State::AddDeviceWaitNew);
   ui_->labelAddDeviceSuccess->setVisible(state_ == State::AddDeviceWaitNew);
   ui_->labelDeviceNewInfo->setVisible(state_ == State::AddDeviceWaitNew);
   ui_->lineAddDevice->setVisible(state_ == State::AddDeviceWaitNew);
   deviceKeyOld_->setVisible(state_ == State::AddDeviceWaitOld);
   deviceKeyNew_->setVisible(state_ == State::AddDeviceWaitNew);
}
