#include <bech32/ref/c++/segwit_addr.h>
#include "CheckRecipSigner.h"
#include "CoinSelection.h"
#include "Wallets.h"
#include "CoreHDNode.h"    // only for calculating walletId in Seed
#include "CoreWallet.h"

#define SAFE_NUM_CONFS        6
#define ASSETMETA_PREFIX      0xAC

using namespace bs::core;

std::shared_ptr<wallet::AssetEntryMeta> wallet::AssetEntryMeta::deserialize(int, BinaryDataRef value)
{
   BinaryRefReader brr(value);

   const auto type = brr.get_uint8_t();
   if ((type == wallet::AssetEntryMeta::Comment) && (brr.getSizeRemaining() > 0)) {
      auto aeComment = std::make_shared<wallet::AssetEntryComment>();
      return (aeComment->deserialize(brr) ? aeComment : nullptr);
   }
   throw AssetException("unknown metadata type");
   return nullptr;
}


BinaryData wallet::AssetEntryComment::serialize() const
{
   BinaryWriter bw;
   bw.put_uint8_t(type());

   bw.put_var_int(key_.getSize());
   bw.put_BinaryData(key_);

   bw.put_var_int(comment_.length());
   bw.put_BinaryData(comment_);

   BinaryWriter finalBw;
   finalBw.put_var_int(bw.getSize());
   finalBw.put_BinaryData(bw.getData());
   return finalBw.getData();
}

bool wallet::AssetEntryComment::deserialize(BinaryRefReader brr)
{
   uint64_t len = brr.get_var_int();
   key_ = BinaryData(brr.get_BinaryData(len));

   len = brr.get_var_int();
   comment_ = BinaryData(brr.get_BinaryDataRef(len)).toBinStr();
   return true;
}


void wallet::MetaData::set(const std::shared_ptr<AssetEntryMeta> &value)
{
   data_[value->key()] = value;
}

void wallet::MetaData::write(const std::shared_ptr<LMDBEnv> env, LMDB *db)
{
   if (!env || !db) {
      return;
   }
   for (const auto value : data_) {
      if (!value.second->needsCommit()) {
         continue;
      }
      const auto itData = data_.find(value.first);
      const bool exists = (itData != data_.end());
      auto id = value.second->getIndex();
      if (exists) {
         id = itData->second->getIndex();
      }

      auto&& serializedEntry = value.second->serialize();

      BinaryWriter bw;
      bw.put_uint8_t(ASSETMETA_PREFIX);
      bw.put_uint32_t(id);
      auto &&dbKey = bw.getData();

      CharacterArrayRef keyRef(dbKey.getSize(), dbKey.getPtr());
      CharacterArrayRef dataRef(serializedEntry.getSize(), serializedEntry.getPtr());
      {
         LMDBEnv::Transaction tx(env.get(), LMDB::ReadWrite);
         db->insert(keyRef, dataRef);
      }
      value.second->doNotCommit();
   }
}

void wallet::MetaData::readFromDB(const std::shared_ptr<LMDBEnv> env, LMDB *db)
{
   if ((env == nullptr) || (db == nullptr)) {
      throw WalletException("LMDB is not initialized");
   }
   LMDBEnv::Transaction tx(env.get(), LMDB::ReadOnly);

   auto dbIter = db->begin();

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ASSETMETA_PREFIX);
   CharacterArrayRef keyRef(bwKey.getSize(), bwKey.getData().getPtr());

   dbIter.seek(keyRef, LMDB::Iterator::Seek_GE);

   while (dbIter.isValid()) {
      auto iterkey = dbIter.key();
      auto itervalue = dbIter.value();

      BinaryDataRef keyBDR((uint8_t*)iterkey.mv_data, iterkey.mv_size);
      BinaryDataRef valueBDR((uint8_t*)itervalue.mv_data, itervalue.mv_size);

      BinaryRefReader brrVal(valueBDR);
      auto valsize = brrVal.get_var_int();
      if (valsize != brrVal.getSizeRemaining()) {
         throw WalletException("entry val size mismatch");
      }

      try {
         BinaryRefReader brrKey(keyBDR);

         auto prefix = brrKey.get_uint8_t();
         if (prefix != ASSETMETA_PREFIX) {
            throw AssetException("invalid prefix");
         }
         auto index = brrKey.get_int32_t();
         nbMetaData_ = index + 1;

         auto entryPtr = AssetEntryMeta::deserialize(index, brrVal.get_BinaryDataRef((uint32_t)brrVal.getSizeRemaining()));
         if (entryPtr) {
            entryPtr->doNotCommit();
            data_[entryPtr->key()] = entryPtr;
         }
      }
      catch (AssetException& e) {
         LOGERR << e.what();
         break;
      }

      dbIter.advance();
   }
}


