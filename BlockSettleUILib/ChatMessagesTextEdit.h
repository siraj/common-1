#ifndef __CHAT_MESSAGES_VIEW_MODEL_H__
#define __CHAT_MESSAGES_VIEW_MODEL_H__

#include <memory>
#include <QTextBrowser>
#include <QMap>
#include <QVector>
#include <QDateTime>
#include <tuple>
#include <QTextTable>
#include <QImage>

namespace Chat {
   class MessageData;
}

class ChatMessagesTextEditStyle : public QWidget
{
   Q_OBJECT

   Q_PROPERTY(QColor color_hyperlink READ colorHyperlink
              WRITE setColorHyperlink)
   Q_PROPERTY(QColor color_white READ colorWhite
              WRITE setColorWhite)

public:
   inline explicit ChatMessagesTextEditStyle(QWidget *parent)
      : QWidget(parent), colorHyperlink_(Qt::blue), colorWhite_(Qt::white)
   {
      setVisible(false);
   }

   QColor colorHyperlink() const { return colorHyperlink_; }
   void setColorHyperlink(const QColor &colorHyperlink) {
      colorHyperlink_ = colorHyperlink;
   }

   QColor colorWhite() const { return colorWhite_; }
   void setColorWhite(const QColor &colorWhite) {
      colorWhite_ = colorWhite;
   }

private:
   QColor colorHyperlink_;
   QColor colorWhite_;
};

class ChatMessagesTextEdit : public QTextBrowser
{
   Q_OBJECT

public:
   ChatMessagesTextEdit(QWidget* parent = nullptr);
   ~ChatMessagesTextEdit() noexcept override = default;

public:
   void setOwnUserId(const std::string &userId) { ownUserId_ = QString::fromStdString(userId); }
   
signals:
   void MessageRead(const std::shared_ptr<Chat::MessageData> &) const;
   void rowsInserted();
   void userHaveNewMessageChanged(const QString &userId, const bool &haveNewMessage, const bool &isInCurrentChat);

protected:
   enum class Column {
      Time,
      Status,
      User,
      Message,
      last
   };

   QString data(const int &row, const Column &column);
   QImage statusImage(const int &row);

   
public slots:
   void onSwitchToChat(const QString& chatId);
   void onMessagesUpdate(const std::vector<std::shared_ptr<Chat::MessageData>> & messages, bool isFirstFetch);
   void onSingleMessageUpdate(const std::shared_ptr<Chat::MessageData> &);
   void onMessageIdUpdate(const QString& oldId, const QString& newId,const QString& chatId);
   void onMessageStatusChanged(const QString& messageId, const QString chatId, int newStatus);
   void urlActivated(const QUrl &link);


private:
   using MessagesHistory = std::vector<std::shared_ptr<Chat::MessageData>>;
   QMap<QString, MessagesHistory> messages_;
   MessagesHistory messagesToLoadMore_;
   QString   currentChatId_;
   QString   ownUserId_;
   
private:
   std::shared_ptr<Chat::MessageData> findMessage(const QString& chatId, const QString& messageId);
   void notifyMessageChanged(std::shared_ptr<Chat::MessageData> message);
   void insertMessage(std::shared_ptr<Chat::MessageData> message);
   void insertLoadMore();
   void loadMore();
   QString toHtmlText(const QString &text);

   QTextTableFormat tableFormat;
   QTextTable *table;
   ChatMessagesTextEditStyle internalStyle_;

   QImage statusImageOffline_;
   QImage statusImageConnecting_;
   QImage statusImageOnline_;
   QImage statusImageRead_;
};

#endif