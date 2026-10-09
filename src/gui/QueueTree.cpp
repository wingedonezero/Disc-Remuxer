#include "QueueTree.h"

#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>

QueueTree::QueueTree(QWidget *parent) : QTreeWidget(parent)
{
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(true);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::MoveAction);
}

void QueueTree::startDrag(Qt::DropActions actions)
{
    // only a disc (a top row) moves
    if (reorderable && currentItem() && !currentItem()->parent())
        QTreeWidget::startDrag(actions);
}

void QueueTree::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls() || (e->source() == this && reorderable))
        e->acceptProposedAction();
    else
        e->ignore();
}

void QueueTree::dragMoveEvent(QDragMoveEvent *e)
{
    if (e->mimeData()->hasUrls() || (e->source() == this && reorderable)) {
        QTreeWidget::dragMoveEvent(e);
        e->acceptProposedAction();
    } else {
        e->ignore();
    }
}

// The top row a drop at pos goes before: the disc under it (or the disc of
// the title or track under it), after it in its lower half.
int QueueTree::targetRow(const QPoint &pos) const
{
    QTreeWidgetItem *it = itemAt(pos);
    if (!it)
        return topLevelItemCount();
    QTreeWidgetItem *top = it;
    while (top->parent())
        top = top->parent();
    int row = indexOfTopLevelItem(top);
    QRect r = visualItemRect(it);
    return it == top && pos.y() > r.center().y() ? row + 1 : row;
}

void QueueTree::dropEvent(QDropEvent *e)
{
    if (e->mimeData()->hasUrls()) {
        QStringList paths;
        for (const QUrl &u : e->mimeData()->urls())
            if (u.isLocalFile())
                paths << u.toLocalFile();
        e->acceptProposedAction();
        emit pathsDropped(paths);
        return;
    }
    if (e->source() == this && reorderable && currentItem() && !currentItem()->parent()) {
        int from = indexOfTopLevelItem(currentItem()), to = targetRow(e->position().toPoint());
        // the tree is changed by the window, not by Qt's own move
        e->setDropAction(Qt::IgnoreAction);
        e->accept();
        if (to != from && to != from + 1)
            emit moveRequested(from, to);
        return;
    }
    e->ignore();
}