bool wallet::TXSignRequest::isValid() const noexcept
{
   if (!prevStates.empty()) {
      return true;
   }
   if (inputs.empty() || recipients.empty()) {
      return false;
   }
   return true;
}

Signer wallet::TXSignRequest::getSigner() const
{
   bs::CheckRecipSigner signer;

   if (!prevStates.empty()) {
      for (const auto &prevState : prevStates) {
         signer.deserializeState(prevState);
      }
   }
   else {
      signer.setFlags(SCRIPT_VERIFY_SEGWIT);

      for (const auto &utxo : inputs) {
         std::shared_ptr<ScriptSpender> spender;
/*         if (resolver) {
            spender = std::make_shared<ScriptSpender>(utxo, resolver);
         }
         else*/ if (populateUTXOs) {
            spender = std::make_shared<ScriptSpender>(utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue());
         }
         else {
            spender = std::make_shared<ScriptSpender>(utxo);
         }
         if (RBF) {
            spender->setSequence(UINT32_MAX - 2);
         }
         signer.addSpender(spender);
      }
   }
   if (populateUTXOs) {
      for (const auto &utxo : inputs) {
         try {
            signer.populateUtxo(utxo);
         }
         catch (const std::exception &) {}
      }
   }

   for (const auto& recipient : recipients) {
      signer.addRecipient(recipient);
   }

   if (change.value) {
      const auto changeRecip = change.address.getRecipient(change.value);
      if (changeRecip) {
         signer.addRecipient(changeRecip);
      }
   }
   signer.removeDupRecipients();

/*   if (resolver) {
      signer.setFeed(resolver);
   }*/
   return signer;
}

static std::vector<UTXO> decorateUTXOs(const std::vector<UTXO> &inUTXOs)
{
   std::vector<UTXO> inputUTXOs;
   if (inUTXOs.empty()) {
      return inputUTXOs;
   }
   inputUTXOs = inUTXOs;

   for (auto &utxo : inputUTXOs) {  // some kind of decoration code to replace the code above
      const bs::Address recipAddr(utxo.getRecipientScrAddr());
      utxo.txinRedeemSizeBytes_ = recipAddr.getInputSize();
      utxo.witnessDataSizeBytes_ = recipAddr.getWitnessDataSize();
      utxo.isInputSW_ = ((recipAddr.getType() == AddressEntryType_P2WPKH)
         || (recipAddr.getType() == AddressEntryType_P2WSH)) ? true : false;
   }
   return inputUTXOs;
}

static UtxoSelection computeSizeAndFee(const std::vector<UTXO> &inUTXOs, const PaymentStruct &inPS)
{
   auto usedUTXOCopy{ inUTXOs };
   UtxoSelection selection{ usedUTXOCopy };

   try {
      selection.computeSizeAndFee(inPS);
   } catch (...) {
   }
   return selection;
}

static size_t getVirtSize(const UtxoSelection &inUTXOSel)
{
   size_t nonWitSize = inUTXOSel.size_ - inUTXOSel.witnessSize_;
   return std::ceil(static_cast<float>(3 * nonWitSize + inUTXOSel.size_) / 4.0f);
}

