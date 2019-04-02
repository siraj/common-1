#ifndef __ZMQ_BIP15X_DATACONNECTION_H__
#define __ZMQ_BIP15X_DATACONNECTION_H__

#include <functional>
#include <QObject>
#include <spdlog/spdlog.h>
#include "ArmoryServersProvider.h"
#include "AuthorizedPeers.h"
#include "BIP150_151.h"
#include "ZmqDataConnection.h"
#include "ZMQ_BIP15X_Msg.h"

#define CLIENT_AUTH_PEER_FILENAME "client.peers"

class ZmqBIP15XDataConnection : public QObject, public ZmqDataConnection
{
   Q_OBJECT
public:
   ZmqBIP15XDataConnection(const std::shared_ptr<spdlog::logger>& logger
      , const bool& ephemeralPeers = false, bool monitored = false);
   ZmqBIP15XDataConnection(const std::shared_ptr<spdlog::logger>& logger
      , const ArmoryServersProvider& trustedServer, const bool& ephemeralPeers
      , bool monitored);
   ZmqBIP15XDataConnection(const ZmqBIP15XDataConnection&) = delete;
   ZmqBIP15XDataConnection& operator= (const ZmqBIP15XDataConnection&) = delete;
   ZmqBIP15XDataConnection(ZmqBIP15XDataConnection&&) = delete;
   ZmqBIP15XDataConnection& operator= (ZmqBIP15XDataConnection&&) = delete;

   // Overridden functions from ZmqDataConnection.
   bool send(const std::string& data) override;
   bool closeConnection() override;
   bool startBIP151Handshake(const std::function<void()> &cbCompleted = nullptr);

   SecureBinaryData getOwnPubKey() const;

signals:
   void bip15XCompleted(); // BIP 150 & 151 handshakes completed.

protected:
   bool handshakeCompleted() {
      return (bip150HandshakeCompleted_ && bip151HandshakeCompleted_);
   }

   // Overridden functions from ZmqDataConnection.
   void onRawDataReceived(const std::string& rawData) override;
   ZmqContext::sock_ptr CreateDataSocket() override;
   bool recvData() override;

private:
   void ProcessIncomingData(BinaryData& payload);
   bool processAEADHandshake(const ZmqBIP15XMsgPartial& msgObj);
   void promptUser(const BinaryDataRef& newKey, const std::string& srvAddrPort);
   AuthPeersLambdas getAuthPeerLambda() const;

   std::shared_ptr<std::promise<bool>> serverPubkeyProm_;
   std::shared_ptr<AuthorizedPeers> authPeers_;
   std::shared_ptr<BIP151Connection> bip151Connection_;
   std::chrono::time_point<std::chrono::system_clock> outKeyTimePoint_;
   uint32_t outerRekeyCount_ = 0;
   uint32_t innerRekeyCount_ = 0;
   ZmqBIP15XMsgFragments currentReadMessage_;
   BinaryData leftOverData_;
   std::atomic_flag lockSocket_ = ATOMIC_FLAG_INIT;
   bool bip150HandshakeCompleted_ = false;
   bool bip151HandshakeCompleted_ = false;
   uint32_t msgID_ = 0;
   std::function<void()>   cbCompleted_ = nullptr;
};

#endif // __ZMQ_BIP15X_DATACONNECTION_H__