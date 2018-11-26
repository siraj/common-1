#include "ArmoryEventsSubscriber.h"

#include "ConnectionManager.h"

#include "armory_events.pb.h"

ArmoryEventsSubscriber::ArmoryEventsSubscriber(const std::string& name
      , const std::shared_ptr<spdlog::logger>& logger)
 : name_{name}
 , logger_{logger}
{}

ArmoryEventsSubscriber::~ArmoryEventsSubscriber() noexcept
{
   UnsubscribeFromEvents();
}

void ArmoryEventsSubscriber::SetNewBlockCallback(const onNewBlockCB& cb)
{
   onNewBlock_ = cb;
}

void ArmoryEventsSubscriber::SetZCCallback(const onZCEventCB& cb)
{
   onZCEvent_ = cb;
}

void ArmoryEventsSubscriber::UnsubscribeFromEvents()
{
   onNewBlock_ = {};
   onZCEvent_ = {};
}

bool ArmoryEventsSubscriber::SubscribeToArmoryEvents(const std::shared_ptr<ConnectionManager>& connectionManager)
{
   logger_->debug("[ArmoryEventsSubscriber::SubscribeToArmoryEvents] start subscription for {}"
      , name_);

   if (subConection_ != nullptr) {
      logger_->error("[ArmoryEventsSubscriber::SubscribeToArmoryEvents] {} :subscription already active"
         , name_);
      return false;
   }

   subConection_ = connectionManager->CreateSubscriberConnection();
   if (!subConection_->ConnectToPublisher("armory_events", this)) {
      logger_->error("[ArmoryEventsSubscriber::SubscribeToArmoryEvents] {} : failed to subscribe"
         , name_);
      subConection_ = nullptr;
      return false;
   }

   return true;
}

void ArmoryEventsSubscriber::OnDataReceived(const std::string& data)
{
   Blocksettle::ArmoryEvents::EventHeader    header;

   if (!header.ParseFromString(data)) {
      logger_->error("[ArmoryEventsSubscriber::OnDataReceived] failed to parse header");
      return;
   }

   switch (header.event_type()) {
   case Blocksettle::ArmoryEvents::NewBlockEventType:
      ProcessNewBlockEvent(header.event_data());
      break;
   case Blocksettle::ArmoryEvents::ZCEventType:
      ProcessZCEvent(header.event_data());
      break;
   default:
      logger_->debug("[ArmoryEventsSubscriber::OnDataReceived] unsupported event {}"
         , header.event_type());
      break;
   }
}

void ArmoryEventsSubscriber::OnConnected()
{
   logger_->debug("[ArmoryEventsSubscriber::OnConnected] {} connected to armory events"
      , name_);
}

void ArmoryEventsSubscriber::OnDisconnected()
{
   logger_->debug("[ArmoryEventsSubscriber::OnDisconnected] {} disconnected to armory events"
      , name_);
}

void ArmoryEventsSubscriber::ProcessNewBlockEvent(const std::string& eventData)
{
   Blocksettle::ArmoryEvents::NewBlockEvent message;
   if (!message.ParseFromString(eventData)) {
      logger_->error("[ArmoryEventsSubscriber::ProcessNewBlockEvent] failed to parse event data");
      return;
   }

   if (onNewBlock_) {
      onNewBlock_(message.height());
   } else {
      logger_->debug("[ArmoryEventsSubscriber::ProcessNewBlockEvent] {} do not have new block handler"
         , name_);
   }
}

void ArmoryEventsSubscriber::ProcessZCEvent(const std::string& eventData)
{
   Blocksettle::ArmoryEvents::ZCEvent message;
   if (!message.ParseFromString(eventData)) {
      logger_->error("[ArmoryEventsSubscriber::ProcessZCEvent] failed to parse event data");
      return;
   }

   if (onZCEvent_) {
      onZCEvent_(message.zc_id());
   } else {
      logger_->debug("[ArmoryEventsSubscriber::ProcessZCEvent] {} do not have ZC handler"
         , name_);
   }
}