size_t wallet::TXSignRequest::estimateTxVirtSize() const
{   // another implementation based on Armory and TransactionData code
   auto transactions = decorateUTXOs(inputs);
   std::map<unsigned int, std::shared_ptr<ScriptRecipient>> recipientsMap;
   for (unsigned int i = 0; i < recipients.size(); ++i) {
      recipientsMap[i] = recipients[i];
   }

   try {
      PaymentStruct payment(recipientsMap, fee, 0, 0);
      return getVirtSize(computeSizeAndFee(transactions, payment));
   }
   catch (const std::exception &) {}
   return 0;
}


bool wallet::TXMultiSignRequest::isValid() const noexcept
{
   if (inputs.empty() || recipients.empty()) {
      return false;
   }
   return true;
}


wallet::Seed::Seed(const std::string &seed, NetworkType netType)
   : netType_(netType)
{
   try {
      BinaryData base58In(seed);
      base58In.append('\0'); // Remove once base58toScrAddr() is fixed.
      seed_ = BtcUtils::base58toScrAddr(base58In);
   }
   catch (const std::exception &) {
      const auto result = segwit_addr::decode("seed", seed);
      if (result.first >= 0) {
         seed_ = BinaryData(&result.second[0], result.second.size());
      }
      if (seed_.isNull()) {
         seed_ = seed;
      }
   }
}

std::string wallet::Seed::getWalletId() const
{
   if (walletId_.empty()) {
      walletId_ = bs::core::hd::Node(*this).getId();
   }
   return walletId_;
}

EasyCoDec::Data wallet::Seed::toEasyCodeChecksum(size_t ckSumSize) const
{
   if (!hasPrivateKey()) {
      return {};
   }
   const size_t halfSize = privKey_.getSize() / 2;
   auto privKeyHalf1 = privKey_.getSliceCopy(0, (uint32_t)halfSize);
   auto privKeyHalf2 = privKey_.getSliceCopy(halfSize, (uint32_t)halfSize);
   const auto hash1 = BtcUtils::getHash256(privKeyHalf1);
   const auto hash2 = BtcUtils::getHash256(privKeyHalf2);
   privKeyHalf1.append(hash1.getSliceCopy(0, (uint32_t)ckSumSize));
   privKeyHalf2.append(hash2.getSliceCopy(0, (uint32_t)ckSumSize));
   const auto chkSumPrivKey = privKeyHalf1 + privKeyHalf2;
   return EasyCoDec().fromHex(chkSumPrivKey.toHexStr());
}

SecureBinaryData wallet::Seed::decodeEasyCodeChecksum(const EasyCoDec::Data &easyData, size_t ckSumSize)
{
   auto const privKeyHalf1 = decodeEasyCodeLineChecksum(easyData.part1, ckSumSize);
   auto const privKeyHalf2 = decodeEasyCodeLineChecksum(easyData.part2, ckSumSize);

   return (privKeyHalf1 + privKeyHalf2);
}

BinaryData wallet::Seed::decodeEasyCodeLineChecksum(
   const std::string& easyCodeHalf, size_t ckSumSize, size_t keyValueSize)
{
    const auto& hexStr = EasyCoDec().toHex(easyCodeHalf);
    const auto keyHalfWithChecksum = BinaryData::CreateFromHex(hexStr);

    size_t halfSize = keyValueSize + ckSumSize;

    if (keyHalfWithChecksum.getSize() != halfSize) {
        throw std::invalid_argument("invalid key size");
    }

    const auto privKeyValue = keyHalfWithChecksum.getSliceCopy(0, (uint32_t)keyValueSize);
    const auto hashValue = keyHalfWithChecksum.getSliceCopy(keyValueSize, (uint32_t)ckSumSize);

    if (BtcUtils::getHash256(privKeyValue).getSliceCopy(0, (uint32_t)ckSumSize) != hashValue) {
        throw std::runtime_error("checksum failure");
    }

    return privKeyValue;
}

wallet::Seed wallet::Seed::fromEasyCodeChecksum(const EasyCoDec::Data &easyData, NetworkType netType
   , size_t ckSumSize)
{
   return wallet::Seed(netType, decodeEasyCodeChecksum(easyData, ckSumSize));
}

