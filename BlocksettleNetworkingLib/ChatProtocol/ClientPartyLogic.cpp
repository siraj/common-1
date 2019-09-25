#include <QUuid>

#include "ChatProtocol/ClientPartyLogic.h"
#include "ChatProtocol/ClientParty.h"

#include <disable_warnings.h>
#include <spdlog/logger.h>
#include <enable_warnings.h>

#include "chat.pb.h"

using namespace Chat;

ClientPartyLogic::ClientPartyLogic(const LoggerPtr& loggerPtr, const ClientDBServicePtr& clientDBServicePtr, QObject* parent)
   : loggerPtr_(loggerPtr), QObject(parent)
{
   qRegisterMetaType<Chat::ClientPartyLogicError>();

   clientDBServicePtr_ = clientDBServicePtr;
   clientPartyModelPtr_ = std::make_shared<ClientPartyModel>(loggerPtr, this);
   connect(clientPartyModelPtr_.get(), &ClientPartyModel::clientPartyDisplayNameChanged, this, &ClientPartyLogic::clientPartyDisplayNameChanged);
   connect(clientPartyModelPtr_.get(), &ClientPartyModel::partyModelChanged, this, &ClientPartyLogic::partyModelChanged);

   connect(clientDBServicePtr.get(), &ClientDBService::partyDisplayNameLoaded, this, &ClientPartyLogic::partyDisplayNameLoaded);

   connect(this, &ClientPartyLogic::error, this, &ClientPartyLogic::handleLocalErrors);
   connect(clientDBServicePtr.get(), &ClientDBService::messageArrived, clientPartyModelPtr_.get(), &ClientPartyModel::messageArrived);
   connect(clientDBServicePtr.get(), &ClientDBService::messageStateChanged, clientPartyModelPtr_.get(), &ClientPartyModel::messageStateChanged);
   connect(clientDBServicePtr_.get(), &ClientDBService::recipientKeysHasChanged, this, &ClientPartyLogic::onRecipientKeysHasChanged);
   connect(clientDBServicePtr_.get(), &ClientDBService::recipientKeysUnchanged, this, &ClientPartyLogic::onRecipientKeysUnchanged);

   connect(clientPartyModelPtr_.get(), &ClientPartyModel::partyInserted, this, &ClientPartyLogic::handlePartyInserted);
   connect(this, &ClientPartyLogic::userPublicKeyChanged, clientPartyModelPtr_.get(), &ClientPartyModel::userPublicKeyChanged, Qt::QueuedConnection);
}

void ClientPartyLogic::handlePartiesFromWelcomePacket(const WelcomeResponse& welcomeResponse)
{
   clientPartyModelPtr_->clearModel();

   // all unique recipients
   UniqieRecipientMap uniqueRecipients;

   for (int i = 0; i < welcomeResponse.party_size(); i++)
   {
      const PartyPacket& partyPacket = welcomeResponse.party(i);

      ClientPartyPtr clientPartyPtr = std::make_shared<ClientParty>(
         partyPacket.party_id(), partyPacket.party_type(), partyPacket.party_subtype(), partyPacket.party_state());
      clientPartyPtr->setDisplayName(partyPacket.display_name());
      clientPartyPtr->setUserHash(partyPacket.display_name());
      clientPartyPtr->setPartyCreatorHash(partyPacket.party_creator_hash());

      if (PartyType::PRIVATE_DIRECT_MESSAGE == partyPacket.party_type() && PartySubType::STANDARD == partyPacket.party_subtype())
      {
         PartyRecipientsPtrList recipients;
         for (int i = 0; i < partyPacket.recipient_size(); i++)
         {
            PartyRecipientPacket recipient = partyPacket.recipient(i);
            PartyRecipientPtr recipientPtr =
               std::make_shared<PartyRecipient>(recipient.user_name(), recipient.public_key(), QDateTime::fromMSecsSinceEpoch(recipient.timestamp_ms()));
            recipients.push_back(recipientPtr);

            uniqueRecipients[recipientPtr->userName()] = recipientPtr;
         }

         clientPartyPtr->setRecipients(recipients);
      }

      clientPartyModelPtr_->insertParty(clientPartyPtr);

      // Read and provide last 10 history messages for private parties
      if (PartyType::PRIVATE_DIRECT_MESSAGE == partyPacket.party_type() && PartySubType::STANDARD == partyPacket.party_subtype())
      {
         clientDBServicePtr_->readHistoryMessages(partyPacket.party_id(), 10);
      }
   }

   // check if any of recipients has changed public key
   clientDBServicePtr_->checkRecipientPublicKey(uniqueRecipients);
}

