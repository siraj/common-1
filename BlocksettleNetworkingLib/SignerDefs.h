#ifndef __SIGNER_DEFS_H__
#define __SIGNER_DEFS_H__

#include "Address.h"
#include "BtcDefinitions.h"
#include "HDPath.h"
#include "WalletEncryption.h"

namespace bs {
namespace signer {

   using RequestId = unsigned int;

   struct Limits {
      uint64_t    autoSignSpendXBT = UINT64_MAX;
      uint64_t    manualSpendXBT = UINT64_MAX;
      int         autoSignTimeS = 0;
      int         manualPassKeepInMemS = 0;

      Limits() {}
      Limits(uint64_t asXbt, uint64_t manXbt, int asTime, int manPwTime)
         : autoSignSpendXBT(asXbt), manualSpendXBT(manXbt), autoSignTimeS(asTime)
         , manualPassKeepInMemS(manPwTime) {}
   };

   enum class RunMode {
      fullgui,
      lightgui,
      headless,
      cli
   };

} // signer

namespace sync {

   enum class WalletFormat {
      Unknown = 0,
      HD,
      Plain,
      Settlement
   };

   struct WalletInfo
   {
      WalletFormat   format;
      std::string id;
      std::string name;
      std::string description;
      NetworkType netType;
      bool        watchOnly;
   };

   struct HDWalletData
   {
      struct Leaf {
         std::string          id;
         bs::hd::Path::Elem   index;
         bool extOnly;
      };
      struct Group {
         bs::hd::CoinType  type;
         std::vector<Leaf> leaves;
         bool extOnly;
      };
      std::vector<Group>   groups;
   };

   struct AddressData
   {
      std::string index;
      bs::Address address;
      std::string comment;
   };

   struct TxCommentData
   {
      BinaryData  txHash;
      std::string comment;
   };

   struct WalletData
   {
      std::vector<bs::wallet::EncryptionType>   encryptionTypes;
      std::vector<SecureBinaryData>          encryptionKeys;
      std::pair<unsigned int, unsigned int>  encryptionRank{ 0,0 };
      NetworkType netType = NetworkType::Invalid;

      //flag value, signifies the higest index entries are unset if not changed from UINT32_MAX
      unsigned highestExtIndex_ = UINT32_MAX; 
      unsigned highestIntIndex_ = UINT32_MAX;

      std::vector<AddressData>   addresses;
      std::vector<AddressData>   addrPool;
      std::vector<TxCommentData> txComments;
   };

   struct WatchingOnlyWallet
   {
      struct Address {
         std::string index;
         AddressEntryType  aet;
      };
      struct Leaf {
         std::string          id;
         bs::hd::Path::Elem   index;
         BinaryData           publicKey;
         BinaryData           chainCode;
         std::vector<Address> addresses;
      };
      struct Group {
         bs::hd::CoinType  type;
         std::vector<Leaf> leaves;
      };

      NetworkType netType = NetworkType::Invalid;
      std::string id;
      std::string name;
      std::string description;
      std::vector<Group>   groups;
   };

}  //namespace sync

} // bs

#endif
