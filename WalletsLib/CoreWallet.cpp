/*

***********************************************************************************
* Copyright (C) 2016 - 2019, BlockSettle AB
* Distributed under the GNU Affero General Public License (AGPL v3)
* See LICENSE or http://www.gnu.org/licenses/agpl.html
*
**********************************************************************************

*/
#include "CoreWallet.h"

#include "CheckRecipSigner.h"
#include "CoinSelection.h"
#include "Wallets.h"
#include "XBTAmount.h"

#define SAFE_NUM_CONFS        6
#define ASSETMETA_PREFIX      0xAC

#include <spdlog/spdlog.h>

#include <sstream>

using namespace bs::core;

std::shared_ptr<wallet::AssetEntryMeta> wallet::AssetEntryMeta::deserialize(int, BinaryDataRef value)
{
   BinaryRefReader brr(value);

   const auto type = brr.get_uint8_t();
   if ((type == wallet::AssetEntryMeta::Comment) && (brr.getSizeRemaining() > 0)) {
      auto aeComment = std::make_shared<wallet::AssetEntryComment>();
      return (aeComment->deserialize(brr) ? aeComment : nullptr);
   }
   throw AssetException("unknown metadata type " + std::to_string(type));
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

   return bw.getData();
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

bool wallet::MetaData::write(const std::shared_ptr<DBIfaceTransaction> &tx)
{
   if (!tx) {
      return false;
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

      tx->insert(dbKey, serializedEntry);
      value.second->doNotCommit();
   }
   return true;
}

void wallet::MetaData::readFromDB(const std::shared_ptr<DBIfaceTransaction> &tx)
{
   if (!tx) {
      throw WalletException("DB interface is not initialized");
   }

   const auto dbIter = tx->getIterator();

   BinaryWriter bwKey;
   bwKey.put_uint8_t(ASSETMETA_PREFIX);

   dbIter->seek(bwKey.getData());

   while (dbIter->isValid()) {
      const auto keyBDR = dbIter->key();
      const auto valueBDR = dbIter->value();

//      try {
         BinaryRefReader brrKey(keyBDR);

         auto prefix = brrKey.get_uint8_t();
         if (prefix != ASSETMETA_PREFIX) {
            break;
//            throw AssetException("invalid prefix " + std::to_string(prefix));
         }
         auto index = brrKey.get_int32_t();
         nbMetaData_ = index + 1;

         auto entryPtr = AssetEntryMeta::deserialize(index, valueBDR);
         if (entryPtr) {
            entryPtr->doNotCommit();
            data_[entryPtr->key()] = entryPtr;
         }
/*      }
      catch (AssetException& e) {
         LOGERR << e.what();
         break;
      }*/

      dbIter->advance();
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

Signer wallet::TXSignRequest::getSigner(const std::shared_ptr<ResolverFeed> &resolver) const
{
   bs::CheckRecipSigner prevStateSigner;
   bs::CheckRecipSigner signer;

   if (!prevStates.empty()) {
      for (const auto &prevState : prevStates) {
         prevStateSigner.deserializeState(prevState);
      }

      signer.setFlags(prevStateSigner.getFlags());
      for (const auto &spender : prevStateSigner.spenders()) {
         signer.addSpender(spender);
      }
   }
   else {
      signer.setFlags(SCRIPT_VERIFY_SEGWIT);

      for (const auto &utxo : inputs) {   // TODO: maybe this loop should always run, not only if prevStates are empty
         std::shared_ptr<ScriptSpender> spender;
         if (resolver) {
            spender = std::make_shared<ScriptSpender>(utxo, resolver);
         } else if (populateUTXOs) {
            spender = std::make_shared<ScriptSpender>(utxo.getTxHash(), utxo.getTxOutIndex(), utxo.getValue());
         } else {
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

   for (const auto &orderType : outSortOrder) {
      switch (orderType) {
      case OutputOrderType::PrevState:
         for (const auto &recip : prevStateSigner.recipients()) {
            signer.addRecipient(recip);
         }
         break;

      case OutputOrderType::Recipients:
         for (const auto& recipient : recipients) {
            signer.addRecipient(recipient);
         }
         break;

      case OutputOrderType::Change:
         if (!change.value) {
            break;
         }
         {
            const auto changeRecip = change.address.getRecipient(bs::XBTAmount{ change.value });
            if (changeRecip) {
               signer.addRecipient(changeRecip);
            }
            else {
               throw std::runtime_error("failed to get change recipient for " + std::to_string(change.value));
            }
         }
         break;

      default:
         throw std::invalid_argument("invalid output order type " + std::to_string((int)orderType));
      }
   }

   signer.removeDupRecipients();

   if (resolver) {
      signer.setFeed(resolver);
   }
   return signer;
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
   const size_t nonWitSize = inUTXOSel.size_ - inUTXOSel.witnessSize_;
   return std::ceil(static_cast<float>(3 * nonWitSize + inUTXOSel.size_) / 4.0f);
}

size_t wallet::TXSignRequest::estimateTxVirtSize() const
{   // another implementation based on Armory and TransactionData code
   auto transactions = bs::Address::decorateUTXOsCopy(inputs);
   std::map<unsigned int, std::shared_ptr<ScriptRecipient>> recipientsMap;
   for (unsigned int i = 0; i < recipients.size(); ++i) {
      recipientsMap[i] = recipients[i];
   }

   try {
      const PaymentStruct payment(recipientsMap, fee, 0, 0);
      return getVirtSize(computeSizeAndFee(transactions, payment));
   }
   catch (const std::exception &) {}
   return 0;
}

uint64_t wallet::TXSignRequest::amount(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   // synonym for amountSent
   return amountSent(containsAddressCb);
}

uint64_t wallet::TXSignRequest::inputAmount(const ContainsAddressCb &containsAddressCb) const
{
   // calculate total input amount based on inputs
   // prevStates inputs parsed first
   // duplicated inputs skipped

   std::set<UTXO> utxoSet;
   uint64_t inputAmount = 0;

   if (!prevStates.empty() && containsAddressCb != nullptr) {
      bs::CheckRecipSigner signer(prevStates.front());

      for (auto spender : signer.spenders()) {
         const auto addr = bs::Address::fromUTXO(spender->getUtxo());
         if (utxoSet.find(spender->getUtxo()) == utxoSet.cend()) {
            if (containsAddressCb(addr)) {
               utxoSet.insert(spender->getUtxo());
               inputAmount += spender->getValue();
            }
         }
      }
   }

   for (const auto &utxo: inputs) {
      const auto addr = bs::Address::fromUTXO(utxo);

      if (utxoSet.find(utxo) == utxoSet.cend()) {
         if (containsAddressCb(addr)) {
            utxoSet.insert(utxo);
            inputAmount += utxo.getValue();
         }
      }
   }

   return inputAmount;
}

uint64_t wallet::TXSignRequest::totalSpent(const ContainsAddressCb &containsAddressCb) const
{
   return inputAmount(containsAddressCb) - changeAmount(containsAddressCb);
}

uint64_t wallet::TXSignRequest::changeAmount(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   // calculate change amount
   // if change is not explicitly set, calculate change using prevStates for provided containsAddressCb

   uint64_t changeVal = change.value;
   if (changeVal == 0 && !prevStates.empty() && containsAddressCb != nullptr) {
      bs::CheckRecipSigner signer(prevStates.front());

      for (auto recip : signer.recipients()) {
         const auto addr = bs::Address::fromRecipient(recip);
         if (containsAddressCb(addr)) {
            uint64_t change = recip->getValue();
            changeVal += change;
         }
      }
   }

   return changeVal;
}

uint64_t wallet::TXSignRequest::amountReceived(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   // calculate received amount based on recipients
   // containsAddressCb should return true if address is our
   // prevStates recipients parsed first
   // duplicated recipients skipped

   std::set<BinaryData> txSet;
   uint64_t amount = 0;

   if (!prevStates.empty() && containsAddressCb != nullptr) {
      bs::CheckRecipSigner signer(prevStates.front());
      for (auto recip : signer.recipients()) {
         const auto addr = bs::Address::fromRecipient(recip);
         const auto hash = recip->getSerializedScript();

         if (txSet.find(hash) == txSet.cend()) {
            if (containsAddressCb(addr)) {
               txSet.insert(hash);
               amount += recip->getValue();
            }
         }
      }
   }

   for (const auto &recip: recipients) {
      const auto addr = bs::Address::fromRecipient(recip);
      const auto hash = recip->getSerializedScript();

      if (txSet.find(hash) == txSet.cend()) {
         if (containsAddressCb(addr)) {
            txSet.insert(hash);
            amount += recip->getValue();
         }
      }
   }

   return amount;
}

uint64_t wallet::TXSignRequest::amountSent(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   // calculate sent amount based on inputs and change
   // containsAddressCb should return true if change address is in our wallet
   // or
   // get sent amount directly from recipients

   if (!prevStates.empty() && containsAddressCb != nullptr) {
      return totalSpent(containsAddressCb) - getFee();
   }

   uint64_t amount = 0;
   for (const auto &recip : recipients) {
      amount += recip->getValue();
   }

   return amount;
}

uint64_t wallet::TXSignRequest::amountReceivedOn(const bs::Address &address, bool removeDuplicatedRecipients) const
{
   // Duplicated recipients removal should be used only for calculaion values in TXInfo for CC settlements
   // to bypass workaround In ReqCCSettlementContainer::createCCUnsignedTXdata()

   std::set<BinaryData> txSet;
   uint64_t amount = 0;

   if (!prevStates.empty()) {
      bs::CheckRecipSigner signer(prevStates.front());
      for (auto recip : signer.recipients()) {
         const auto addr = bs::Address::fromRecipient(recip);
         const auto hash = recip->getSerializedScript();

         if (txSet.find(hash) == txSet.cend()) {
            if (addr == address) {
               if (removeDuplicatedRecipients) {
                  txSet.insert(hash);
               }
               amount += recip->getValue();
            }
         }
      }
   }

   for (const auto &recip: recipients) {
      const auto addr = bs::Address::fromRecipient(recip);
      const auto hash = recip->getSerializedScript();

      if (txSet.find(hash) == txSet.cend()) {
         if (addr == address) {
            if (removeDuplicatedRecipients) {
               txSet.insert(hash);
            }
            amount += recip->getValue();
         }
      }
   }

   if (change.address == address) {
      amount += change.value;
   }

   return amount;
}

uint64_t wallet::TXSignRequest::getFee() const
{
   if (fee > 0) {
      return fee;
   }

   if (!prevStates.empty()) {
      bs::CheckRecipSigner signer(prevStates.front());
      return signer.inputsTotalValue() - signer.outputsTotalValue();
   }

   return 0;
}

std::vector<UTXO> wallet::TXSignRequest::getInputs(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   std::vector<UTXO> inputsVector;
   std::set<UTXO> inputsSet;

   if (!prevStates.empty()) {
      bs::CheckRecipSigner signer(prevStates.front());
      for (auto spender : signer.spenders()) {
         const auto &addr = bs::Address::fromUTXO(spender->getUtxo());

         if (inputsSet.find(spender->getUtxo()) == inputsSet.cend()) {
            if (containsAddressCb(addr)) {
               inputsSet.insert(spender->getUtxo());
               inputsVector.push_back(spender->getUtxo());
            }
         }
      }
   }

   for (const auto &utxo : inputs) {
      const auto &addr = bs::Address::fromUTXO(utxo);

      if (inputsSet.find(utxo) == inputsSet.cend()) {
         if (containsAddressCb(addr)) {
            inputsSet.insert(utxo);
            inputsVector.push_back(utxo);
         }
      }
   }

   return inputsVector;
}

std::vector<std::shared_ptr<ScriptRecipient>> wallet::TXSignRequest::getRecipients(const wallet::TXSignRequest::ContainsAddressCb &containsAddressCb) const
{
   std::vector<std::shared_ptr<ScriptRecipient>> recipientsVector;
   std::set<BinaryData> recipientsSet;

   if (!prevStates.empty()) {
      bs::CheckRecipSigner signer(prevStates.front());
      for (auto recip : signer.recipients()) {
         const auto &addr = bs::Address::fromRecipient(recip);
         const auto &hash = recip->getSerializedScript();

         if (recipientsSet.find(hash) == recipientsSet.cend()) {
            if (containsAddressCb(addr)) {
               recipientsSet.insert(hash);
               recipientsVector.push_back(recip);
            }
         }
      }
   }

   for (const auto &recip : recipients) {
      const auto &addr = bs::Address::fromRecipient(recip);
      const auto &hash = recip->getSerializedScript();

      if (recipientsSet.find(hash) == recipientsSet.cend()) {
         if (containsAddressCb(addr)) {
            recipientsSet.insert(hash);
            recipientsVector.push_back(recip);
         }
      }
   }

   return recipientsVector;
}

bool wallet::TXSignRequest::isSourceOfTx(const Tx &signedTx) const
{
   try {
      if ((inputs.size() != signedTx.getNumTxIn())) {
         return false;
      }

      const size_t nbRecipients = change.value > 0 ? recipients.size() + 1 : recipients.size();
      // this->change may contains one of TxOut
      if (signedTx.getNumTxOut() != nbRecipients) {
         return false;
      }

      for (int i = 0; i < signedTx.getNumTxOut(); i++) {
         auto&& txOut = signedTx.getTxOutCopy(i);
         bs::Address txAddr = bs::Address::fromTxOut(txOut);
         uint64_t outValSigned = txOut.getValue();
         uint64_t outValUnsigned = amountReceivedOn(txAddr);

         if (outValUnsigned != outValSigned) {
            return false;
         }
      }

      for (int i = 0; i < signedTx.getNumTxIn(); i++) {
         auto&& txIn = signedTx.getTxInCopy(i);
         OutPoint op = txIn.getOutPoint();

         const auto signedHash = op.getTxHash();
         uint32_t signedTxOutIndex = op.getTxOutIndex();

         bool hasUnsignedInput = false;
         for (int j = 0; j < inputs.size(); j++) {
            const auto unsignedHash = inputs.at(j).getTxHash();
            uint32_t unsignedTxOutIndex = inputs.at(j).getTxOutIndex();

            if (signedHash == unsignedHash && signedTxOutIndex == unsignedTxOutIndex) {
               hasUnsignedInput = true;
               break;
            }
         }

         if (!hasUnsignedInput) {
            return false;
         }
      }
      return true;

   } catch (...) {
      return false;
   }
}

bool wallet::TXMultiSignRequest::isValid() const noexcept
{
   if (inputs.empty() || recipients.empty()) {
      return false;
   }
   return true;
}


wallet::Seed::Seed(const SecureBinaryData &seed, NetworkType netType)
   : netType_(netType), seed_(seed)
{
   node_.initFromSeed(seed_);
}

std::string wallet::Seed::getWalletId() const
{
   if (walletId_.empty()) {
/*      const SecureBinaryData hmacMasterMsg("MetaEntry");
      const auto &pubkey = node_.getPublicKey();
      auto &&masterID = BtcUtils::getHMAC256(pubkey, hmacMasterMsg);
      walletId_ = BtcUtils::computeID(masterID).toBinStr();*/

      const auto node = getNode();
      auto chainCode = node.getChaincode();
      DerivationScheme_ArmoryLegacy derScheme(chainCode);

      auto pubKey = node.getPublicKey();
      if (pubKey.isNull()) {
         return {};
      }
      auto assetSingle = std::make_shared<AssetEntry_Single>(
         ROOT_ASSETENTRY_ID, BinaryData(), pubKey, nullptr);

      auto addrVec = derScheme.extendPublicChain(assetSingle, 1, 1);
      assert(addrVec.size() == 1);
      auto firstEntry = std::dynamic_pointer_cast<AssetEntry_Single>(addrVec[0]);
      assert(firstEntry != nullptr);
      walletId_ = BtcUtils::computeID(firstEntry->getPubKey()->getUncompressedKey()).toBinStr();
      if (*(walletId_.rbegin()) == 0) {
         walletId_.resize(walletId_.size() - 1);
      }
   }
   return walletId_;
}

EasyCoDec::Data wallet::Seed::toEasyCodeChecksum(size_t ckSumSize) const
{
   if (seed_.getSize() == 0)
      throw AssetException("empty seed, cannot generate ez16");

   const size_t halfSize = seed_.getSize() / 2;
   auto privKeyHalf1 = seed_.getSliceCopy(0, (uint32_t)halfSize);
   auto privKeyHalf2 = seed_.getSliceCopy(halfSize, seed_.getSize() - halfSize);
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
   return wallet::Seed(decodeEasyCodeChecksum(easyData, ckSumSize), netType);
}

SecureBinaryData wallet::Seed::toXpriv() const
{
   return node_.getBase58();
}

wallet::Seed wallet::Seed::fromXpriv(const SecureBinaryData& xpriv, NetworkType netType)
{
   wallet::Seed seed(netType);
   seed.node_.initFromBase58(xpriv);

   //check network

   //check base
   if (seed.node_.getDepth() > 0 || seed.node_.getFingerPrint() != 0)
      throw WalletException("xpriv is not for wallet root");

   return seed;
}

////////////////////////////////////////////////////////////////////////////////
Wallet::Wallet(std::shared_ptr<spdlog::logger> logger)
   : wallet::MetaData(), logger_(logger)
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
   return write(getDBWriteTx());
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
   return write(getDBWriteTx());
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

Signer Wallet::getSigner(const wallet::TXSignRequest &request,
                             bool keepDuplicatedRecipients)
{
   bs::CheckRecipSigner signer, prevStateSigner;

   if (!request.prevStates.empty()) {
      for (const auto &prevState : request.prevStates) {
         prevStateSigner.deserializeState(prevState);
      }

      signer.setFlags(prevStateSigner.getFlags());
      for (const auto &spender : prevStateSigner.spenders()) {
         signer.addSpender(spender);
      }
   }
   else {
      signer.setFlags(SCRIPT_VERIFY_SEGWIT);
   }

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
         auto spender = std::make_shared<ScriptSpender>(utxo, getResolver());
         if (request.RBF) {
            spender->setSequence(UINT32_MAX - 2);
         }
         signer.addSpender(spender);
      }
   }

   for (const auto &orderType : request.outSortOrder) {
      switch (orderType) {
      case wallet::OutputOrderType::PrevState:
         for (const auto &recip : prevStateSigner.recipients()) {
            signer.addRecipient(recip);
         }
         break;

      case wallet::OutputOrderType::Recipients:
         for (const auto& recipient : request.recipients) {
            signer.addRecipient(recipient);
         }
         break;

      case wallet::OutputOrderType::Change:
         if (!request.change.value || request.populateUTXOs) {
            break;
         }
         {
            const auto changeRecip = request.change.address
               .getRecipient(bs::XBTAmount{ request.change.value });
            if (changeRecip) {
               signer.addRecipient(changeRecip);
            } else {
               throw std::runtime_error("failed to get change recipient for "
                  + std::to_string(request.change.value));
            }
         }
         break;

      default:
         throw std::invalid_argument("invalid output order type " + std::to_string((int)orderType));
      }
   }

   if (!keepDuplicatedRecipients) {
      signer.removeDupRecipients();
   }

#ifndef NDEBUG
   if (logger_) {
      for (const auto &spender : signer.spenders()) {
         logger_->debug("[{}] {} spender: {} @ {}", __func__
            , walletId(), spender->getValue()
            , bs::Address::fromUTXO(spender->getUtxo()).display());
      }
      for (const auto &recip : signer.recipients()) {
         logger_->debug("[{}] {} recipient: {} @ {}", __func__
            , walletId(), recip->getValue()
            , bs::Address::fromRecipient(recip).display());
      }
   }
#endif //NDEBUG

   signer.setFeed(getResolver());
   return signer;
}

BinaryData Wallet::signTXRequest(const wallet::TXSignRequest &request, bool keepDuplicatedRecipients)
{
   auto lock = lockDecryptedContainer();
   auto signer = getSigner(request, keepDuplicatedRecipients);
   signer.sign();
   if (!signer.verify()) {
      throw std::logic_error("signer failed to verify");
   }
   return signer.serialize();
}

BinaryData Wallet::signPartialTXRequest(const wallet::TXSignRequest &request)
{
   auto lock = lockDecryptedContainer();
   bs::CheckRecipSigner signer{ getSigner(request) };
   signer.sign();
   if (!signer.verifyPartial()) {
      throw std::logic_error("signer failed to verify");
   }
   return signer.serializeState();
}


BinaryData bs::core::SignMultiInputTX(const bs::core::wallet::TXMultiSignRequest &txMultiReq
   , const WalletMap &wallets, bool partial)
{
   bs::CheckRecipSigner signer;
   if (!txMultiReq.prevState.isNull()) {
      signer.deserializeState(txMultiReq.prevState);

      signer.setFlags(SCRIPT_VERIFY_SEGWIT);

      for (const auto &wallet : wallets) {
         if (wallet.second->isWatchingOnly()) {
            throw std::logic_error("Won't sign with watching-only wallet");
         }
         auto lock = wallet.second->lockDecryptedContainer();
         signer.setFeed(wallet.second->getResolver());
         signer.sign();
         signer.resetFeeds();
      }
   }
   else {
      signer.setFlags(SCRIPT_VERIFY_SEGWIT);

      for (const auto &input : txMultiReq.inputs) {
         const auto itWallet = wallets.find(input.second);
         if (itWallet == wallets.end()) {
            throw std::runtime_error("missing wallet for id " + input.second);
         }
         auto lock = itWallet->second->lockDecryptedContainer();
         auto spender = std::make_shared<ScriptSpender>(input.first, itWallet->second->getResolver());
         if (txMultiReq.RBF) {
            spender->setSequence(UINT32_MAX - 2);
         }
         signer.addSpender(spender);
      }

      for (const auto &recipient : txMultiReq.recipients) {
         signer.addRecipient(recipient);
      }

      auto lock = wallets.begin()->second->lockDecryptedContainer();
      signer.sign();
   }

   if (partial) {
      if (!signer.verifyPartial()) {
         throw std::logic_error("signer failed to verify");
      }
      return signer.serializeState();
   }
   else {
      if (!signer.verify()) {
         throw std::logic_error("signer failed to verify");
      }
      return signer.serialize();
   }
}


BinaryData wallet::computeID(const BinaryData &input)
{
   auto result = BtcUtils::computeID(input);
   const auto outSz = result.getSize();
   if (result.getPtr()[outSz - 1] == 0) {
      result.resize(outSz - 1);
   }
   return result;
}


void wallet::TXSignRequest::DebugPrint(const std::string& prefix, const std::shared_ptr<spdlog::logger>& logger
                                       , bool serializeAndPrint
                                       , const std::shared_ptr<ResolverFeed> &resolver)
{
   std::stringstream ss;

   // wallet ids
   ss << "   TXSignRequest TX ID:   " << txId(resolver).toHexStr(true) << "\n";

   uint64_t inputAmount = 0;
   ss << "      Inputs: " << inputs.size() << '\n';
   for (const auto& utxo : inputs) {
      ss << "    UTXO txHash : " << utxo.txHash_.toHexStr() << '\n';
      ss << "         txOutIndex : " << utxo.txOutIndex_ << '\n';
      ss << "         txHeight : " << utxo.txHeight_ << '\n';
      ss << "         txIndex : " << utxo.txIndex_ << '\n';
      ss << "         value : " << utxo.value_ << '\n';
      ss << "         script : " << utxo.script_.toHexStr() << '\n';
      ss << "         SW  : " << (utxo.isInputSW_ ? "yes" : "no") << '\n';
      ss << "         txinRedeemSizeBytes: " << utxo.txinRedeemSizeBytes_ << '\n';
      ss << "         witnessDataSizeBytes: " << utxo.witnessDataSizeBytes_ << '\n';
      inputAmount += utxo.getValue();
   }

   // outputs
   ss << "   Outputs: " << recipients.size() << '\n';
   for (const auto &recipient : recipients) {
      ss << "       Amount: " << recipient->getValue() << '\n';
   }

   // amount
   ss << "    Inputs Amount: " << inputAmount << '\n';
   // change
   if (change.value != 0) {
      ss << "    Change : " << change.value << " to " << change.address.display() << '\n';
   } else  {
      ss << "    No change\n";
   }
   // fee
   ss << "    Fee: " << fee << '\n';

   if (serializeAndPrint) {
      const auto &serialized = serializeState(resolver);

      ss << "     Serialized: " << serialized.toHexStr() << '\n';
      try {
         Tx tx{serialized};
         std::string payinHash = tx.getThisHash().toHexStr(true);

         ss << "    TX hash: " << payinHash << '\n';
         ss << "    SS info:\n";
         tx.pprintAlot(ss);
      } catch (...) {
         ss << "   error: failed to serialize tx\n";
      }
   }

   logger->debug("{} :\n{}"
                  ,prefix , ss.str());
}
