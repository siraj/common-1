#ifndef __BS_ERROR_CODE_STRINGS_H__
#define __BS_ERROR_CODE_STRINGS_H__

#include <QObject>
#include "BSErrorCode.h"

namespace bs {
namespace error {

inline QString ErrorCodeToString(bs::error::ErrorCode errorCode) {
   switch (errorCode) {

   // General errors
   case bs::error::ErrorCode::NoError:
      return QObject::tr("");
   case bs::error::ErrorCode::FailedToParse:
      return QObject::tr("Failed to parse request");
   case bs::error::ErrorCode::WalletNotFound:
      return QObject::tr("Failed to find wallet");
   case bs::error::ErrorCode::MissingPassword:
      return QObject::tr("Missing password for encrypted wallet");
   case bs::error::ErrorCode::MissingAuthKeys:
      return QObject::tr("Missing auth priv/pub keys for encrypted wallet");
   case bs::error::ErrorCode::MissingSettlementWallet:
      return QObject::tr("Missing settlement wallet");
   case bs::error::ErrorCode::MissingAuthWallet:
      return QObject::tr("Missing Auth wallet");


   // TX Signing errors
   case bs::error::ErrorCode::TxInvalidRequest:
      return QObject::tr("Tx Request is invalid, missing critical data");
   case bs::error::ErrorCode::TxCanceled:
      return QObject::tr("Transaction canceled");
   case bs::error::ErrorCode::TxSpendLimitExceed:
      return QObject::tr("Spend limit exceeded");
   case bs::error::ErrorCode::TxRequestFileExist:
      return QObject::tr("TX request file already exist");
   case bs::error::ErrorCode::TxFailedToOpenRequestFile:
      return QObject::tr("Failed to open TX request file");
   case bs::error::ErrorCode::TxFailedToWriteRequestFile:
      return QObject::tr("Failed to write TX request file");
   default:
      return QObject::tr("Unknown error");
   }
}

} // namespace error
} // namespace bs

#endif