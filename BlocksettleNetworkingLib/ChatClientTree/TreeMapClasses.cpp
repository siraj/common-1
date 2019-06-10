#include "TreeMapClasses.h"

#include "TreeObjects.h"

#include <algorithm>
#include <QDebug>

bool RootItem::insertRoomObject(std::shared_ptr<Chat::RoomData> data)
{
   TreeItem* candidate =  new ChatRoomElement(data);
   bool res = insertNode(candidate);
   if (!res) {
      delete  candidate;
   }
   return  res;
}

bool RootItem::insertContactObject(std::shared_ptr<Chat::ContactRecordData> data, bool isOnline)
{
   ChatContactElement* candidate = new ChatContactElement(data);
   candidate->setOnlineStatus(isOnline
                              ? ChatContactElement::OnlineStatus::Online
                              : ChatContactElement::OnlineStatus::Offline);
   bool res = insertNode(candidate);
   if (!res) {
      delete candidate;
   }

   return  res;
}

bool RootItem::insertContactRequestObject(std::shared_ptr<Chat::ContactRecordData> data, bool isOnline)
{
   ChatContactRequestElement* candidate = new ChatContactRequestElement(data);

   bool res = insertNode(candidate);
   if (!res) {
      delete candidate;
   }

   return  res;
}

bool RootItem::insertGeneralUserObject(std::shared_ptr<Chat::UserData> data)
{
   TreeItem* candidate = new ChatUserElement(data);
   bool res = insertNode(candidate);
   if (!res) {
      delete  candidate;
   }
   return  res;
}

bool RootItem::insertSearchUserObject(std::shared_ptr<Chat::UserData> data)
{
   TreeItem* candidate = new ChatSearchElement(data);
   bool res = insertNode(candidate);
   if (!res) {
      delete  candidate;
   }
   return  res;
}


TreeItem * RootItem::resolveMessageTargetNode(DisplayableDataNode * messageNode)
{
   if (!messageNode){
      return nullptr;
   }

   for (auto categoryGroup : children_) {
      if (categoryGroup->isChildTypeSupported(messageNode->targetParentType_)) {
         return categoryGroup->findSupportChild(messageNode);
      }
   }

   return nullptr;
}

TreeItem* RootItem::findChatNode(const std::string &chatId)
{
   for (auto child : children_){ // through all categories
      if ( child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::RoomsElement)
        || child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::ContactsElement)
        || child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::ContactsRequestElement)) {
         for (auto cchild : child->getChildren()) {
            auto data = static_cast<CategoryElement*>(cchild)->getDataObject();
            switch (data->getType()) {
               case Chat::DataObject::Type::RoomData:{
                  auto room = std::dynamic_pointer_cast<Chat::RoomData>(data);
                  if (room->getId().toStdString() == chatId){
                     return cchild;
                  }
               }
                  break;
               case Chat::DataObject::Type::ContactRecordData: {
                  auto contact = std::dynamic_pointer_cast<Chat::ContactRecordData>(data);
                  if (contact->getContactId().toStdString() == chatId){
                     return cchild;
                  }
               }
                  break;
               default:
                  break;

            }
         }
      }

      if (child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::OTCSentResponsesElement)) {
         for (auto cchild : child->getChildren()) {
            auto sentResponse = static_cast<OTCSentResponseElement*>(cchild);
            if (sentResponse->otcId() == chatId) {
               return sentResponse;
            }

         }
      }

      if (child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::OTCReceivedResponsesElement)) {
         for (auto cchild : child->getChildren()) {
            auto receivedResponse = static_cast<OTCReceivedResponseElement*>(cchild);
            if (receivedResponse->otcId() == chatId) {
               return receivedResponse;
            }

         }
      }
   }
   return nullptr;
}

std::vector<std::shared_ptr<Chat::ContactRecordData> > RootItem::getAllContacts()
{
   std::vector<std::shared_ptr<Chat::ContactRecordData>> contacts;

   for (auto child : children_){ // through all categories
      if (child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::ContactsElement)) {
         for (auto cchild : child->getChildren()){
            auto data = static_cast<CategoryElement*>(cchild)->getDataObject();
            if (data->getType() == Chat::DataObject::Type::ContactRecordData) {
               auto contact = std::dynamic_pointer_cast<Chat::ContactRecordData>(data);
               contacts.push_back(contact);
            }
         }
      }
   }
   return  contacts;
}

bool RootItem::removeContactNode(const std::string &contactId)
{
   for (auto child : children_){ // through all categories
      if (child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::ContactsElement)) {
         for (auto cchild : child->getChildren()){
            auto data = static_cast<CategoryElement*>(cchild)->getDataObject();
            if (data->getType() == Chat::DataObject::Type::ContactRecordData) {
               auto contact = std::dynamic_pointer_cast<Chat::ContactRecordData>(data);
               if (contact->getContactId().toStdString() == contactId) {
                  child->removeChild(cchild);
                  return true;
               }
            }
         }
      }
   }
   return false;
}

std::shared_ptr<Chat::ContactRecordData> RootItem::findContactItem(const std::string &contactId)
{
   CategoryElement* elementNode = findContactNode(contactId);
   if (elementNode){
      switch (elementNode->getType()) {
         case ChatUIDefinitions::ChatTreeNodeType::ContactsElement:
            return static_cast<ChatContactElement*>(elementNode)->getContactData();
         case ChatUIDefinitions::ChatTreeNodeType::ContactsRequestElement:
            return static_cast<ChatContactRequestElement*>(elementNode)->getContactData();
         default:
            return nullptr;

      }

   }
   return  nullptr;
}