void ClientPartyLogic::onUserStatusChanged(const StatusChanged& statusChanged)
{
   ClientPartyPtr clientPartyPtr = clientPartyModelPtr_->getPartyByUserName(statusChanged.user_name());

   if (clientPartyPtr == nullptr)
   {
      emit error(ClientPartyLogicError::NonexistentClientStatusChanged, statusChanged.user_name());
      return;
   }

   // don't change status for other than private parties
   if (PartyType::PRIVATE_DIRECT_MESSAGE != clientPartyPtr->partyType())
   {
      return;
   }

   clientPartyPtr->setClientStatus(statusChanged.client_status());

   if (ClientStatus::ONLINE != clientPartyPtr->clientStatus())
   {
      return;
   }

   // check if public key changed
   if (statusChanged.has_public_key())
   {
      PartyRecipientPtr recipientPtr = clientPartyPtr->getRecipient(statusChanged.user_name());

      if (recipientPtr)
      {
         const BinaryData public_key(statusChanged.public_key().value());
         const QDateTime dt = QDateTime::fromMSecsSinceEpoch(statusChanged.timestamp_ms().value());
         const UserPublicKeyInfoPtr userPkPtr = std::make_shared<UserPublicKeyInfo>();

         userPkPtr->setUser_hash(QString::fromStdString(recipientPtr->userName()));
         userPkPtr->setOldPublicKeyHex(recipientPtr->publicKey());
         userPkPtr->setOldPublicKeyTime(recipientPtr->publicKeyTime());
         userPkPtr->setNewPublicKeyHex(public_key);
         userPkPtr->setNewPublicKeyTime(dt);
         UserPublicKeyInfoList userPkList;
         userPkList.push_back(userPkPtr);

         emit userPublicKeyChanged(userPkList);
      }

      return;
   }

   // if client status is online check if we have any unsent messages for this user
   clientDBServicePtr_->checkUnsentMessages(clientPartyPtr->id());
}

void ClientPartyLogic::handleLocalErrors(const ClientPartyLogicError& errorCode, const std::string& what)
{
   loggerPtr_->debug("[ClientPartyLogic::handleLocalErrors] Error: {}, what: {}", (int)errorCode, what);
}

void ClientPartyLogic::handlePartyInserted(const Chat::PartyPtr& partyPtr)
{
   clientDBServicePtr_->createNewParty(partyPtr->id());
}

void ClientPartyLogic::createPrivateParty(const ChatUserPtr& currentUserPtr, const std::string& remoteUserName)
{
   // check if private party exist
   IdPartyList idPartyList = clientPartyModelPtr_->getIdPartyList();

   for (const auto& partyId : idPartyList)
   {
      ClientPartyPtr clientPartyPtr = clientPartyModelPtr_->getClientPartyById(partyId);
      if (!clientPartyPtr)
      {
         continue;
      }

      if (clientPartyPtr->isPrivateStandard())
      {
         continue;
      }

      PartyRecipientsPtrList recipients = clientPartyPtr->getRecipientsExceptMe(currentUserPtr->userName());
      for (const auto& recipient : recipients)
      {
         if (recipient->userName() == remoteUserName)
         {
            // party already existed
            emit privatePartyAlreadyExist(clientPartyPtr->id());
            return;
         }
      }
   }

   // party not exist, create new one
   ClientPartyPtr newClientPrivatePartyPtr =
      std::make_shared<ClientParty>(QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString(), PartyType::PRIVATE_DIRECT_MESSAGE, PartySubType::STANDARD);

   newClientPrivatePartyPtr->setDisplayName(remoteUserName);
   newClientPrivatePartyPtr->setUserHash(remoteUserName);
   newClientPrivatePartyPtr->setPartyCreatorHash(currentUserPtr->userName());
   // setup recipients for new private party
   PartyRecipientsPtrList recipients;
   recipients.push_back(std::make_shared<PartyRecipient>(currentUserPtr->userName()));
   recipients.push_back(std::make_shared<PartyRecipient>(remoteUserName));
   newClientPrivatePartyPtr->setRecipients(recipients);

   // update model
   clientPartyModelPtr_->insertParty(newClientPrivatePartyPtr);
   emit partyModelChanged();

   // save party in db
   clientDBServicePtr_->createNewParty(newClientPrivatePartyPtr->id());

   emit privatePartyCreated(newClientPrivatePartyPtr->id());
}

