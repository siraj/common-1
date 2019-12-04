/*

***********************************************************************************
* Copyright (C) 2016 - 2019, BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#include "HeadlessContainer.h"

#include "BSErrorCodeStrings.h"
#include "ConnectionManager.h"
#include "ProtobufHeadlessUtils.h"
#include "Wallets/SyncHDWallet.h"
#include "Wallets/SyncWalletsManager.h"
#include "SystemFileUtils.h"
#include "ZMQ_BIP15X_DataConnection.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <spdlog/spdlog.h>
#include "signer.pb.h"

namespace {

   constexpr int kKillTimeout = 5000;
   constexpr int kStartTimeout = 5000;

   // When remote signer will try to reconnect
   constexpr auto kLocalReconnectPeriod = std::chrono::seconds(10);
   constexpr auto kRemoteReconnectPeriod = std::chrono::seconds(1);

} // namespace

using namespace Blocksettle::Communication;
using namespace bs::sync;

Q_DECLARE_METATYPE(headless::RequestPacket)
Q_DECLARE_METATYPE(std::shared_ptr<bs::sync::hd::Leaf>)

NetworkType HeadlessContainer::mapNetworkType(headless::NetworkType netType)
{
   switch (netType) {
   case headless::MainNetType:   return NetworkType::MainNet;
   case headless::TestNetType:   return NetworkType::TestNet;
   default:                      return NetworkType::Invalid;
   }
}

void HeadlessListener::OnDataReceived(const std::string& data)
{
   headless::RequestPacket packet;
   if (!packet.ParseFromString(data)) {
      logger_->error("[HeadlessListener] failed to parse request packet");
      return;
   }

   if (packet.id() > id_) {
      logger_->error("[HeadlessListener] reply id inconsistency: {} > {}", packet.id(), id_);
      tryEmitError(HeadlessContainer::InvalidProtocol, tr("reply id inconsistency"));
      return;
   }

   if (packet.type() == headless::DisconnectionRequestType) {
      processDisconnectNotification();
      return;
   }

   if (packet.type() == headless::AuthenticationRequestType) {
      headless::AuthenticationReply response;
      if (!response.ParseFromString(packet.data())) {
         logger_->error("[HeadlessListener] failed to parse auth reply");

         tryEmitError(HeadlessContainer::SerializationFailed, tr("failed to parse auth reply"));
         return;
      }

      if (HeadlessContainer::mapNetworkType(response.nettype()) != netType_) {
         logger_->error("[HeadlessListener] network type mismatch");
         tryEmitError(HeadlessContainer::NetworkTypeMismatch, tr("Network type mismatch (Mainnet / Testnet)"));
         return;
      }

      // BIP 150/151 should be be complete by this point.
      isReady_ = true;
      emit authenticated();
   } else {
      emit PacketReceived(packet);
   }
}

void HeadlessListener::OnConnected()
{
   if (isConnected_) {
      logger_->error("already connected");
      return;
   }

   isConnected_ = true;
   logger_->debug("[HeadlessListener] Connected");
   emit connected();
}

void HeadlessListener::OnDisconnected()
{
   if (isShuttingDown_) {
      return;
   }

   SPDLOG_LOGGER_ERROR(logger_, "remote signer disconnected unexpectedly");
   isConnected_ = false;
   isReady_ = false;
   tryEmitError(HeadlessContainer::SocketFailed, tr("TCP connection was closed unexpectedly"));
}

void HeadlessListener::OnError(DataConnectionListener::DataConnectionError errorCode)
{
   logger_->debug("[HeadlessListener] error {}", errorCode);
   isConnected_ = false;
   isReady_ = false;

   switch (errorCode) {
      case NoError:
         assert(false);
         break;
      case UndefinedSocketError:
         tryEmitError(HeadlessContainer::SocketFailed, tr("Socket error"));
         break;
      case HostNotFoundError:
         tryEmitError(HeadlessContainer::HostNotFound, tr("Host not found"));
         break;
      case HandshakeFailed:
         tryEmitError(HeadlessContainer::HandshakeFailed, tr("Handshake failed"));
         break;
      case SerializationFailed:
         tryEmitError(HeadlessContainer::SerializationFailed, tr("Serialization failed"));
         break;
      case HeartbeatWaitFailed:
         tryEmitError(HeadlessContainer::HeartbeatWaitFailed, tr("Connection lost"));
         break;
      case ConnectionTimeout:
         tryEmitError(HeadlessContainer::ConnectionTimeout, tr("Connection timeout"));
         break;
   }
}

bs::signer::RequestId HeadlessListener::Send(headless::RequestPacket packet, bool updateId)
{
   if (!connection_) {
      return 0;
   }

   bs::signer::RequestId id = 0;
   if (updateId) {
      id = newRequestId();
      packet.set_id(id);
   }

   if (!connection_->send(packet.SerializeAsString())) {
      logger_->error("[HeadlessListener] Failed to send request packet");
      emit disconnected();
      return 0;
   }
   return id;
}

HeadlessContainer::HeadlessContainer(const std::shared_ptr<spdlog::logger> &logger, OpMode opMode)
   : WalletSignerContainer(logger, opMode)
{
   qRegisterMetaType<headless::RequestPacket>();
   qRegisterMetaType<std::shared_ptr<bs::sync::hd::Leaf>>();
}

bs::signer::RequestId HeadlessContainer::Send(const headless::RequestPacket &packet, bool incSeqNo)
{
   if (!listener_) {
      return 0;
   }
   return listener_->Send(packet, incSeqNo);
}

void HeadlessContainer::ProcessSignTXResponse(unsigned int id, const std::string &data)
{
   headless::SignTxReply response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSignTXResponse] Failed to parse SignTxReply");
      emit TXSigned(id, {}, bs::error::ErrorCode::FailedToParse);
      return;
   }
   emit TXSigned(id, response.signedtx(), static_cast<bs::error::ErrorCode>(response.errorcode()));
}

void HeadlessContainer::ProcessSettlementSignTXResponse(unsigned int id, const std::string &data)
{
   headless::SignTxReply response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSettlementSignTXResponse] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbSettlementSignTxMap_.find(id);
   if (itCb != cbSettlementSignTxMap_.end()) {
      if (itCb->second) {
         itCb->second(static_cast<bs::error::ErrorCode>(response.errorcode()), BinaryData(response.signedtx()));
      }
      cbSettlementSignTxMap_.erase(itCb);
   }
   emit TXSigned(id, response.signedtx(), static_cast<bs::error::ErrorCode>(response.errorcode()));
}

void HeadlessContainer::ProcessCreateHDLeafResponse(unsigned int id, const std::string &data)
{
   headless::CreateHDLeafResponse response;

   auto it = cbCCreateLeafMap_.find(id);
   WalletSignerContainer::CreateHDLeafCb cb = nullptr;

   if (it != cbCCreateLeafMap_.end()) {
      cb = it->second;
      cbCCreateLeafMap_.erase(it);
   } else {
      logger_->debug("[HeadlessContainer::ProcessCreateHDLeafResponse] no CB for create leaf response");
   }

   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessCreateHDLeafResponse] Failed to parse CreateHDWallet reply");

      if (cb) {
         cb(bs::error::ErrorCode::FailedToParse, {});
      }

      return;
   }

   bs::error::ErrorCode result = static_cast<bs::error::ErrorCode>(response.errorcode());

   if (result == bs::error::ErrorCode::NoError) {
      const auto path = bs::hd::Path::fromString(response.leaf().path());
      logger_->debug("[HeadlessContainer::ProcessCreateHDLeafResponse] HDLeaf {} created", response.leaf().path());
   } else {
      logger_->error("[HeadlessContainer::ProcessCreateHDLeafResponse] failed to create leaf: {}"
                     , response.errorcode());
   }

   if (cb) {
      cb(result, response.leaf().walletid());
   }
}

void HeadlessContainer::ProcessHDWalletPromotionResponse(unsigned int id, const std::string& data)
{
   headless::PromoteHDWalletResponse response;

   auto it = cbPromoteHDWalletMap_.find(id);
   WalletSignerContainer::PromoteHDWalletCb cb = nullptr;

   if (it != cbPromoteHDWalletMap_.end()) {
      cb = it->second;
      cbPromoteHDWalletMap_.erase(it);
   } else {
      logger_->debug("[HeadlessContainer::ProcessHDWalletPromotionResponse] no CB for promote HD Wallet response");
   }

   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessHDWalletPromotionResponse] Failed to parse PromoteHDWallet reply");

      if (cb) {
         cb(bs::error::ErrorCode::FailedToParse, {});
      }

      return;
   }

   bs::error::ErrorCode result = static_cast<bs::error::ErrorCode>(response.errorcode());

   if (result == bs::error::ErrorCode::NoError) {
      logger_->debug("[HeadlessContainer::ProcessHDWalletPromotionResponse] HDWallet {} promoted", response.rootwalletid());
   } else {
      logger_->error("[HeadlessContainer::ProcessHDWalletPromotionResponse] failed to create leaf: {}"
                     , response.errorcode());
   }

   if (cb) {
      cb(result, response.rootwalletid());
   }
}

void HeadlessContainer::ProcessGetHDWalletInfoResponse(unsigned int id, const std::string &data)
{
   headless::GetHDWalletInfoResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessGetHDWalletInfoResponse] Failed to parse GetHDWalletInfo reply");
      emit Error(id, "failed to parse");
      return;
   }
   if (response.error().empty()) {
      emit QWalletInfo(id, bs::hd::WalletInfo(response));
   }
   else {
      missingWallets_.insert(response.rootwalletid());
      emit Error(id, response.error());
   }
}

void HeadlessContainer::ProcessAutoSignActEvent(unsigned int id, const std::string &data)
{
   headless::AutoSignActEvent event;
   if (!event.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessAutoSignActEvent] Failed to parse SetLimits reply");
      emit Error(id, "failed to parse");
      return;
   }
   emit AutoSignStateChanged(static_cast<bs::error::ErrorCode>(event.errorcode()), event.rootwalletid());
}

void HeadlessContainer::ProcessSetUserId(const std::string &data)
{
   headless::SetUserIdResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSetUserId] failed to parse response");
      return;
   }
   if (!response.auth_wallet_id().empty() && (response.response() == headless::AWR_NoError)) {
      emit AuthLeafAdded(response.auth_wallet_id());
   }
   else {   // unset auth wallet
      emit AuthLeafAdded("");
   }
}

bs::signer::RequestId HeadlessContainer::signTXRequest(const bs::core::wallet::TXSignRequest &txSignReq
   , SignContainer::TXSignMode mode, bool keepDuplicatedRecipients)
{
   if (!txSignReq.isValid()) {
      logger_->error("[HeadlessContainer::signTXRequest] Invalid TXSignRequest");
      return 0;
   }

   headless::SignTxRequest request = bs::signer::coreTxRequestToPb(txSignReq, keepDuplicatedRecipients);

   headless::RequestPacket packet;
   switch (mode) {
   case TXSignMode::Full:
      packet.set_type(headless::SignTxRequestType);
      break;

   case TXSignMode::Partial:
      packet.set_type(headless::SignPartialTXRequestType);
      break;
   }
   packet.set_data(request.SerializeAsString());
   const auto id = Send(packet);
   signRequests_.insert(id);
   return id;
}

bs::signer::RequestId HeadlessContainer::signSettlementTXRequest(const bs::core::wallet::TXSignRequest &txSignReq
   , const bs::sync::PasswordDialogData &dialogData, SignContainer::TXSignMode mode
   , bool keepDuplicatedRecipients
   , const SignTxCb &cb)
{
   if (!txSignReq.isValid()) {
      logger_->error("[HeadlessContainer::signSettlementTXRequest] Invalid TXSignRequest");
      return 0;
   }

   headless::SignTxRequest signTxRequest = bs::signer::coreTxRequestToPb(txSignReq, keepDuplicatedRecipients);

   headless::SignSettlementTxRequest settlementRequest;
   *(settlementRequest.mutable_signtxrequest()) = signTxRequest;
   *(settlementRequest.mutable_passworddialogdata()) = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::SignSettlementTxRequestType);

   packet.set_data(settlementRequest.SerializeAsString());
   const auto reqId = Send(packet);
   cbSettlementSignTxMap_[reqId] = cb;
   return reqId;
}

bs::signer::RequestId HeadlessContainer::signSettlementPartialTXRequest(
   const bs::core::wallet::TXSignRequest &txSignReq
   , const bs::sync::PasswordDialogData &dialogData
   , const SignTxCb &cb)
{
   if (!txSignReq.isValid()) {
      logger_->error("[HeadlessContainer::signSettlementPartialTXRequest] Invalid TXSignRequest");
      return 0;
   }

   headless::SignTxRequest signTxRequest = bs::signer::coreTxRequestToPb(txSignReq);

   headless::SignSettlementTxRequest settlementRequest;
   *(settlementRequest.mutable_signtxrequest()) = signTxRequest;
   *(settlementRequest.mutable_passworddialogdata()) = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::SignSettlementPartialTxType);
   packet.set_data(settlementRequest.SerializeAsString());

   const auto reqId = Send(packet);
   cbSettlementSignTxMap_[reqId] = cb;
   return reqId;
}

static void fillSettlementData(headless::SettlementData *settlData, const bs::core::wallet::SettlementData &sd)
{
   settlData->set_settlement_id(sd.settlementId.toBinStr());
   settlData->set_counterparty_pubkey(sd.cpPublicKey.toBinStr());
   settlData->set_my_pubkey_first(sd.ownKeyFirst);
}

bs::signer::RequestId HeadlessContainer::signSettlementPayoutTXRequest(const bs::core::wallet::TXSignRequest &txSignReq
   , const bs::core::wallet::SettlementData &sd, const bs::sync::PasswordDialogData &dialogData
   , const SignTxCb &cb)
{
   if ((txSignReq.inputs.size() != 1) || (txSignReq.recipients.size() != 1) || sd.settlementId.isNull()) {
      logger_->error("[HeadlessContainer::signSettlementPayoutTXRequest] Invalid PayoutTXSignRequest");
      return 0;
   }
   headless::SignSettlementPayoutTxRequest settlementRequest;
   auto request = settlementRequest.mutable_signpayouttxrequest();
   request->set_input(txSignReq.inputs[0].serialize().toBinStr());
   request->set_recipient(txSignReq.recipients[0]->getSerializedScript().toBinStr());
   request->set_fee(txSignReq.fee);

   fillSettlementData(request->mutable_settlement_data(), sd);
   *(settlementRequest.mutable_passworddialogdata()) = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::SignSettlementPayoutTxType);
   packet.set_data(settlementRequest.SerializeAsString());

   const auto reqId = Send(packet);
   cbSettlementSignTxMap_[reqId] = cb;
   return reqId;
}

bs::signer::RequestId HeadlessContainer::signMultiTXRequest(const bs::core::wallet::TXMultiSignRequest &txMultiReq)
{
   if (!txMultiReq.isValid()) {
      logger_->error("[HeadlessContainer::signMultiTXRequest] Invalid TXMultiSignRequest");
      return 0;
   }

   Signer signer;
   signer.setFlags(SCRIPT_VERIFY_SEGWIT);

   headless::SignTXMultiRequest request;
   for (const auto &input : txMultiReq.inputs) {
      request.add_walletids(input.second);
      signer.addSpender(std::make_shared<ScriptSpender>(input.first));
   }
   for (const auto &recip : txMultiReq.recipients) {
      signer.addRecipient(recip);
   }
   request.set_signerstate(signer.serializeState().toBinStr());

   headless::RequestPacket packet;
   packet.set_type(headless::SignTXMultiRequestType);
   packet.set_data(request.SerializeAsString());
   const auto id = Send(packet);
   signRequests_.insert(id);
   return id;
}

bs::signer::RequestId HeadlessContainer::signAuthRevocation(const std::string &walletId, const bs::Address &authAddr
   , const UTXO &utxo, const bs::Address &bsAddr, const SignTxCb &cb)
{
   headless::SignAuthAddrRevokeRequest request;
   request.set_wallet_id(walletId);
   request.set_auth_address(authAddr.display());
   request.set_utxo(utxo.serialize().toBinStr());
   request.set_validation_address(bsAddr.display());

   headless::RequestPacket packet;
   packet.set_type(headless::SignAuthAddrRevokeType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   cbSettlementSignTxMap_[reqId] = cb;
   signRequests_.insert(reqId);
   return reqId;
}

bs::signer::RequestId HeadlessContainer::updateDialogData(const bs::sync::PasswordDialogData &dialogData, uint32_t dialogId)
{
   headless::UpdateDialogDataRequest updateDialogDataRequest;
   updateDialogDataRequest.set_dialogid(dialogId);
   *(updateDialogDataRequest.mutable_passworddialogdata()) = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::UpdateDialogDataType);

   packet.set_data(updateDialogDataRequest.SerializeAsString());
   const auto reqId = Send(packet);
   return reqId;
}

bs::signer::RequestId HeadlessContainer::CancelSignTx(const BinaryData &txId)
{
   headless::CancelSignTx request;
   request.set_tx_id(txId.toBinStr());

   headless::RequestPacket packet;
   packet.set_type(headless::CancelSignTxRequestType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

bs::signer::RequestId HeadlessContainer::setUserId(const BinaryData &userId, const std::string &walletId)
{
   if (!listener_) {
      logger_->warn("[HeadlessContainer::SetUserId] listener not set yet");
      return 0;
   }

   bs::sync::PasswordDialogData info;
   info.setValue(PasswordDialogData::WalletId, QString::fromStdString(walletId));

   headless::SetUserIdRequest request;
   auto dialogData = request.mutable_passworddialogdata();
   *dialogData = info.toProtobufMessage();
   if (!userId.isNull()) {
      request.set_userid(userId.toBinStr());
   }

   headless::RequestPacket packet;
   packet.set_type(headless::SetUserIdType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

bs::signer::RequestId HeadlessContainer::syncCCNames(const std::vector<std::string> &ccNames)
{
   logger_->debug("[HeadlessContainer::syncCCNames] syncing {} CCs", ccNames.size());
   headless::SyncCCNamesData request;
   for (const auto &cc : ccNames) {
      request.add_ccnames(cc);
   }

   headless::RequestPacket packet;
   packet.set_type(headless::SyncCCNamesType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

bool HeadlessContainer::createHDLeaf(const std::string &rootWalletId, const bs::hd::Path &path
   , const std::vector<bs::wallet::PasswordData> &pwData, bs::sync::PasswordDialogData dialogData
   , const CreateHDLeafCb &cb)
{
   if (rootWalletId.empty() || (path.length() != 3)) {
      logger_->error("[HeadlessContainer::createHDLeaf] Invalid input data for HD wallet creation");
      return false;
   }
   headless::CreateHDLeafRequest request;
   request.set_rootwalletid(rootWalletId);
   request.set_path(path.toString());

   if (!pwData.empty()) {
      if (!pwData[0].salt.isNull()) {
         request.set_salt(pwData[0].salt.toBinStr());
      }
   }
   dialogData.setValue(PasswordDialogData::WalletId, QString::fromStdString(rootWalletId));

   auto requestDialogData = request.mutable_passworddialogdata();
   *requestDialogData = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::CreateHDLeafRequestType);
   packet.set_data(request.SerializeAsString());
   auto createLeafRequestId = Send(packet);
   if (createLeafRequestId == 0) {
      logger_->error("[HeadlessContainer::createHDLeaf] failed to send request");
      return false;
   }

   if (cb) {
      cbCCreateLeafMap_.emplace(createLeafRequestId, cb);
   } else {
      logger_->warn("[HeadlessContainer::createHDLeaf] cb not set for leaf creation {}"
                     , path.toString());
   }
   return true;
}

bool HeadlessContainer::promoteHDWallet(const std::string& rootWalletId
   , const BinaryData &userId, bs::sync::PasswordDialogData dialogData
   , const WalletSignerContainer::PromoteHDWalletCb& cb)
{
   headless::PromoteHDWalletRequest request;
   request.set_rootwalletid(rootWalletId);
   request.set_user_id(userId.toBinStr());

   dialogData.setValue(PasswordDialogData::WalletId, QString::fromStdString(rootWalletId));

   auto requestDialogData = request.mutable_passworddialogdata();
   *requestDialogData = dialogData.toProtobufMessage();

   headless::RequestPacket packet;
   packet.set_type(headless::PromoteHDWalletRequestType);
   packet.set_data(request.SerializeAsString());
   auto promoteWalletRequestID = Send(packet);

   if (promoteWalletRequestID == 0) {
      logger_->error("[HeadlessContainer::promoteHDWallet] failed to send request");
      return false;
   }

   if (cb) {
      cbPromoteHDWalletMap_.emplace(promoteWalletRequestID, cb);
   } else {
      logger_->warn("[HeadlessContainer::promoteHDWallet] cb not set for wallet promotion {}"
                     , rootWalletId);
   }

   return true;
}

bs::signer::RequestId HeadlessContainer::DeleteHDRoot(const std::string &rootWalletId)
{
   if (rootWalletId.empty()) {
      return 0;
   }
   return SendDeleteHDRequest(rootWalletId, {});
}

bs::signer::RequestId HeadlessContainer::DeleteHDLeaf(const std::string &leafWalletId)
{
   if (leafWalletId.empty()) {
      return 0;
   }
   return SendDeleteHDRequest({}, leafWalletId);
}

bs::signer::RequestId HeadlessContainer::SendDeleteHDRequest(const std::string &rootWalletId, const std::string &leafId)
{
   headless::DeleteHDWalletRequest request;
   if (!rootWalletId.empty()) {
      request.set_rootwalletid(rootWalletId);
   }
   else if (!leafId.empty()) {
      request.set_leafwalletid(leafId);
   }
   else {
      logger_->error("[HeadlessContainer::SendDeleteHDRequest] can't send delete request - both IDs are empty");
      return 0;
   }

   headless::RequestPacket packet;
   packet.set_type(headless::DeleteHDWalletRequestType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

//void HeadlessContainer::setLimits(const std::string &walletId, const SecureBinaryData &pass
//   , bool autoSign)
//{
//   if (walletId.empty()) {
//      logger_->error("[HeadlessContainer] no walletId for SetLimits");
//      return;
//   }
//   headless::SetLimitsRequest request;
//   request.set_rootwalletid(walletId);
//   if (!pass.isNull()) {
//      request.set_password(pass.toHexStr());
//   }
//   request.set_activateautosign(autoSign);

//   headless::RequestPacket packet;
//   packet.set_type(headless::SetLimitsRequestType);
//   packet.set_data(request.SerializeAsString());
//   Send(packet);
//}

bs::signer::RequestId HeadlessContainer::customDialogRequest(bs::signer::ui::GeneralDialogType signerDialog, const QVariantMap &data)
{
   // serialize variant data
   QByteArray ba;
   QDataStream stream(&ba, QIODevice::WriteOnly);
   stream << data;

   headless::CustomDialogRequest request;
   request.set_dialogname(bs::signer::ui::getGeneralDialogName(signerDialog).toStdString());
   request.set_variantdata(ba.data(), size_t(ba.size()));

   headless::RequestPacket packet;
   packet.set_type(headless::ExecCustomDialogRequestType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

bs::signer::RequestId HeadlessContainer::GetInfo(const std::string &rootWalletId)
{
   if (rootWalletId.empty()) {
      return 0;
   }
   headless::GetHDWalletInfoRequest request;
   request.set_rootwalletid(rootWalletId);

   headless::RequestPacket packet;
   packet.set_type(headless::GetHDWalletInfoRequestType);
   packet.set_data(request.SerializeAsString());
   return Send(packet);
}

bool HeadlessContainer::isReady() const
{
   return (listener_ != nullptr) && listener_->isReady();
}

bool HeadlessContainer::isWalletOffline(const std::string &walletId) const
{
   return ((missingWallets_.find(walletId) != missingWallets_.end())
      || (woWallets_.find(walletId) != woWallets_.end()));
}

void HeadlessContainer::createSettlementWallet(const bs::Address &authAddr
   , const std::function<void(const SecureBinaryData &)> &cb)
{
   headless::CreateSettlWalletRequest request;
   request.set_auth_address(authAddr.display());

   headless::RequestPacket packet;
   packet.set_data(request.SerializeAsString());
   packet.set_type(headless::CreateSettlWalletType);
   const auto reqId = Send(packet);
   cbSettlWalletMap_[reqId] = cb;
}

void HeadlessContainer::setSettlementID(const std::string &walletId, const SecureBinaryData &id
   , const std::function<void(bool)> &cb)
{
   headless::SetSettlementIdRequest request;
   request.set_wallet_id(walletId);
   request.set_settlement_id(id.toBinStr());

   headless::RequestPacket packet;
   packet.set_data(request.SerializeAsString());
   packet.set_type(headless::SetSettlementIdType);
   const auto reqId = Send(packet);
   cbSettlIdMap_[reqId] = cb;
}

void HeadlessContainer::getSettlementPayinAddress(const std::string &walletId
   , const bs::core::wallet::SettlementData &sd
   , const std::function<void(bool, bs::Address)> &cb)
{
   headless::SettlPayinAddressRequest request;
   request.set_wallet_id(walletId);
   fillSettlementData(request.mutable_settlement_data(), sd);

   headless::RequestPacket packet;
   packet.set_data(request.SerializeAsString());
   packet.set_type(headless::GetSettlPayinAddrType);
   const auto reqId = Send(packet);
   cbPayinAddrMap_[reqId] = cb;
}

void HeadlessContainer::getRootPubkey(const std::string &walletID
   , const std::function<void(bool, const SecureBinaryData &)> &cb)
{
   headless::SettlGetRootPubkeyRequest request;
   request.set_wallet_id(walletID);

   headless::RequestPacket packet;
   packet.set_data(request.SerializeAsString());
   packet.set_type(headless::SettlGetRootPubkeyType);
   const auto reqId = Send(packet);
   cbSettlPubkeyMap_[reqId] = cb;
}

void HeadlessContainer::getChatNode(const std::string &walletID
   , const std::function<void(const BIP32_Node &)> &cb)
{
   headless::ChatNodeRequest request;
   request.set_wallet_id(walletID);

   headless::RequestPacket packet;
   packet.set_data(request.SerializeAsString());
   packet.set_type(headless::ChatNodeRequestType);
   const auto reqId = Send(packet);
   cbChatNodeMap_[reqId] = cb;
}

void HeadlessContainer::syncWalletInfo(const std::function<void(std::vector<bs::sync::WalletInfo>)> &cb)
{
   headless::RequestPacket packet;
   packet.set_type(headless::SyncWalletInfoType);
   const auto reqId = Send(packet);
   cbWalletInfoMap_[reqId] = cb;
}

void HeadlessContainer::syncHDWallet(const std::string &id, const std::function<void(bs::sync::HDWalletData)> &cb)
{
   headless::SyncWalletRequest request;
   request.set_walletid(id);

   headless::RequestPacket packet;
   packet.set_type(headless::SyncHDWalletType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   cbHDWalletMap_[reqId] = cb;
}

void HeadlessContainer::syncWallet(const std::string &id, const std::function<void(bs::sync::WalletData)> &cb)
{
   headless::SyncWalletRequest request;
   request.set_walletid(id);

   headless::RequestPacket packet;
   packet.set_type(headless::SyncWalletType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   cbWalletMap_[reqId] = cb;
}

void HeadlessContainer::syncAddressComment(const std::string &walletId, const bs::Address &addr
   , const std::string &comment)
{
   headless::SyncCommentRequest request;
   request.set_walletid(walletId);
   request.set_address(addr.display());
   request.set_comment(comment);

   headless::RequestPacket packet;
   packet.set_type(headless::SyncCommentType);
   packet.set_data(request.SerializeAsString());
   Send(packet);
}

void HeadlessContainer::syncTxComment(const std::string &walletId, const BinaryData &txHash
   , const std::string &comment)
{
   headless::SyncCommentRequest request;
   request.set_walletid(walletId);
   request.set_txhash(txHash.toBinStr());
   request.set_comment(comment);

   headless::RequestPacket packet;
   packet.set_type(headless::SyncCommentType);
   packet.set_data(request.SerializeAsString());
   Send(packet);
}

void HeadlessContainer::extendAddressChain(
   const std::string &walletId, unsigned count, bool extInt,
   const std::function<void(const std::vector<std::pair<bs::Address, std::string>> &)> &cb)
{
   headless::ExtendAddressChainRequest request;
   request.set_wallet_id(walletId);
   request.set_count(count);
   request.set_ext_int(extInt);

   headless::RequestPacket packet;
   packet.set_type(headless::ExtendAddressChainType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   if (!reqId) {
      if (cb) {
         cb({});
      }
      return;
   }
   cbExtAddrsMap_[reqId] = cb;
}

void HeadlessContainer::syncNewAddresses(const std::string &walletId
   , const std::vector<std::string> &inData
   , const std::function<void(const std::vector<std::pair<bs::Address, std::string>> &)> &cb
   , bool persistent)
{
   headless::SyncNewAddressRequest request;
   request.set_wallet_id(walletId);
   for (const auto &in : inData) {
      auto addrData = request.add_addresses();
      addrData->set_index(in);
   }

   headless::RequestPacket packet;
   packet.set_type(headless::SyncNewAddressType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   if (!reqId) {
      if (cb) {
         cb({});
      }
      return;
   }
   cbExtAddrsMap_[reqId] = cb;
}

void HeadlessContainer::syncAddressBatch(
   const std::string &walletId, const std::set<BinaryData>& addrSet,
   std::function<void(bs::sync::SyncState)> cb)
{
   QMetaObject::invokeMethod(this, [this, walletId, addrSet, cb] {
      headless::SyncAddressesRequest request;
      request.set_wallet_id(walletId);
      for (const auto &addr : addrSet) {
         request.add_addresses(addr.toBinStr());
      }

      headless::RequestPacket packet;
      packet.set_type(headless::SyncAddressesType);
      packet.set_data(request.SerializeAsString());
      const auto reqId = Send(packet);
      if (!reqId) {
         if (cb) {
            cb(bs::sync::SyncState::Failure);
         }
         return;
      }
      cbSyncAddrsMap_[reqId] = cb;
   });
}

void HeadlessContainer::getAddressPreimage(const std::map<std::string, std::vector<bs::Address>> &inputs
   , const std::function<void(const std::map<bs::Address, BinaryData> &)> &cb)
{
   headless::AddressPreimageRequest request;
   for (const auto &input : inputs) {
      auto req = request.add_request();
      req->set_wallet_id(input.first);
      for (const auto &addr : input.second) {
         req->add_address(addr.display());
      }
   }
   headless::RequestPacket packet;
   packet.set_type(headless::AddressPreimageType);
   packet.set_data(request.SerializeAsString());
   const auto reqId = Send(packet);
   if (!reqId) {
      if (cb) {
         cb({});
      }
      return;
   }
   cbAddrPreimageMap_[reqId] = cb;
}

void HeadlessContainer::ProcessAddrPreimageResponse(unsigned int id, const std::string &data)
{
   headless::AddressPreimageResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessAddrPreimageResponse] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   std::map<bs::Address, BinaryData> result;
   for (int i = 0; i < response.response_size(); ++i) {
      const auto resp = response.response(i);
      for (int j = 0; j < resp.preimages_size(); ++j) {
         const auto piData = resp.preimages(j);
         auto addrObj = bs::Address::fromAddressString(piData.address());
         result[addrObj] = piData.preimage();
      }
   }
   const auto itCb = cbAddrPreimageMap_.find(id);
   if (itCb == cbAddrPreimageMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   itCb->second(result);
   cbAddrPreimageMap_.erase(itCb);
}

void HeadlessContainer::ProcessSettlWalletCreate(unsigned int id, const std::string &data)
{
   headless::CreateSettlWalletResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSettlWalletCreate] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbSettlWalletMap_.find(id);
   if (itCb == cbSettlWalletMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   itCb->second(response.public_key());
   cbSettlWalletMap_.erase(itCb);
}

void HeadlessContainer::ProcessSetSettlementId(unsigned int id, const std::string &data)
{
   headless::SetSettlementIdResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSetSettlementId] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbSettlIdMap_.find(id);
   if (itCb == cbSettlIdMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   itCb->second(response.success());
   cbSettlIdMap_.erase(itCb);
}

void HeadlessContainer::ProcessGetPayinAddr(unsigned int id, const std::string &data)
{
   headless::SettlPayinAddressResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessGetPayinAddr] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbPayinAddrMap_.find(id);
   if (itCb == cbPayinAddrMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   auto addrObj = bs::Address::fromAddressString(response.address());
   itCb->second(response.success(), addrObj);
   cbPayinAddrMap_.erase(itCb);
}

void HeadlessContainer::ProcessSettlGetRootPubkey(unsigned int id, const std::string &data)
{
   headless::SettlGetRootPubkeyResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSettlGetRootPubkey] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbSettlPubkeyMap_.find(id);
   if (itCb == cbSettlPubkeyMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   itCb->second(response.success(), response.public_key());
   cbSettlPubkeyMap_.erase(itCb);
}

void HeadlessContainer::ProcessChatNodeResponse(unsigned int id, const std::string &data)
{
   headless::ChatNodeResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessChatNodeResponse] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbChatNodeMap_.find(id);
   if (itCb == cbChatNodeMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }

   if (response.wallet_id().empty()) {
      logger_->error("[HeadlessContainer::ProcessChatNodeResponse] wallet not found");
      emit Error(id, "wallet not found for chat node");
   }
   else {
      BIP32_Node chatNode;
      try {
         chatNode.initFromBase58(response.b58_chat_node());
      } catch (const std::exception &e) {
         logger_->error("[HeadlessContainer::ProcessChatNodeResponse] failed to deserialize BIP32 node: {}", e.what());
      }
      itCb->second(chatNode);
   }
   cbChatNodeMap_.erase(itCb);
}

void HeadlessContainer::ProcessSyncWalletInfo(unsigned int id, const std::string &data)
{
   headless::SyncWalletInfoResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSyncWalletInfo] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbWalletInfoMap_.find(id);
   if (itCb == cbWalletInfoMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   std::vector<bs::sync::WalletInfo> result = bs::sync::WalletInfo::fromPbMessage(response);

   for (size_t i = 0; i < result.size(); ++i) {
      const auto &walletInfo = result.at(i);
      if (walletInfo.watchOnly) {
         woWallets_.insert(walletInfo.id);
      } else {
         woWallets_.erase(walletInfo.id);
      }
   }
   itCb->second(result);
   cbWalletInfoMap_.erase(itCb);
}

void HeadlessContainer::ProcessSyncHDWallet(unsigned int id, const std::string &data)
{
   headless::SyncHDWalletResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSyncHDWallet] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbHDWalletMap_.find(id);
   if (itCb == cbHDWalletMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   const bool isWoRoot = (woWallets_.find(response.walletid()) != woWallets_.end());
   bs::sync::HDWalletData result;
   for (int i = 0; i < response.groups_size(); ++i) {
      const auto groupInfo = response.groups(i);
      bs::sync::HDWalletData::Group group;
      group.type = static_cast<bs::hd::CoinType>(groupInfo.type() | bs::hd::hardFlag);
      group.extOnly = groupInfo.ext_only();
      group.salt = groupInfo.salt();
      for (int j = 0; j < groupInfo.leaves_size(); ++j) {
         const auto leafInfo = groupInfo.leaves(j);
         if (isWoRoot) {
            woWallets_.insert(leafInfo.id());
         }
         group.leaves.push_back({ leafInfo.id(), bs::hd::Path::fromString(leafInfo.path())
            , group.extOnly, leafInfo.extra_data() });
      }
      result.groups.push_back(group);
   }
   itCb->second(result);
   cbHDWalletMap_.erase(itCb);
}

void HeadlessContainer::ProcessSyncWallet(unsigned int id, const std::string &data)
{
   qint64 t1 = QDateTime::currentMSecsSinceEpoch();

   headless::SyncWalletResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSyncWallet] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbWalletMap_.find(id);
   if (itCb == cbWalletMap_.end()) {
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }

   bs::sync::WalletData result = bs::sync::WalletData::fromPbMessage(response);

   itCb->second(result);
   cbWalletMap_.erase(itCb);
   qDebug() << "HeadlessContainer::ProcessSyncWallet" << QDateTime::currentMSecsSinceEpoch() - t1
            << result.highestExtIndex << result.highestIntIndex
            << result.addresses.size() << result.addrPool.size() << result.txComments.size();

}

static bs::sync::SyncState mapFrom(headless::SyncState state)
{
   switch (state) {
   case headless::SyncState_Success:      return bs::sync::SyncState::Success;
   case headless::SyncState_NothingToDo:  return bs::sync::SyncState::NothingToDo;
   case headless::SyncState_Failure:      return bs::sync::SyncState::Failure;
   }
   return bs::sync::SyncState::Failure;
}

void HeadlessContainer::ProcessSyncAddresses(unsigned int id, const std::string &data)
{
   headless::SyncAddressesResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessSyncAddresses] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbSyncAddrsMap_.find(id);
   if (itCb == cbSyncAddrsMap_.end()) {
      logger_->error("[HeadlessContainer::ProcessSyncAddresses] no callback found for id {}", id);
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }

   const auto result = mapFrom(response.state());
   itCb->second(result);
   cbSyncAddrsMap_.erase(itCb);
}

void HeadlessContainer::ProcessExtAddrChain(unsigned int id, const std::string &data)
{
   headless::ExtendAddressChainResponse response;
   if (!response.ParseFromString(data)) {
      logger_->error("[HeadlessContainer::ProcessExtAddrChain] Failed to parse reply");
      emit Error(id, "failed to parse");
      return;
   }
   const auto itCb = cbExtAddrsMap_.find(id);
   if (itCb == cbExtAddrsMap_.end()) {
      logger_->error("[HeadlessContainer::ProcessExtAddrChain] no callback found for id {}", id);
      emit Error(id, "no callback found for id " + std::to_string(id));
      return;
   }
   std::vector<std::pair<bs::Address, std::string>> result;
   for (int i = 0; i < response.addresses_size(); ++i) {
      const auto &addr = response.addresses(i);
      auto addrObj = bs::Address::fromAddressString(addr.address());
      result.push_back({ addrObj, addr.index() });
   }
   itCb->second(result);
   cbExtAddrsMap_.erase(itCb);
}


RemoteSigner::RemoteSigner(const std::shared_ptr<spdlog::logger> &logger
   , const QString &host, const QString &port, NetworkType netType
   , const std::shared_ptr<ConnectionManager>& connectionManager
   , OpMode opMode
   , const bool ephemeralDataConnKeys
   , const std::string& ownKeyFileDir
   , const std::string& ownKeyFileName
   , const ZmqBipNewKeyCb& inNewKeyCB)
   : HeadlessContainer(logger, opMode)
   , host_(host), port_(port), netType_(netType)
   , ephemeralDataConnKeys_(ephemeralDataConnKeys)
   , ownKeyFileDir_(ownKeyFileDir)
   , ownKeyFileName_(ownKeyFileName)
   , cbNewKey_{inNewKeyCB}
   , connectionManager_{connectionManager}
{
}

// Establish the remote connection to the signer.
bool RemoteSigner::Start()
{
   if (!connection_) {
      RecreateConnection();
   }

   // If we've already connected, don't do more setup.
   if (headlessConnFinished_) {
      return true;
   }

   if (opMode() == OpMode::RemoteInproc) {
      connection_->SetZMQTransport(ZMQTransport::InprocTransport);
   }

   {
      std::lock_guard<std::mutex> lock(mutex_);
      listener_ = std::make_shared<HeadlessListener>(logger_, connection_, netType_);
      connect(listener_.get(), &HeadlessListener::connected, this
         , &RemoteSigner::onConnected, Qt::QueuedConnection);
      connect(listener_.get(), &HeadlessListener::authenticated, this
         , &RemoteSigner::onAuthenticated, Qt::QueuedConnection);
      connect(listener_.get(), &HeadlessListener::disconnected, this
         , &RemoteSigner::onDisconnected, Qt::QueuedConnection);
      connect(listener_.get(), &HeadlessListener::error, this
         , &RemoteSigner::onConnError, Qt::QueuedConnection);
      connect(listener_.get(), &HeadlessListener::PacketReceived, this
         , &RemoteSigner::onPacketReceived, Qt::QueuedConnection);
   }

   return Connect();
}

bool RemoteSigner::Stop()
{
   return Disconnect();
}

bool RemoteSigner::Connect()
{
   if (!connection_) {
      logger_->error("[RemoteSigner::Connect] connection not created");
      return false;
   }

   if (connection_->isActive()) {
      return true;
   }

   listener_->wasErrorReported_ = false;
   listener_->isShuttingDown_ = false;

   bool result = connection_->openConnection(host_.toStdString(), port_.toStdString(), listener_.get());
   if (!result) {
      logger_->error("[RemoteSigner::Connect] Failed to open connection to headless container");
      return false;
   }

   emit connected();
   headlessConnFinished_ = true;
   return true;
}

bool RemoteSigner::Disconnect()
{
   if (!connection_) {
      return true;
   }

   if (listener_) {
      listener_->isShuttingDown_ = true;
   }

   bool result = connection_->closeConnection();
   connection_.reset();
   return result;
}

void RemoteSigner::Authenticate()
{
   mutex_.lock();
   if (!listener_) {
      mutex_.unlock();
      emit connectionError(UnknownError, tr("listener missing on authenticate"));
      return;
   }
   mutex_.unlock();

   headless::AuthenticationRequest request;
   request.set_nettype((netType_ == NetworkType::TestNet) ? headless::TestNetType : headless::MainNetType);

   headless::RequestPacket packet;
   packet.set_type(headless::AuthenticationRequestType);
   packet.set_data(request.SerializeAsString());
   Send(packet);
}

void RemoteSigner::RecreateConnection()
{
   logger_->info("[RemoteSigner::RecreateConnection] Restart connection...");

   ZmqBIP15XDataConnectionParams params;
   params.ephemeralPeers = ephemeralDataConnKeys_;
   params.ownKeyFileDir = ownKeyFileDir_;
   params.ownKeyFileName = ownKeyFileName_;
   params.setLocalHeartbeatInterval();

   // Server's cookies are not available in remote mode
   if (opMode() == OpMode::Local || opMode() == OpMode::LocalInproc) {
      params.cookie = BIP15XCookie::ReadServer;
      params.cookiePath = SystemFilePaths::appDataLocation() + "/" + "signerServerID";
   }

   try {
      connection_ = connectionManager_->CreateZMQBIP15XDataConnection(params);
      connection_->setCBs(cbNewKey_);

      headlessConnFinished_ = false;
   }
   catch (const std::exception &e) {
      logger_->error("[RemoteSigner::RecreateConnection] connection creation failed: {}", e.what());
      QTimer::singleShot(10, this, [this] {  // slight delay is required on start-up init
         emit connectionError(ConnectionError::SocketFailed, tr("Connection creation failed"));
      });
   }
}

void RemoteSigner::ScheduleRestart()
{
   if (isRestartScheduled_) {
      return;
   }

   isRestartScheduled_ = true;
   auto timeout = isLocal() ? kLocalReconnectPeriod : kRemoteReconnectPeriod;
   QTimer::singleShot(timeout, this, [this] {
      isRestartScheduled_ = false;
      RecreateConnection();
      Start();
   });
}

bool RemoteSigner::isOffline() const
{
   std::lock_guard<std::mutex> lock(mutex_);
   return (listener_ == nullptr);
}

void RemoteSigner::updatePeerKeys(const ZmqBIP15XPeers &peers)
{
   if (!connection_) {
      RecreateConnection();
   }

   connection_->updatePeerKeys(peers);
}

void RemoteSigner::onConnected()
{
   Authenticate();
}

void RemoteSigner::onAuthenticated()
{
   // Once the BIP 150/151 handshake is complete, it's safe to start sending
   // app-level data to the signer.
   emit authenticated();
   emit ready();
}

void RemoteSigner::onDisconnected()
{
   missingWallets_.clear();
   woWallets_.clear();

   // signRequests_ will be empty after that
   std::set<bs::signer::RequestId> tmpReqs = std::move(signRequests_);

   for (const auto &id : tmpReqs) {
      emit TXSigned(id, {}, bs::error::ErrorCode::TxCanceled, "Signer disconnected");
   }

   emit disconnected();

   ScheduleRestart();
}

void RemoteSigner::onConnError(ConnectionError error, const QString &details)
{
   emit connectionError(error, details);
   ScheduleRestart();
}

void RemoteSigner::onPacketReceived(headless::RequestPacket packet)
{
   signRequests_.erase(packet.id());

   switch (packet.type()) {
   case headless::SignTxRequestType:
   case headless::SignPartialTXRequestType:
   case headless::SignSettlementPayoutTxType:
   case headless::SignTXMultiRequestType:
   case headless::SignAuthAddrRevokeType:
      ProcessSignTXResponse(packet.id(), packet.data());
      break;

   case headless::SignSettlementTxRequestType:
   case headless::SignSettlementPartialTxType:
      ProcessSettlementSignTXResponse(packet.id(), packet.data());
      break;

   case headless::CreateHDLeafRequestType:
      ProcessCreateHDLeafResponse(packet.id(), packet.data());
      break;

   case headless::PromoteHDWalletRequestType:
      ProcessHDWalletPromotionResponse(packet.id(), packet.data());
      break;

   case headless::GetHDWalletInfoRequestType:
      ProcessGetHDWalletInfoResponse(packet.id(), packet.data());
      break;

   case headless::SetUserIdType:
      ProcessSetUserId(packet.data());
      break;

   case headless::AutoSignActType:
      ProcessAutoSignActEvent(packet.id(), packet.data());
      break;

   case headless::CreateSettlWalletType:
      ProcessSettlWalletCreate(packet.id(), packet.data());
      break;

   case headless::SetSettlementIdType:
      ProcessSetSettlementId(packet.id(), packet.data());
      break;

   case headless::GetSettlPayinAddrType:
      ProcessGetPayinAddr(packet.id(), packet.data());
      break;

   case headless::SettlGetRootPubkeyType:
      ProcessSettlGetRootPubkey(packet.id(), packet.data());
      break;

   case headless::SyncWalletInfoType:
      ProcessSyncWalletInfo(packet.id(), packet.data());
      break;

   case headless::SyncHDWalletType:
      ProcessSyncHDWallet(packet.id(), packet.data());
      break;

   case headless::SyncWalletType:
      ProcessSyncWallet(packet.id(), packet.data());
      break;

   case headless::SyncCommentType:
      break;   // normally no data will be returned on sync of comments

   case headless::SyncAddressesType:
      ProcessSyncAddresses(packet.id(), packet.data());
      break;

   case headless::ExtendAddressChainType:
   case headless::SyncNewAddressType:
      ProcessExtAddrChain(packet.id(), packet.data());
      break;

   case headless::WalletsListUpdatedType:
      logger_->debug("received WalletsListUpdatedType message");
      emit walletsListUpdated();
      break;

   case headless::AddressPreimageType:
      ProcessAddrPreimageResponse(packet.id(), packet.data());
      break;

   case headless::ChatNodeRequestType:
      ProcessChatNodeResponse(packet.id(), packet.data());
      break;

   default:
      logger_->warn("[HeadlessContainer] Unknown packet type: {}", packet.type());
      break;
   }
}

LocalSigner::LocalSigner(const std::shared_ptr<spdlog::logger> &logger
   , const QString &homeDir, NetworkType netType, const QString &port
   , const std::shared_ptr<ConnectionManager>& connectionManager
   , const bool startSignerProcess
   , const std::string& ownKeyFileDir
   , const std::string& ownKeyFileName
   , double asSpendLimit
   , const ZmqBipNewKeyCb& inNewKeyCB)
   : RemoteSigner(logger, QLatin1String("127.0.0.1"), port, netType
      , connectionManager, OpMode::Local, true
      , ownKeyFileDir, ownKeyFileName, inNewKeyCB)
      , homeDir_(homeDir), startProcess_(startSignerProcess), asSpendLimit_(asSpendLimit)
{}

LocalSigner::~LocalSigner() noexcept
{
   Stop();
}

QStringList LocalSigner::args() const
{
   auto walletsCopyDir = homeDir_ + QLatin1String("/copy");
   if (!QDir().exists(walletsCopyDir)) {
      walletsCopyDir = homeDir_ + QLatin1String("/signer");
   }

   QStringList result;
   result << QLatin1String("--guimode") << QLatin1String("litegui");
   switch (netType_) {
   case NetworkType::TestNet:
   case NetworkType::RegTest:
      result << QString::fromStdString("--testnet");
      break;
   case NetworkType::MainNet:
      result << QString::fromStdString("--mainnet");
      break;
   default:
      break;
   }

   // Among many other things, send the signer the terminal's BIP 150 ID key.
   // Processes reading keys from the disk are subject to attack.
   result << QLatin1String("--listen") << QLatin1String("127.0.0.1");
   result << QLatin1String("--accept_from") << QLatin1String("127.0.0.1");
   result << QLatin1String("--port") << port_;
   result << QLatin1String("--dirwallets") << walletsCopyDir;
   if (asSpendLimit_ > 0) {
      result << QLatin1String("--auto_sign_spend_limit")
         << QString::number(asSpendLimit_, 'f', 8);
   }
   result << QLatin1String("--terminal_id_key")
      << QString::fromStdString(connection_->getOwnPubKey().toHexStr());

   return result;
}

bool LocalSigner::Start()
{
   Stop();

   bool result = RemoteSigner::Start();
   if (!result) {
      return false;
   }

   if (startProcess_) {
      // If there's a previous headless process, stop it.
      headlessProcess_ = std::make_shared<QProcess>();

#ifdef Q_OS_WIN
      const auto signerAppPath = QCoreApplication::applicationDirPath() + QLatin1String("/blocksettle_signer.exe");
#elif defined (Q_OS_MACOS)
      auto bundleDir = QDir(QCoreApplication::applicationDirPath());
      bundleDir.cdUp();
      bundleDir.cdUp();
      bundleDir.cdUp();
      const auto signerAppPath = bundleDir.absoluteFilePath(QLatin1String("BlockSettle Signer.app/Contents/MacOS/BlockSettle Signer"));
#else
      const auto signerAppPath = QCoreApplication::applicationDirPath() + QLatin1String("/blocksettle_signer");
#endif
      if (!QFile::exists(signerAppPath)) {
         logger_->error("[LocalSigner::Start] Signer binary {} not found"
            , signerAppPath.toStdString());
         emit connectionError(UnknownError, tr("missing signer binary"));
         return false;
      }

      const auto cmdArgs = args();
      logger_->debug("[LocalSigner::Start] starting {} {}"
         , signerAppPath.toStdString(), cmdArgs.join(QLatin1Char(' ')).toStdString());

#ifndef NDEBUG
      headlessProcess_->setProcessChannelMode(QProcess::MergedChannels);
      connect(headlessProcess_.get(), &QProcess::readyReadStandardOutput, this, [this]() {
         qDebug().noquote() << headlessProcess_->readAllStandardOutput();
      });
#endif

      headlessProcess_->start(signerAppPath, cmdArgs);
      if (!headlessProcess_->waitForStarted(kStartTimeout)) {
         logger_->error("[LocalSigner::Start] Failed to start process");
         headlessProcess_.reset();
         emit connectionError(UnknownError, tr("failed to start process"));
         return false;
      }
   }

   return true;
}

bool LocalSigner::Stop()
{
   RemoteSigner::Stop();

   if (headlessProcess_) {
      if (!headlessProcess_->waitForFinished(kKillTimeout)) {
         headlessProcess_->terminate();
         headlessProcess_->waitForFinished(kKillTimeout);
      }
      headlessProcess_.reset();
   }
   return true;
}

void HeadlessListener::processDisconnectNotification()
{
   SPDLOG_LOGGER_INFO(logger_, "remote signer has been disconnected");
   isConnected_ = false;
   isReady_ = false;
   tryEmitError(HeadlessContainer::SignerGoesOffline, tr("Remote signer disconnected"));
}

void HeadlessListener::tryEmitError(SignContainer::ConnectionError errorCode, const QString &msg)
{
   // Try to send error only once because only first error should be relevant.
   if (!wasErrorReported_) {
      wasErrorReported_ = true;
      emit error(errorCode, msg);
   }
}

bs::signer::RequestId HeadlessListener::newRequestId()
{
   return ++id_;
}
