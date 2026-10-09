#include "PrefsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QUrl>
#include <QVBoxLayout>

extern "C" {
#include "settings.h"
}

static QString groupTitle(const QString &g)
{
    static const QMap<QString, QString> titles = {
        { "read", "Reading" }, { "aacs", "AACS" }, { "titles", "Titles" }, { "output", "Output" },
        { "bluray", "Blu-ray" }, { "dvd", "DVD" }, { "log", "Log" }, { "gui", "GUI" },
    };
    return titles.value(g, g);
}

static QString nameOf(const Setting *s)
{
    return QString("%1.%2").arg(s->group, s->key);
}

// "split_joined_titles" -> "Split joined titles"
static QString labelOf(const Setting *s)
{
    QString l = QString::fromUtf8(s->key).replace('_', ' ');
    return l.left(1).toUpper() + l.mid(1);
}

PrefsDialog::PrefsDialog(QWidget *parent) : QDialog(parent)
{
    auto *tabs = new QTabWidget;
    auto *layout = new QVBoxLayout(this);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                         QDialogButtonBox::RestoreDefaults);
    QMap<QString, QVBoxLayout *> pages;
    auto *note = new QLabel("Settings that change how a disc is read apply to discs added (or scanned again) after "
                            "OK. They are kept in " + QString::fromUtf8(settings_path()) + ", shared with the "
                            "command line.");

    setWindowTitle("Preferences");
    resize(760, 640);
    note->setWordWrap(true);

    for (int i = 0; i < settings_count(); i++) {
        const Setting *s = settings_get(i);
        QString group = QString::fromUtf8(s->group);
        QWidget *w = nullptr;
        if (s->kept)
            continue;
        if (!pages.contains(group)) {
            auto *page = new QWidget;
            auto *scroll = new QScrollArea;
            auto *v = new QVBoxLayout(page);
            v->addStretch();
            scroll->setWidget(page);
            scroll->setWidgetResizable(true);
            tabs->addTab(scroll, groupTitle(group));
            pages[group] = v;
        }
        QVBoxLayout *v = pages[group];
        auto *name = new QLabel("<b>" + labelOf(s).toHtmlEscaped() + "</b>  <span style=\"color:gray\">" +
                                nameOf(s) + "</span>");
        static const char *const when[] = {
            "Read as a disc is opened: discs already in the queue keep what they were scanned with (Scan Again).",
            "Read as a rip runs.", "Read by the GUI." };
        auto *help = new QLabel(QString::fromUtf8(s->help).toHtmlEscaped() + "<br><i>" + when[s->use] + "</i>");
        help->setTextFormat(Qt::RichText);
        auto *row = new QHBoxLayout;
        help->setWordWrap(true);
        help->setStyleSheet("color:gray;");

        switch (s->kind) {
        case SET_BOOL:
            w = new QCheckBox("on");
            break;
        case SET_INT: {
            auto *spin = new QSpinBox;
            spin->setRange((int)qMax<int64_t>(s->min, INT_MIN), (int)qMin<int64_t>(s->max, INT_MAX));
            w = spin;
            break;
        }
        case SET_CHOICE: {
            auto *combo = new QComboBox;
            for (const char *const *c = s->choices; *c; c++)
                combo->addItem(QString::fromUtf8(*c));
            w = combo;
            break;
        }
        default:
            w = new QLineEdit;
            break;
        }
        row->addWidget(w, 1);
        // a folder or files to pick
        if (nameOf(s) == "output.folder") {
            auto *pick = new QPushButton("Browse…");
            auto *edit = static_cast<QLineEdit *>(w);
            edit->setPlaceholderText("output next to the program");
            connect(pick, &QPushButton::clicked, this, [this, edit] {
                QString d = QFileDialog::getExistingDirectory(this, "Main output folder", edit->text());
                if (!d.isEmpty())
                    edit->setText(d);
            });
            row->addWidget(pick);
        } else if (nameOf(s) == "aacs.key_files") {
            auto *pick = new QPushButton("Add files…");
            auto *open = new QPushButton("Open keydb folder");
            auto *edit = static_cast<QLineEdit *>(w);
            edit->setPlaceholderText("every .cfg file in the keydb folder");
            connect(pick, &QPushButton::clicked, this, [this, edit] {
                const QStringList files = QFileDialog::getOpenFileNames(this, "AACS key files", QString(),
                                                                        "Key files (*.cfg);;All files (*)");
                QStringList items = edit->text().isEmpty() ? QStringList() : edit->text().split(',');
                items << files;
                edit->setText(items.join(','));
            });
            connect(open, &QPushButton::clicked, this, [] {
                QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromUtf8(program_dir()) + "/keydb"));
            });
            row->addWidget(pick);
            row->addWidget(open);
        }
        v->insertWidget(v->count() - 1, name);
        v->insertLayout(v->count() - 1, row);
        v->insertWidget(v->count() - 1, help);
        v->insertSpacing(v->count() - 1, 10);
        fields.append({ i, w });
        setValue(fields.last(), QString::fromUtf8(settings_value(i)));
    }

    layout->addWidget(tabs);
    layout->addWidget(note);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this,
            &PrefsDialog::restoreDefaults);
}

QString PrefsDialog::valueOf(const Field &f) const
{
    switch (settings_get(f.index)->kind) {
    case SET_BOOL:   return static_cast<QCheckBox *>(f.widget)->isChecked() ? "true" : "false";
    case SET_INT:    return QString::number(static_cast<QSpinBox *>(f.widget)->value());
    case SET_CHOICE: return static_cast<QComboBox *>(f.widget)->currentText();
    case SET_LIST: {
        QStringList items;
        for (const QString &it : static_cast<QLineEdit *>(f.widget)->text().split(','))
            if (!it.trimmed().isEmpty())
                items << it.trimmed();
        return items.join(',');
    }
    default:         return static_cast<QLineEdit *>(f.widget)->text();
    }
}

void PrefsDialog::setValue(const Field &f, const QString &v)
{
    switch (settings_get(f.index)->kind) {
    case SET_BOOL:   static_cast<QCheckBox *>(f.widget)->setChecked(v == "true"); break;
    case SET_INT:    static_cast<QSpinBox *>(f.widget)->setValue(v.toInt()); break;
    case SET_CHOICE: static_cast<QComboBox *>(f.widget)->setCurrentText(v); break;
    default:         static_cast<QLineEdit *>(f.widget)->setText(v); break;
    }
}

void PrefsDialog::restoreDefaults()
{
    for (const Field &f : fields)
        setValue(f, QString::fromUtf8(settings_get(f.index)->def));
}

bool PrefsDialog::apply(QStringList *changed, bool *atOpen)
{
    *atOpen = false;
    for (const Field &f : fields) {
        const Setting *s = settings_get(f.index);
        QString v = valueOf(f);
        char err[256];
        if (v == QString::fromUtf8(settings_value(f.index)))
            continue;
        if (settings_store(nameOf(s).toUtf8().constData(), v.toUtf8().constData(), err, sizeof(err)) < 0) {
            QMessageBox::warning(this, "Preferences", QString("%1: the value must be %2").arg(nameOf(s), err));
            return false;
        }
        changed->append(QString("%1 = %2").arg(nameOf(s), v.isEmpty() ? "(empty)" : v));
        *atOpen |= s->use == SET_AT_OPEN;
    }
    if (!changed->isEmpty() && settings_save() < 0) {
        QMessageBox::warning(this, "Preferences", "The settings file cannot be written (see the log)");
        return false;
    }
    return true;
}