void ClientPartyLogic::createPrivatePartyFromPrivatePartyRequest(const ChatUserPtr& currentUserPtr, const PrivatePartyRequest& privatePartyRequest)
{
   PartyPacket partyPacket = privatePartyRequest.party_packet();

   ClientPartyPtr newClientPrivatePartyPtr =
      std::make_shared<ClientParty>(
         partyPacket.party_id(),
         partyPacket.party_type(),
         partyPacket.party_subtype()
         );

   newClientPrivatePartyPtr->setPartyState(partyPacket.party_state());
   newClientPrivatePartyPtr->setPartyCreatorHash(partyPacket.party_creator_hash());
   newClientPrivatePartyPtr->setDisplayName(partyPacket.display_name());
   newClientPrivatePartyPtr->setUserHash(partyPacket.party_creator_hash());

   PartyRecipientsPtrList recipients;
   for (int i = 0; i < partyPacket.recipient_size(); i++)
   {
      PartyRecipientPtr recipient = std::make_shared<PartyRecipient>(
         partyPacket.recipient(i).user_name(), partyPacket.recipient(i).public_key()
         );

      recipients.push_back(recipient);
   }

   newClientPrivatePartyPtr->setRecipients(recipients);

   // check if already exist party in rejected state with this same user
   ClientPartyPtr oldClientPartyPtr = clientPartyModelPtr_->getClientPartyByUserHash(newClientPrivatePartyPtr->partyCreatorHash());
   if (nullptr != oldClientPartyPtr)
   {
      // exist one, checking party state
      if (PartyState::REJECTED == oldClientPartyPtr->partyState() && oldClientPartyPtr->isPrivateStandard())
      {
         emit deletePrivateParty(oldClientPartyPtr->id());

         // delete old recipients keys
         PartyRecipientsPtrList oldRecipients = oldClientPartyPtr->getRecipientsExceptMe(currentUserPtr->userName());
         clientDBServicePtr_->deleteRecipientsKeys(oldRecipients);
      }
   }

   // update model
   clientPartyModelPtr_->insertParty(newClientPrivatePartyPtr);

   // update recipients keys
   PartyRecipientsPtrList remoteRecipients = newClientPrivatePartyPtr->getRecipientsExceptMe(currentUserPtr->userName());
   clientDBServicePtr_->saveRecipientsKeys(remoteRecipients);

   emit partyModelChanged();

   // save party in db
   clientDBServicePtr_->createNewParty(newClientPrivatePartyPtr->id());

   // ! Do NOT emit here privatePartyCreated, it's connected with party request
}

void ClientPartyLogic::clientPartyDisplayNameChanged(const std::string& partyId)
{
   ClientPartyPtr clientPartyPtr = clientPartyModelPtr_->getClientPartyById(partyId);

   if (!clientPartyPtr)
   {
      return;
   }

   clientDBServicePtr_->updateDisplayNameForParty(partyId, clientPartyPtr->displayName());
}

void ClientPartyLogic::partyDisplayNameLoaded(const std::string& partyId, const std::string& displayName)
{
   ClientPartyPtr clientPartyPtr = clientPartyModelPtr_->getClientPartyById(partyId);

   if (clientPartyPtr == nullptr)
   {
      emit error(ClientPartyLogicError::PartyNotExist, partyId);
      return;
   }

   clientPartyPtr->setDisplayName(displayName);

   emit partyModelChanged();
}

// if logged out set offline for all private parties
void ClientPartyLogic::loggedOutFromServer()
{
   IdPartyList idPartyList = clientPartyModelPtr_->getIdPartyList();
   for (const auto& partyId : idPartyList)
   {
      ClientPartyPtr clientPartyPtr = clientPartyModelPtr_->getClientPartyById(partyId);

      if (!clientPartyPtr)
      {
         continue;
      }

      if (clientPartyPtr->isPrivateStandard())
      {
         clientPartyPtr->setClientStatus(ClientStatus::OFFLINE);
      }
   }
}

void ClientPartyLogic::onRecipientKeysHasChanged(const Chat::UserPublicKeyInfoList& userPkList)
{
   emit userPublicKeyChanged(userPkList);
}

void ClientPartyLogic::onRecipientKeysUnchanged()
{
   updateModelAndRefreshPartyDisplayNames();
}

void ClientPartyLogic::updateModelAndRefreshPartyDisplayNames()
{
   emit partyModelChanged();

   // parties loaded, check is party display name should be updated
   IdPartyList idPartyList = clientPartyModelPtr_->getIdPartyList();
   for (const auto& partyId : idPartyList)
   {
      clientDBServicePtr_->loadPartyDisplayName(partyId);
   }

}