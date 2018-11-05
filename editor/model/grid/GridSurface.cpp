#include "GridSurface.h"

#include "GridItem.h"
#include "common/WatchSequenceOperators.h"

using namespace AxiomModel;

GridSurface::GridSurface(BoxedItemCollection view, bool deferDirty, QPoint minRect, QPoint maxRect)
    : _grid(minRect, maxRect), _items(std::move(view)),
      _selectedItems(AxiomCommon::boxWatchSequence(AxiomCommon::filterWatch(
          AxiomCommon::refWatchSequence(&_items), [](GridItem *itm) { return itm->isSelected(); }))),
      _deferDirty(deferDirty) {
    _items.events().itemAdded().connectTo(this, &GridSurface::handleItemAdded);
}

GridSurface::ItemCollection GridSurface::items() {
    return AxiomCommon::refWatchSequence(&_items);
}

GridSurface::ItemCollection GridSurface::selectedItems() {
    return AxiomCommon::refWatchSequence(&_selectedItems);
}

void GridSurface::selectAll() {
    auto itms = items();
    for (const auto &item : itms.sequence()) {
        item->select(false);
    }
}

void GridSurface::deselectAll() {
    auto itms = items();
    for (const auto &item : itms.sequence()) {
        item->deselect();
    }
}

void GridSurface::startDragging() {
    lastDragDelta = QPoint(0, 0);
    auto selectedItms = selectedItems();
    for (auto &item : selectedItms.sequence()) {
        item->startDragging();
    }
}

void GridSurface::dragTo(QPoint delta) {
    if (delta == lastDragDelta) return;

    auto selectedItms = selectedItems();
    for (auto &item : selectedItms.sequence()) {
        _grid.setRect(item->pos(), item->size(), nullptr);
    }

    auto availableDelta = findAvailableDelta(delta);
    lastDragDelta = availableDelta;
    for (auto &item : selectedItms.sequence()) {
        item->dragTo(availableDelta);
    }

    for (auto &item : selectedItms.sequence()) {
        _grid.setRect(item->pos(), item->size(), item);
    }
    setDirty();
}

void GridSurface::finishDragging() {
    auto selectedItms = selectedItems();
    for (auto &item : selectedItms.sequence()) {
        item->finishDragging();
    }
}

void GridSurface::setDirty() {
    if (_deferDirty) {
        _isDirty = true;
    } else {
        gridChanged();
        _isDirty = false;
    }
}

void GridSurface::tryFlush() {
    if (_isDirty) {
        gridChanged();
        _isDirty = false;
    }
}

bool GridSurface::isAllDragAvailable(QPoint delta) {
    auto selectedItms = selectedItems();
    for (auto &item : selectedItms.sequence()) {
        if (!item->isDragAvailable(delta)) return false;
    }
    return true;
}

QPoint GridSurface::findAvailableDelta(QPoint delta) {
    if (isAllDragAvailable(delta)) return delta;
    auto xDelta = QPoint(delta.x(), lastDragDelta.y());
    if (isAllDragAvailable(xDelta)) return xDelta;
    auto yDelta = QPoint(lastDragDelta.x(), delta.y());
    if (isAllDragAvailable(yDelta)) return yDelta;
    return lastDragDelta;
}

void GridSurface::handleItemAdded(AxiomModel::GridItem *const &item) {
    item->startedDragging.connectTo(this, &GridSurface::startDragging);
    item->draggedTo.connectTo(this, &GridSurface::dragTo);
    item->finishedDragging.connectTo(this, &GridSurface::finishDragging);
    item->selected.connectTo(this, [this, item](bool exclusive) {
        if (exclusive) {
            // get sequence of selected items that aren't this item
            auto clearItems = filter(_selectedItems.sequence(),
                                     [item](AxiomModel::GridItem *const &filterItem) { return filterItem != item; });
            while (!clearItems.empty()) {
                (*clearItems.begin())->deselect();
            }
        }
        if (_selectedItems.sequence().size() == 1) {
            hasSelectionChanged(true);
        }
    });
    item->deselected.connectTo(this, [this]() {
        if (_selectedItems.sequence().empty()) {
            hasSelectionChanged(false);
        }
    });
}
