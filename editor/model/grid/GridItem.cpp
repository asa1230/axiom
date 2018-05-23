#include "GridItem.h"

#include <QtCore/QDataStream>

#include "GridSurface.h"

using namespace AxiomModel;

GridItem::GridItem(GridSurface *parent, QPoint pos, QSize size, bool selected)
    : parentSurface(parent), m_pos(parent->grid().findNearestAvailable(pos, size)), m_size(size), m_selected(selected) {
    parentSurface->grid().setRect(m_pos, m_size, this);
    parentSurface->flushGrid();
}

GridItem::~GridItem() {
    parentSurface->grid().setRect(m_pos, m_size, nullptr);
    parentSurface->flushGrid();
}

void GridItem::deserialize(QDataStream &stream, QPoint &pos, QSize &size, bool &selected) {
    stream >> pos;
    stream >> size;
    stream >> selected;
}

void GridItem::serialize(QDataStream &stream) const {
    stream << pos();
    stream << size();
    stream << isSelected();
}

bool GridItem::isDragAvailable(QPoint delta) {
    return parentSurface->grid().isRectAvailable(_dragStartPos + delta, m_size, this);
}

void GridItem::setSize(QSize size) {
    if (!isResizable()) return;

    if (size != m_size) {
        if (size.width() < 1 || size.height() < 1 || !parentSurface->grid().isRectAvailable(m_pos, size, this)) return;

        beforeSizeChanged.trigger(size);
        parentSurface->grid().moveRect(m_pos, m_size, m_pos, size, this);
        parentSurface->flushGrid();
        m_size = size;
        sizeChanged.trigger(size);
    }
}

void GridItem::setRect(QRect rect) {
    setCorners(rect.topLeft(), rect.topLeft() + QPoint(rect.width(), rect.height()));
}

void GridItem::setCorners(QPoint topLeft, QPoint bottomRight) {
    if (!isResizable()) return;

    auto newSize = QSize(bottomRight.x() - topLeft.x(), bottomRight.y() - topLeft.y());
    if (newSize.width() < 1 || newSize.height() < 1) return;
    if (topLeft == m_pos && newSize == m_size) return;

    if (!parentSurface->grid().isRectAvailable(topLeft, newSize, this)) {
        // try resizing just horizontally or just vertically
        auto hTopLeft = QPoint(topLeft.x(), m_pos.y());
        auto hSize = QSize(newSize.width(), m_size.height());
        auto vTopLeft = QPoint(m_pos.x(), topLeft.y());
        auto vSize = QSize(m_size.width(), newSize.height());
        if (parentSurface->grid().isRectAvailable(hTopLeft, hSize, this)) {
            topLeft = hTopLeft;
            newSize = hSize;
        } else if (parentSurface->grid().isRectAvailable(vTopLeft, vSize, this)) {
            topLeft = vTopLeft;
            newSize = vSize;
        } else return;
    }

    if (topLeft == m_pos && newSize == m_size) return;
    parentSurface->grid().moveRect(m_pos, m_size, topLeft, newSize, this);
    parentSurface->flushGrid();
    beforePosChanged.trigger(topLeft);
    m_pos = topLeft;
    posChanged.trigger(m_pos);
    beforeSizeChanged.trigger(newSize);
    m_size = newSize;
    sizeChanged.trigger(m_size);
}

void GridItem::select(bool exclusive) {
    if (exclusive || !m_selected) {
        m_selected = true;
        selectedChanged.trigger(m_selected);
        selected.trigger(exclusive);
    }
}

void GridItem::deselect() {
    if (!m_selected) return;
    m_selected = false;
    selectedChanged.trigger(m_selected);
    deselected.trigger();
}

void GridItem::startDragging() {
    _dragStartPos = m_pos;
}

void GridItem::dragTo(QPoint delta) {
    setPos(_dragStartPos + delta, false, false);
}

void GridItem::finishDragging() {
}

void GridItem::setPos(QPoint pos, bool updateGrid, bool checkPositions) {
    if (pos != m_pos) {
        if (checkPositions && !parentSurface->grid().isRectAvailable(pos, m_size, this)) return;

        if (pos == m_pos) return;

        beforePosChanged.trigger(pos);
        if (updateGrid) {
            parentSurface->grid().moveRect(m_pos, m_size, pos, m_size, this);
            parentSurface->flushGrid();
        }
        m_pos = pos;
        posChanged.trigger(pos);
    }
}