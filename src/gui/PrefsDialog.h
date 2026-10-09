// The settings menu: every setting of the engine and the GUI, from the
// settings registry (settings.h), one tab per group, with its help. OK keeps
// the changed values in the settings file next to the program.
#pragma once

#include <QDialog>
#include <QVector>

class QWidget;

class PrefsDialog : public QDialog {
    Q_OBJECT
public:
    explicit PrefsDialog(QWidget *parent = nullptr);

    // The changed settings stored and the file written; "name = value" of
    // each into changed, *atOpen: one is read as a disc is opened. false: a
    // value was refused (said in a message box).
    bool apply(QStringList *changed, bool *atOpen);

private:
    struct Field {
        int      index;     // in the registry
        QWidget *widget;
    };
    QString valueOf(const Field &f) const;
    void    setValue(const Field &f, const QString &v);
    void    restoreDefaults();

    QVector<Field> fields;
};
