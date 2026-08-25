#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_Salacia_Terminal.h"

class Salacia_Terminal : public QMainWindow
{
    Q_OBJECT

public:
    Salacia_Terminal(QWidget *parent = nullptr);
    ~Salacia_Terminal();

private:
    Ui::Salacia_TerminalClass ui;
};