CategoryElement *RootItem::findContactNode(const std::string &contactId)
{
   TreeItem* chatNode = findChatNode(contactId);
   if (chatNode && chatNode->getType() == ChatUIDefinitions::ChatTreeNodeType::ContactsElement){
      return static_cast<ChatContactElement*> (chatNode);
   } else if (chatNode && chatNode->getType() == ChatUIDefinitions::ChatTreeNodeType::ContactsRequestElement) {
      return static_cast<ChatContactRequestElement*> (chatNode);
   }

   return nullptr;
}

std::shared_ptr<Chat::MessageData> RootItem::findMessageItem(const std::string &chatId, const std::string &messgeId)
{
   TreeItem* chatNode = findChatNode(chatId);
   if (chatNode && chatNode->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::MessageDataNode)) {
         for (auto child : chatNode->getChildren()){
            auto message = std::dynamic_pointer_cast<Chat::MessageData>(static_cast<CategoryElement*>(child)->getDataObject());
            if (message && message->id().toStdString() == messgeId){
               return message;
            }
         }
   }
   return  nullptr;
}

void RootItem::clear()
{
   for (auto child : children_) {
      child->deleteChildren();
   }
   currentUser_.clear();
}

void RootItem::clearSearch()
{
   for (auto child : children_) {
      if (child->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::SearchElement))
         child->deleteChildren();
   }
}

std::string RootItem::currentUser() const
{
   return currentUser_;
}

bool RootItem::insertMessageNode(DisplayableDataNode * messageNode)
{
   for (auto categoryGroup : children_ ) {
      if (categoryGroup->isChildTypeSupported(messageNode->targetParentType_)) {
         auto targetElement = categoryGroup->findSupportChild(messageNode);
         if (targetElement != nullptr) {
            if (targetElement->insertItem(messageNode)) {
               emit itemChanged(targetElement);
               return true;
            }
            break;
         }
      }
   }

   return false;
}

bool RootItem::insertNode(TreeItem * item)
{
   TreeItem * supportChild = findSupportChild(item);
   if (supportChild) {
      return supportChild->insertItem(item);
   }

   return false;
}

TreeItem *RootItem::findCategoryNodeWith(ChatUIDefinitions::ChatTreeNodeType type)
{
   auto found = std::find_if(children_.begin(), children_.end(),
                                       [type](TreeItem* child){
      return child->isChildTypeSupported(type);
   });

   if (found != children_.end()) {
      return *found;
   }
   return nullptr;
}


void RootItem::setCurrentUser(const std::string &currentUser)
{
   currentUser_ = currentUser;
}

void RootItem::notifyMessageChanged(std::shared_ptr<Chat::MessageData> message)
{
   if (message) {
      QString chatId = message->senderId() == QString::fromStdString(currentUser())
                       ? message->receiverId()
                       : message->senderId();

      TreeItem* chatNode = findChatNode(chatId.toStdString());
      if (chatNode == nullptr) {
         chatId = message->receiverId();
         chatNode = findChatNode(chatId.toStdString());
      }

      if (chatNode && chatNode->isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::MessageDataNode)) {
         for (auto child : chatNode->getChildren()) {
            CategoryElement * elem = static_cast<CategoryElement*>(child);
            switch (elem->getDataObject()->getType()) {
               case Chat::DataObject::Type::MessageData: {
                  auto msg = std::dynamic_pointer_cast<Chat::MessageData>(elem->getDataObject());
                  if (message->id() == msg->id()) {
                     emit itemChanged(chatNode);
                  }
               } break;
               default:
                  emit itemChanged(chatNode);
                  break;
            }
         }
      }
   }
}

void RootItem::notifyContactChanged(std::shared_ptr<Chat::ContactRecordData> contact)
{
   TreeItem* chatNode = findChatNode(contact->getContactId().toStdString());
   if (chatNode && chatNode->getType() == ChatUIDefinitions::ChatTreeNodeType::ContactsElement){
      emit itemChanged(chatNode);
   }
}

bool CategoryElement::updateNewItemsFlag()
{
   if (!isChildTypeSupported(ChatUIDefinitions::ChatTreeNodeType::MessageDataNode)) {
      return false;
   }
   //Reset flag
   newItemsFlag_ = false;


   for (const auto child : children_) {
      auto messageNode = dynamic_cast<TreeMessageNode*>(child);

      if (!messageNode) {
         return false;
      }

      auto message = messageNode->getMessage();
      const RootItem * root = static_cast<const RootItem*>(recursiveRoot());
      if (message
          && !message->testFlag(Chat::MessageData::State::Read)
          && root->currentUser() != message->senderId().toStdString()) {
         newItemsFlag_ = true;
         break; //If first is found, no reason to continue
      }
   }
   return newItemsFlag_;
}

bool CategoryElement::getNewItemsFlag() const
{
   return newItemsFlag_;
}

// insert channel for response that client send to OTC requests
bool RootItem::insertOTCSentResponseObject(const std::shared_ptr<Chat::OTCResponseData> &response)
{
   auto otcRequestNode = new OTCSentResponseElement(response);
   bool insertResult = insertNode(otcRequestNode);
   if (!insertResult) {
      delete otcRequestNode;
   }

   qDebug() << "Sent response added";
   return insertResult;
}

// insert channel for response client receive for own OTC
bool RootItem::insertOTCReceivedResponseObject(const std::shared_ptr<Chat::OTCResponseData> &response)
{
   auto otcRequestNode = new OTCReceivedResponseElement(response);
   bool insertResult = insertNode(otcRequestNode);
   if (!insertResult) {
      delete otcRequestNode;
   }

   qDebug() << "Received response added";

   return insertResult;
}

