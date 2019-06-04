#include "GenCommonOTCResponse.h"

using namespace Chat;

GenCommonOTCResponse::GenCommonOTCResponse(std::shared_ptr<OTCRequestData> otcRequestData,
                                     OTCResult result,
                                     const QString& message)
   : Response (ResponseType::ResponseGenCommonOTC)
   , otcRequestData_(otcRequestData)
   , result_(result)
   , message_(message)
{

}

QJsonObject GenCommonOTCResponse::toJson() const
{
   QJsonObject data = Response::toJson();
   data[OTCDataObjectKey] = otcRequestData_->toJson();
   data[OTCResultKey] = static_cast<int>(result_);
   data[OTCMessageKey] = message_;
   return data;
}

std::shared_ptr<Response> GenCommonOTCResponse::fromJSON(const std::string &jsonData)
{
   QJsonObject data = QJsonDocument::fromJson(QString::fromStdString(jsonData).toUtf8()).object();
   OTCResult result = static_cast<OTCResult>(data[OTCResultKey].toInt());
   QString message = data[OTCMessageKey].toString();
   QJsonDocument innerDataDocument = QJsonDocument(data[OTCDataObjectKey].toObject());

   std::shared_ptr<OTCRequestData> otcRequestData =
         OTCRequestData::fromJSON(
            QString::fromUtf8(innerDataDocument.toJson()).toStdString());

   return std::make_shared<GenCommonOTCResponse>(otcRequestData, result, message);
}

void GenCommonOTCResponse::handle(ResponseHandler & handler)
{
   handler.OnGenCommonOTCResponse(*this);
}

std::shared_ptr<OTCRequestData> GenCommonOTCResponse::otcRequestData() const
{
   return otcRequestData_;
}

OTCResult GenCommonOTCResponse::getResult() const
{
   return result_;
}

QString GenCommonOTCResponse::getMessage() const
{
   return message_;
}

void GenCommonOTCResponse::setMessage(const QString &message)
{
   message_ = message;
}