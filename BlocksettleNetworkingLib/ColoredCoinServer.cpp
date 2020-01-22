/*

***********************************************************************************
* Copyright (C) 2016 - , BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/

#include "ColoredCoinServer.h"

#include "ColoredCoinCache.h"
#include "ColoredCoinLogic.h"
#include "DispatchQueue.h"
#include "StringUtils.h"
#include "ZMQ_BIP15X_DataConnection.h"
#include "ZMQ_BIP15X_ServerConnection.h"

#include "tracker_server.pb.h"

namespace {

   bool isValidTrackerAddr(const std::string &str)
   {
      try {
         auto addr = bs::Address::fromAddressString(str);
         // Limit to P2WPKH only
         if (addr.getType() != AddressEntryType_P2WPKH) {
            return false;
         }
      } catch (...) {
         return false;
      }
      return true;
   }

   bool isValidTrackerKey(const bs::tracker_server::TrackerKey &trackerKey)
   {
      if (trackerKey.coins_per_share() <= 0 || trackerKey.coins_per_share() > 100000000) {
         return false;
      }
      if (trackerKey.origin_addresses().empty()) {
         return false;
      }
      bool result = std::is_sorted(trackerKey.origin_addresses().begin(), trackerKey.origin_addresses().end());
      if (!result) {
         return false;
      }
      result = std::is_sorted(trackerKey.revoked_addresses().begin(), trackerKey.revoked_addresses().end());
      if (!result) {
         return false;
      }
      for (const auto &addr : trackerKey.origin_addresses()) {
         if (!isValidTrackerAddr(addr)) {
            return false;
         }
      }
      for (const auto &addr : trackerKey.revoked_addresses()) {
         if (!isValidTrackerAddr(addr)) {
            return false;
         }
      }
      return true;
   }

} // namespace

class CcTrackerImpl : public ColoredCoinTrackerInterface
{
public:
   ~CcTrackerImpl() override
   {
      parent_->removeClient(this);
   }

   void addOriginAddress(const bs::Address &addr) override
   {
      if (isOnline_.load()) {
         throw std::logic_error("addOriginAddress is not implemented after goOnline call");
      }
      originAddresses_.push_back(addr);
   }

   void addRevocationAddress(const bs::Address &addr) override
   {
      if (isOnline_.load()) {
         throw std::logic_error("addOriginAddress is not implemented after goOnline call");
      }
      revocationAddresses_.push_back(addr);
   }

   bool goOnline() override
   {
      if (isOnline_.load()) {
         throw std::logic_error("goOnline was already called");
      }
      isOnline_ = true;
      parent_->registerClient(this);
      return true;
   }

   std::shared_ptr<ColoredCoinSnapshot> snapshot() const override
   {
      return std::atomic_load_explicit(&snapshot_, std::memory_order_acquire);
   }

   std::shared_ptr<ColoredCoinZCSnapshot> zcSnapshot() const override
   {
      return std::atomic_load_explicit(&zcSnapshot_, std::memory_order_acquire);
   }

   CcTrackerClient *parent_{};
   uint64_t coinsPerShare_;
   std::atomic_bool isOnline_{false};
   std::vector<bs::Address> originAddresses_;
   std::vector<bs::Address> revocationAddresses_;
   bool registered_{false};
   int id_{};

   std::shared_ptr<ColoredCoinSnapshot> snapshot_;
   std::shared_ptr<ColoredCoinZCSnapshot> zcSnapshot_;

};

class CcTrackerSrvImpl : public ColoredCoinTracker
{
public:
   struct Client
   {
      std::string clientId;
      int id{};

      bool operator<(const Client &other) const {
         if (clientId != other.clientId) {
            return clientId < other.clientId;
         }
         return id < other.id;
      }
   };

   CcTrackerSrvImpl(CcTrackerServer *parent, uint64_t coinsPerShare, std::shared_ptr<ArmoryConnection> connPtr)
      : ColoredCoinTracker(coinsPerShare, connPtr)
      , parent_(parent)
   {
   }

   void snapshotUpdated() override
   {
      parent_->dispatchQueue_.dispatch([this] {
         auto s = snapshot();
         ccSnapshot_ = serializeColoredCoinSnapshot(s);
         for (const auto &client : clients_) {
            sendSnapshot(client);
         }
      });
   }

   virtual void zcSnapshotUpdated() override
   {
      parent_->dispatchQueue_.dispatch([this] {
         auto s = zcSnapshot();
         ccZcSnapshot_ = serializeColoredCoinZcSnapshot(s);
         for (const auto &client : clients_) {
            sendZcSnapshot(client);
         }
      });
   }

   void sendSnapshot(const Client &client) {
      bs::tracker_server::Response response;
      auto d = response.mutable_update_cc_snapshot();
      d->set_id(client.id);
      d->set_data(ccSnapshot_);
      parent_->server_->SendDataToClient(client.clientId, response.SerializeAsString());
   }

   void sendZcSnapshot(const Client &client) {
      bs::tracker_server::Response response;
      auto d = response.mutable_update_cc_zc_snapshot();
      d->set_id(client.id);
      d->set_data(ccZcSnapshot_);
      parent_->server_->SendDataToClient(client.clientId, response.SerializeAsString());
   }

   std::string ccSnapshot_;
   std::string ccZcSnapshot_;

   std::set<Client> clients_;

   CcTrackerServer *parent_{};
};

CcTrackerClient::CcTrackerClient(const std::shared_ptr<spdlog::logger> &logger)
   : logger_(logger)
{
   dispatchThread_ = std::thread([this]{
      while (!dispatchQueue_.done()) {
         dispatchQueue_.tryProcess();
      }
   });
}

CcTrackerClient::~CcTrackerClient()
{
   dispatchQueue_.quit();
   dispatchThread_.join();
}

std::unique_ptr<ColoredCoinTrackerInterface> CcTrackerClient::createClient(uint64_t coinsPerShare)
{
   auto id = ++nextId_;

   auto client = std::make_unique<CcTrackerImpl>();
   client->parent_ = this;
   client->coinsPerShare_ = coinsPerShare;
   client->id_ = id;

   return std::move(client);
}

void CcTrackerClient::openConnection(const std::string &host, const std::string &port, ZmqBipNewKeyCb newKeyCb)
{
   dispatchQueue_.dispatch([this, host, port, newKeyCb = std::move(newKeyCb)] {
      ZmqBIP15XDataConnectionParams params;
      params.ephemeralPeers = true;
      connection_ = std::make_unique<ZmqBIP15XDataConnection>(logger_, params);
      connection_->setCBs(std::move(newKeyCb));
      connection_->openConnection(host, port, this);
   });
}

void CcTrackerClient::OnDataReceived(const std::string &data)
{
   bs::tracker_server::Response response;
   bool result = response.ParseFromString(data);
   if (!result) {
      SPDLOG_LOGGER_ERROR(logger_, "can't parse bs::tracker_server::Response");
      return;
   }

   dispatchQueue_.dispatch([this, response = std::move(response)] {
      switch (response.data_case()) {
         case bs::tracker_server::Response::kUpdateCcSnapshot:
            processUpdateCcSnapshot(response.update_cc_snapshot());
            return;
         case bs::tracker_server::Response::kUpdateCcZcSnapshot:
            processUpdateCcZcSnapshot(response.update_cc_zc_snapshot());
            return;
         case bs::tracker_server::Response::DATA_NOT_SET:
            SPDLOG_LOGGER_ERROR(logger_, "git invalid empty response from server");
            return;
      }

      SPDLOG_LOGGER_CRITICAL(logger_, "unhandled request detected!");
   });
}

void CcTrackerClient::OnConnected()
{
   dispatchQueue_.dispatch([this] {
      for (auto &client : clients_) {
         bs::tracker_server::Request request;

         auto d = request.mutable_register_cc();
         d->set_id(client->id_);

         d->mutable_tracker_key()->set_coins_per_share(client->coinsPerShare_);
         std::set<std::string> originAddresses;
         for (const auto &addr : client->originAddresses_) {
            originAddresses.insert(addr.display());
         }
         std::set<std::string> revocationAddresses;
         for (const auto &addr : client->revocationAddresses_) {
            revocationAddresses.insert(addr.display());
         }
         for (const auto &addr : originAddresses) {
            d->mutable_tracker_key()->add_origin_addresses(addr);
         }
         for (const auto &addr : revocationAddresses) {
            d->mutable_tracker_key()->add_revoked_addresses(addr);
         }

         if (!isValidTrackerKey(d->tracker_key())) {
            throw std::logic_error("invalid tracker key");
         }

         connection_->send(request.SerializeAsString());
      }
   });
}

void CcTrackerClient::OnDisconnected()
{
}

void CcTrackerClient::OnError(DataConnectionListener::DataConnectionError errorCode)
{
}

void CcTrackerClient::registerClient(CcTrackerImpl *client)
{
   dispatchQueue_.dispatch([this, client] {
      clients_.insert(client);
      clientsById_[client->id_] = client;
   });
}

void CcTrackerClient::removeClient(CcTrackerImpl *client)
{
   dispatchQueue_.dispatch([this, client] {
      clientsById_.erase(client->id_);
      clients_.erase(client);
   });
}

void CcTrackerClient::processUpdateCcSnapshot(const bs::tracker_server::Response_UpdateCcSnapshot &response)
{
   auto it = clientsById_.find(response.id());
   if (it == clientsById_.end()) {
      SPDLOG_LOGGER_ERROR(logger_, "unknown id");
      return;
   }
   CcTrackerImpl *tracker = it->second;
   auto snapshot = deserializeColoredCoinSnapshot(response.data());
   std::atomic_store_explicit(&tracker->snapshot_, snapshot, std::memory_order_release);
}

void CcTrackerClient::processUpdateCcZcSnapshot(const bs::tracker_server::Response_UpdateCcZcSnapshot &response)
{
   auto it = clientsById_.find(response.id());
   if (it == clientsById_.end()) {
      SPDLOG_LOGGER_ERROR(logger_, "unknown id");
      return;
   }
   CcTrackerImpl *tracker = it->second;
   auto snapshot = deserializeColoredCoinZcSnapshot(response.data());
   std::atomic_store_explicit(&tracker->zcSnapshot_, snapshot, std::memory_order_release);
}

CcTrackerServer::CcTrackerServer(const std::shared_ptr<spdlog::logger> &logger
   , const std::shared_ptr<ArmoryConnection> &armory)
   : logger_(logger)
   , armory_(armory)
{
   dispatchThread_ = std::thread([this]{
      while (!dispatchQueue_.done()) {
         dispatchQueue_.tryProcess();
      }
   });
}

CcTrackerServer::~CcTrackerServer()
{
   dispatchQueue_.quit();
   dispatchThread_.join();
}

bool CcTrackerServer::startServer(const std::string &host, const std::string &port
   , const std::shared_ptr<ZmqContext> &context
   , const std::string &ownKeyFileDir, const std::string &ownKeyFileName)
{
   auto cbTrustedClients = []() -> ZmqBIP15XPeers{
      return {};
   };
   server_ = std::make_unique<ZmqBIP15XServerConnection>(logger_, context, cbTrustedClients, ownKeyFileDir, ownKeyFileName);
   bool result = server_->BindConnection(host, port, this);
   return result;
}

void CcTrackerServer::OnDataFromClient(const std::string &clientId, const std::string &data)
{
   bs::tracker_server::Request request;
   bool result = request.ParseFromString(data);
   if (!result) {
      SPDLOG_LOGGER_ERROR(logger_, "can't parse bs::tracker_server::Request from client {}", bs::toHex(clientId));
      return;
   }

   dispatchQueue_.dispatch([this, clientId, request = std::move(request)] {
      auto &client = connectedClients_.at(clientId);

      switch (request.data_case()) {
         case bs::tracker_server::Request::kRegisterCc:
            processRegisterCc(client, request.register_cc());
            return;
         case bs::tracker_server::Request::DATA_NOT_SET:
            SPDLOG_LOGGER_ERROR(logger_, "invalid request from client {}", bs::toHex(clientId));
            return;
      }

      // Should not happen unless there is some new data_case
      SPDLOG_LOGGER_CRITICAL(logger_, "unhandled request detected!");
   });
}

void CcTrackerServer::OnClientConnected(const std::string &clientId)
{
   SPDLOG_LOGGER_INFO(logger_, "new client connected: {}", bs::toHex(clientId));

   dispatchQueue_.dispatch([this, clientId] {
      auto &client = connectedClients_[clientId];
      client.clientId = clientId;
   });
}

void CcTrackerServer::OnClientDisconnected(const std::string &clientId)
{
   SPDLOG_LOGGER_INFO(logger_, "client disconnected: {}", bs::toHex(clientId));

   dispatchQueue_.dispatch([this, clientId] {
      connectedClients_.erase(clientId);
   });
}

void CcTrackerServer::processRegisterCc(CcTrackerServer::ClientData &client, const bs::tracker_server::Request_RegisterCc &request)
{
   auto it = client.trackers.find(request.id());
   if (it != client.trackers.end()) {
      SPDLOG_LOGGER_ERROR(logger_, "request with duplicated tracker id is ignored");
      return;
   }

   if (!isValidTrackerKey(request.tracker_key())) {
      SPDLOG_LOGGER_ERROR(logger_, "invalid tracker_key");
      return;
   }

   auto trackerKey = request.tracker_key().SerializeAsString();
   auto &trackerWeakPtr = trackers_[trackerKey];
   auto trackerPtr = trackerWeakPtr.lock();

   auto c = CcTrackerSrvImpl::Client{client.clientId, request.id()};

   if (!trackerPtr) {
      trackerPtr = std::make_shared<CcTrackerSrvImpl>(this, request.tracker_key().coins_per_share(), armory_);

      for (const auto &addr : request.tracker_key().origin_addresses()) {
         trackerPtr->addOriginAddress(bs::Address::fromAddressString(addr));
      }
      for (const auto &addr : request.tracker_key().revoked_addresses()) {
         trackerPtr->addRevocationAddress(bs::Address::fromAddressString(addr));
      }

      bool result = trackerPtr->goOnline();

      if (!result) {
         // FIXME: decide what to do
         SPDLOG_LOGGER_ERROR(logger_, "goOnline failed");
         return;
      }
   }

   client.trackers[request.id()] = trackerPtr;

   trackerPtr->clients_.insert(c);
   trackerPtr->sendSnapshot(c);
   trackerPtr->sendZcSnapshot(c);
}