wallet::Seed wallet::Seed::fromEasyCodeChecksum(const EasyCoDec::Data &privKey, const EasyCoDec::Data &chainCode
   , NetworkType netType, size_t ckSumSize)
{
   if (chainCode.part1.empty() || chainCode.part2.empty()) {
      return wallet::Seed(netType, decodeEasyCodeChecksum(privKey, ckSumSize));
   }

   return wallet::Seed(netType, decodeEasyCodeChecksum(privKey, ckSumSize)
      , decodeEasyCodeChecksum(chainCode, ckSumSize));
}


Wallet::Wallet(NetworkType netType, const std::shared_ptr<spdlog::logger> &logger)
   : wallet::MetaData(), netType_(netType), logger_(logger)
{}

Wallet::~Wallet() = default;

std::string Wallet::getAddressComment(const bs::Address &address) const
{
   const auto aeMeta = get(address.id());
   if ((aeMeta == nullptr) || (aeMeta->type() != wallet::AssetEntryMeta::Comment)) {
      return "";
   }
   const auto aeComment = std::dynamic_pointer_cast<wallet::AssetEntryComment>(aeMeta);
   if (aeComment == nullptr) {
      return "";
   }
   return aeComment->comment();
}

bool Wallet::setAddressComment(const bs::Address &address, const std::string &comment)
{
   if (address.isNull()) {
      return false;
   }
   set(std::make_shared<wallet::AssetEntryComment>(nbMetaData_++, address.id(), comment));
   write(getDBEnv(), getDB());
   return true;
}

std::string Wallet::getTransactionComment(const BinaryData &txHash)
{
   const auto aeMeta = get(txHash);
   if ((aeMeta == nullptr) || (aeMeta->type() != wallet::AssetEntryMeta::Comment)) {
      return {};
   }
   const auto aeComment = std::dynamic_pointer_cast<wallet::AssetEntryComment>(aeMeta);
   return aeComment ? aeComment->comment() : std::string{};
}

bool Wallet::setTransactionComment(const BinaryData &txHash, const std::string &comment)
{
   if (txHash.isNull() || comment.empty()) {
      return false;
   }
   set(std::make_shared<wallet::AssetEntryComment>(nbMetaData_++, txHash, comment));
   write(getDBEnv(), getDB());
   return true;
}

std::vector<std::pair<BinaryData, std::string>> Wallet::getAllTxComments() const
{
   std::vector<std::pair<BinaryData, std::string>> result;
   for (const auto &data : MetaData::fetchAll()) {
      if (data.first.getSize() == 32) {   //Detect TX hash by size unless other suitable solution is found
         const auto aeComment = std::dynamic_pointer_cast<wallet::AssetEntryComment>(data.second);
         if (aeComment) {
            result.push_back({ data.first, aeComment->comment() });
         }
      }
   }
   return result;
}

bs::Address Wallet::getRandomChangeAddress(AddressEntryType aet)
{
   if (getUsedAddressCount() < 3) {
      return getNewChangeAddress(aet);
   }
   const auto &addresses = getUsedAddressList();
   return addresses[qrand() % addresses.size()];
}

/*bool Wallet::isSegWitInput(const UTXO& input)
{
   return input.isSegWit() || isSegWitScript(input.getScript());
}

bool Wallet::isSegWitScript(const BinaryData &script)
{
//   const auto scrType = BtcUtils::getTxOutScriptType(script);
   switch (getAddrTypeForAddr(BtcUtils::getTxOutRecipientAddr(script))) {
      case AddressEntryType_P2WPKH:
      case AddressEntryType_P2WSH:
      case AddressEntryType_P2SH:
         return true;
      case AddressEntryType_Default:   // fallback for script not from our wallet
      default: break;                  // fallback for incorrectly deserialized wallet
   }
   return false;
}
*/

