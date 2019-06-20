#include "ChatUtils.h"

#include "ChatCommonTypes.h"
#include "Encryption/AEAD_Encryption.h"
#include "Encryption/AEAD_Decryption.h"
#include "Encryption/IES_Encryption.h"
#include "Encryption/IES_Decryption.h"
#include "botan/base64.h"

const char *ChatUtils::GlobalRoomKey = "global_chat";
const char *ChatUtils::OtcRoomKey = "otc_chat";
const char *ChatUtils::SupportRoomKey = "support_chat";

namespace {

   // Temporary function for the protobuf switch
   void copyMsgPlainFields(const Chat::Data_Message &msg, Chat::Data *dst)
   {
      *dst->mutable_message() = msg;
      dst->mutable_message()->clear_message();
      dst->mutable_message()->clear_nonce();
   }

   std::string validateUtf8(const Botan::SecureVector<uint8_t> &data)
   {
      std::string result = QString::fromUtf8(reinterpret_cast<const char*>(data.data()), data.size()).toStdString();
      if (result.size() != data.size() && !std::equal(data.begin(), data.end(), result.begin())) {
         throw std::runtime_error("invalid utf text detected");
      }
      return result;
   }

} // namespace

QString ChatUtils::toString(Chat::OtcRangeType value)
{
   return QString::fromStdString(bs::network::OTCRangeID::toString(bs::network::OTCRangeID::Type(value)));
}

QString ChatUtils::toString(Chat::OtcSide value)
{
   return QString::fromStdString(bs::network::ChatOTCSide::toString(bs::network::ChatOTCSide::Type(value)));
}

bool ChatUtils::messageFlagRead(const Chat::Data_Message &msg, Chat::Data_Message_State flag)
{
   return msg.state() & uint32_t(flag);
}

void ChatUtils::messageFlagSet(Chat::Data_Message *msg, Chat::Data_Message_State state)
{
   msg->set_state(msg->state() | uint32_t(state));
}

void ChatUtils::registerTypes()
{
   qRegisterMetaType<std::shared_ptr<Chat::Data>>();
   qRegisterMetaType<std::vector<std::shared_ptr<Chat::Data>>>();
   qRegisterMetaType<Chat::ContactStatus>();
   qRegisterMetaType<Chat::UserStatus>();
}

size_t ChatUtils::defaultNonceSize()
{
   return 24;
}

std::shared_ptr<Chat::Data> ChatUtils::encryptMessageIes(const std::shared_ptr<spdlog::logger> &logger
   , const Chat::Data_Message &msg, const BinaryData &pubKey)
{
   try {
      auto cipher = Encryption::IES_Encryption::create(logger);

      cipher->setPublicKey(pubKey);
      cipher->setData(msg.message());

      Botan::SecureVector<uint8_t> output;
      cipher->finish(output);

      auto result = std::make_shared<Chat::Data>();

      copyMsgPlainFields(msg, result.get());

      result->mutable_message()->set_encryption(Chat::Data_Message_Encryption_IES);
      result->mutable_message()->set_message(Botan::base64_encode(output));

      return result;
   }
   catch (std::exception & e) {
      logger->error("[ChatUtils::{}] failed: {}", __func__, e.what());
      return nullptr;
   }
}

std::shared_ptr<Chat::Data> ChatUtils::encryptMessageAead(const std::shared_ptr<spdlog::logger> &logger
   , const Chat::Data_Message &msg, const BinaryData &remotePubKey, const SecureBinaryData &privKey
   , const BinaryData &nonce)
{
   try {
      auto cipher = Encryption::AEAD_Encryption::create(logger);

      cipher->setPrivateKey(privKey);
      cipher->setPublicKey(remotePubKey);
      cipher->setNonce(Botan::SecureVector<uint8_t>(nonce.getPtr(), nonce.getPtr() + nonce.getSize()));
      cipher->setData(msg.message());

      Botan::SecureVector<uint8_t> output;
      cipher->finish(output);

      auto result = std::make_shared<Chat::Data>();

      copyMsgPlainFields(msg, result.get());

      result->mutable_message()->set_encryption(Chat::Data_Message_Encryption_AEAD);
      result->mutable_message()->set_nonce(nonce.getPtr(), nonce.getSize());
      result->mutable_message()->set_message(Botan::base64_encode(output));

      return result;
   }
   catch (std::exception & e) {
      logger->error("[ChatUtils::{}] failed: {}", __func__, e.what());
      return nullptr;
   }
}

std::shared_ptr<Chat::Data> ChatUtils::decryptMessageIes(const std::shared_ptr<spdlog::logger> &logger
   , const Chat::Data_Message &msg, const SecureBinaryData &privKey)
{
   try {
      auto decipher = Encryption::IES_Decryption::create(logger);

      decipher->setPrivateKey(privKey);

      auto data = Botan::base64_decode(msg.message());
      decipher->setData(std::string(data.begin(), data.end()));

      Botan::SecureVector<uint8_t> output;
      decipher->finish(output);

      auto result = std::make_shared<Chat::Data>();

      copyMsgPlainFields(msg, result.get());

      result->mutable_message()->set_encryption(Chat::Data_Message_Encryption_UNENCRYPTED);
      result->mutable_message()->set_message(validateUtf8(output));

      return result;
   }
   catch (std::exception & e) {
      logger->error("[ChatUtils::{}] failed: {}", __func__, e.what());
      return nullptr;
   }
}

std::shared_ptr<Chat::Data> ChatUtils::decryptMessageAead(const std::shared_ptr<spdlog::logger> &logger
   , const Chat::Data_Message &msg, const BinaryData &pubKey, const SecureBinaryData &privKey)
{
   try {
      auto decipher = Encryption::AEAD_Decryption::create(logger);

      Botan::SecureVector<uint8_t> output;
      auto data = Botan::base64_decode(msg.message());
      decipher->setData(std::string(data.begin(), data.end()));
      decipher->setPrivateKey(privKey);
      decipher->setPublicKey(pubKey);
      const std::string &nonce = msg.nonce();
      decipher->setNonce(Botan::SecureVector<uint8_t>(nonce.begin(), nonce.end()));

      decipher->finish(output);

      auto result = std::make_shared<Chat::Data>();

      copyMsgPlainFields(msg, result.get());

      result->mutable_message()->set_encryption(Chat::Data_Message_Encryption_UNENCRYPTED);
      result->mutable_message()->set_message(validateUtf8(output));

      return result;
   }
   catch (std::exception & e) {
      logger->error("[ChatUtils::{}] failed: {}", __func__, e.what());
      return nullptr;
   }
}