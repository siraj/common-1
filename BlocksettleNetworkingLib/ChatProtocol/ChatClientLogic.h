/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#ifndef CHATCLIENTLOGIC_H
#define CHATCLIENTLOGIC_H

#include "ChatProtocol/ChatSettings.h"
#include "ChatProtocol/ChatUser.h"
#include "ChatProtocol/ClientConnectionLogic.h"
#include "ChatProtocol/ClientDBService.h"
#include "ChatProtocol/ClientPartyLogic.h"
#include "ChatProtocol/CryptManager.h"

#include "CommonTypes.h"
#include "DataConnectionListener.h"

#include <disable_warnings.h>
#include "ZMQ_BIP15X_DataConnection.h"
#include "ZMQ_BIP15X_Helpers.h"
#include <enable_warnings.h>

namespace spdlog
{
   class logger;
}


namespace Chat
{
   using LoggerPtr = std::shared_ptr<spdlog::logger>;
   using SearchUserReplyList = std::vector<std::string>;

   enum class ChatClientLogicError
   {
      NoError,
      ConnectionAlreadyInitialized,
      ConnectionAlreadyUsed,
      ZmqDataConnectionFailed,
      ClientPartyNotExist,
      PartyNotExist
   };

   class ChatClientLogic final : public QObject, public DataConnectionListener
   {
      Q_OBJECT

   public:
      ChatClientLogic();
      ~ChatClientLogic() override;

      ChatClientLogic(const ChatClientLogic&) = delete;
      ChatClientLogic& operator=(const ChatClientLogic&) = delete;
      ChatClientLogic(ChatClientLogic&&) = delete;
      ChatClientLogic& operator=(ChatClientLogic&&) = delete;

      void OnDataReceived(const std::string&) override;
      void OnConnected() override;
      void OnDisconnected() override;
      void OnError(DataConnectionError) override;

      ClientPartyModelPtr clientPartyModelPtr() const { return clientPartyLogicPtr_->clientPartyModelPtr(); }

   public slots:
      void Init(Chat::LoggerPtr loggerPtr, ChatSettings chatSettings);
      void LoginToServer(const BinaryData &token, const BinaryData &tokenSign, const ZmqBipNewKeyCb& cb);
      void LogoutFromServer();
      void SendPartyMessage(const std::string& partyId, const std::string& data);
      void SetMessageSeen(const std::string& partyId, const std::string& messageId);
      void RequestPrivateParty(const std::string& userName, const std::string& initialMessage) const;
      void RequestPrivatePartyOTC(const std::string& remoteUserName) const;
      void RejectPrivateParty(const std::string& partyId) const;
      void DeletePrivateParty(const std::string& partyId);
      void AcceptPrivateParty(const std::string& partyId) const;
      void SearchUser(const std::string& userHash, const std::string& searchId) const;
      void AcceptNewPublicKeys(const Chat::UserPublicKeyInfoList& userPublicKeyInfoList) const;
      void DeclineNewPublicKeys(const Chat::UserPublicKeyInfoList& userPublicKeyInfoList);

   signals:
      void dataReceived(const std::string&);
      void connected();
      void disconnected();
      void error(DataConnectionListener::DataConnectionError);

      void messagePacketSent(const std::string& messageId);

      void finished();
      void chatClientError(const Chat::ChatClientLogicError& errorCode, const std::string& what = "");

      void chatUserUserHashChanged(const std::string& chatUserUserHash);
      void clientLoggedOutFromServer();
      void partyModelChanged();
      void initDone();
      void properlyConnected();
      void searchUserReply(const Chat::SearchUserReplyList& userHashList, const std::string& searchId);
      void otcPrivatePartyReady(const ClientPartyPtr& clientPartyPtr);

   private slots:
      void sendPacket(const google::protobuf::Message& message);
      void onCloseConnection();
      void handleLocalErrors(const ChatClientLogicError& errorCode, const std::string& what) const;
      void initDbDone();
      void privatePartyCreated(const std::string& partyId) const;
      void privatePartyAlreadyExist(const std::string& partyId) const;

   private:
      void setClientPartyLogicPtr(const ClientPartyLogicPtr& val) { clientPartyLogicPtr_ = val; }
      std::string getChatServerHost() const;
      std::string getChatServerPort() const;

      ChatSettings               chatSettings_;
      ZmqBIP15XDataConnectionPtr connectionPtr_;
      LoggerPtr                  loggerPtr_;
      ChatUserPtr                currentUserPtr_;
      ClientConnectionLogicPtr   clientConnectionLogicPtr_;
      ClientPartyLogicPtr        clientPartyLogicPtr_;
      ClientDBServicePtr         clientDBServicePtr_;
      CryptManagerPtr            cryptManagerPtr_;
      SessionKeyHolderPtr        sessionKeyHolderPtr_;

   };

}

Q_DECLARE_METATYPE(DataConnectionListener::DataConnectionError)
Q_DECLARE_METATYPE(Chat::ChatClientLogicError)
Q_DECLARE_METATYPE(Chat::ClientPartyLogicPtr)
Q_DECLARE_METATYPE(Chat::ChatUserPtr)

#endif // CHATCLIENTLOGIC_H
