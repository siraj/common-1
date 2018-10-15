#ifndef __PUSH_BUTTON_MENU_H__
#define __PUSH_BUTTON_MENU_H__

#include <QPushButton>
#include <QMenu>

class ButtonMenu : public QMenu
{
Q_OBJECT
public:
   explicit ButtonMenu(QPushButton* button);
   ~ButtonMenu() noexcept override = default;

   ButtonMenu(const ButtonMenu&) = delete;
   ButtonMenu& operator = (const ButtonMenu&) = delete;

   ButtonMenu(ButtonMenu&&) = delete;
   ButtonMenu& operator = (ButtonMenu&&) = delete;

   void showEvent(QShowEvent* event) override;

private:
    QPushButton *parentButton_;
};

#endif // __PUSH_BUTTON_MENU_H__