Signer Wallet::getSigner(const wallet::TXSignRequest &request, const SecureBinaryData &password,
                             bool keepDuplicatedRecipients)
{
   bs::CheckRecipSigner signer;
   signer.setFlags(SCRIPT_VERIFY_SEGWIT);

   if (!request.prevStates.empty()) {
      for (const auto &prevState : request.prevStates) {
         signer.deserializeState(prevState);
      }
   }

   if (request.populateUTXOs) {
      for (const auto &utxo : request.inputs) {
         try {
            signer.populateUtxo(utxo);
         }
         catch (const std::exception &) { }
      }
   }
   else {
      for (const auto &utxo : request.inputs) {
         auto spender = std::make_shared<ScriptSpender>(utxo, getResolver(password));
         if (request.RBF) {
            spender->setSequence(UINT32_MAX - 2);
         }
         signer.addSpender(spender);
      }
   }

   for (const auto &recipient : request.recipients) {
      signer.addRecipient(recipient);
   }

   if (request.change.value && !request.populateUTXOs) {
      auto changeAddress = createAddressWithIndex(request.change.index);
      if (changeAddress.isNull() || (changeAddress.prefixed() != request.change.address.prefixed())) {
         changeAddress = request.change.address;   // pub and priv wallets are not synchronized for some reason
      }
      setAddressComment(changeAddress, wallet::Comment::toString(wallet::Comment::ChangeAddress));
      const auto addr = getAddressEntryForAddr(changeAddress);
      const auto changeRecip = (addr != nullptr) ? addr->getRecipient(request.change.value)
         : changeAddress.getRecipient(request.change.value);
      if (changeRecip == nullptr) {
         throw std::logic_error("invalid change recipient");
      }
      signer.addRecipient(changeRecip);
   }

   if (!keepDuplicatedRecipients) {
      signer.removeDupRecipients();
   }

   signer.setFeed(getResolver(password));
   return signer;
}

BinaryData Wallet::signTXRequest(const wallet::TXSignRequest &request,
   const SecureBinaryData &password, bool keepDuplicatedRecipients)
{
   auto signer = getSigner(request, password, keepDuplicatedRecipients);
   signer.sign();
   if (!signer.verify()) {
      throw std::logic_error("signer failed to verify");
   }
   return signer.serialize();
}

BinaryData Wallet::signPartialTXRequest(const wallet::TXSignRequest &request, const SecureBinaryData &password)
{
   auto signer = getSigner(request, password);
   signer.sign();
   return signer.serializeState();
}


BinaryData bs::core::SignMultiInputTX(const bs::core::wallet::TXMultiSignRequest &txMultiReq, const KeyMap &keys
   , const WalletMap &wallets)
{
   Signer signer;
   if (!txMultiReq.prevState.isNull()) {
      signer.deserializeState(txMultiReq.prevState);
   }
   signer.setFlags(SCRIPT_VERIFY_SEGWIT);

   for (const auto &wallet : wallets) {
      if (wallet.second->isWatchingOnly()) {
         throw std::logic_error("Won't sign with watching-only wallet");
      }
      const auto &itKey = keys.find(wallet.first);
      const auto password = (itKey == keys.end()) ? SecureBinaryData{} : itKey->second;
      signer.setFeed(wallet.second->getResolver(password));
      signer.sign();
      signer.resetFeeds();
   }

   if (!signer.verify()) {
      throw std::logic_error("signer failed to verify");
   }
   return signer.serialize();
}


/*size_t wallet::getInputScrSize(const std::shared_ptr<AddressEntry> &addrEntry)
{
   if (addrEntry) {
      switch (addrEntry->getType()) {
      case AddressEntryType_P2SH:   return 64;
      default:    return addrEntry->getInputSize();
      }
   }
   return 65;
}*/

BinaryData wallet::computeID(const BinaryData &input)
{
   auto result = BtcUtils::computeID(input);
   const auto outSz = result.getSize();
   if (result.getPtr()[outSz - 1] == 0) {
      result.resize(outSz - 1);
   }
   return result;
}


/*bool operator ==(const Wallet &a, const Wallet &b)
{
   return (a.GetWalletId() == b.GetWalletId());
}

bool operator !=(const Wallet &a, const Wallet &b)
{
   return (a.GetWalletId() != b.GetWalletId());
}*/