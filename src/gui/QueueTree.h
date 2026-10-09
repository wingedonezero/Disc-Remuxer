// The queue's tree: files and folders dropped on it are added; discs (the
// top rows) are put in another order by dragging them.
#pragma once

#include <QStringList>
#include <QTreeWidget>

class QueueTree : public QTreeWidget {
    Q_OBJECT
public:
    explicit QueueTree(QWidget *parent = nullptr);

    bool reorderable = true;    // off while the queue runs

signals:
    void pathsDropped(QStringList paths);
    // the disc at row from to go before the disc now at row to (to = count: last)
    void moveRequested(int from, int to);

protected:
    void startDrag(Qt::DropActions actions) override;
    void dragEnterEvent(QDragEnterEvent *e) override;
    void dragMoveEvent(QDragMoveEvent *e) override;
    void dropEvent(QDropEvent *e) override;

private:
    int targetRow(const QPoint &pos) const;
};
