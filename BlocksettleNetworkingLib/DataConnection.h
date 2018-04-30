#ifndef __DATA_CONNECTION_H__
#define __DATA_CONNECTION_H__

#include <memory>
#include <string>

#include "DataConnectionListener.h"

class DataConnection
{
public:
   using listener_type = DataConnectionListener;
public:
   DataConnection()
    : listener_(nullptr)
   {}

   virtual ~DataConnection() noexcept = default;

   DataConnection(const DataConnection&) = delete;
   DataConnection& operator = (const DataConnection&) = delete;

   DataConnection(DataConnection&&) = delete;
   DataConnection& operator = (DataConnection&&) = delete;

public:
   virtual bool send(const std::string& data) = 0;

   virtual bool openConnection(const std::string& host
                              , const std::string& port
                              , DataConnectionListener* listener) = 0;
   virtual bool closeConnection() = 0;

protected:
   void setListener(DataConnectionListener* listener) {
      listener_ = listener;
   }

   bool haveListener() const { return listener_ != nullptr; }

   virtual void onRawDataReceived(const std::string& rawData) = 0;

   void notifyOnData(const std::string& data);
   void notifyOnConnected();
   void notifyOnDisconnected();
   void notifyOnError(DataConnectionListener::DataConnectionError errorCode);

private:
   DataConnectionListener* listener_;
};

#endif // __DATA_CONNECTION_H__