// The log as the engine writes it, coloured by level; detail lines only
// when asked for.
#pragma once

#include <QVector>
#include <QWidget>

class QCheckBox;
class QPlainTextEdit;

class Console : public QWidget {
    Q_OBJECT
public:
    explicit Console(QWidget *parent = nullptr);

public slots:
    void addLine(int level, int code, QString source, QString text, bool heading);
    void clear();

private:
    struct Line {
        int     level;
        bool    heading;
        QString text;
    };
    void show(const Line &l);
    void refill();

    QVector<Line>   lines;
    QPlainTextEdit *view;
    QCheckBox      *details;
};
