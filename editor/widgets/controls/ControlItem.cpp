#include "ControlItem.h"

#include <QtCore/QPropertyAnimation>
#include <QtCore/QSignalTransition>
#include <QtCore/QStateMachine>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtWidgets/QGraphicsSceneMouseEvent>

#include "../CommonColors.h"
#include "../ItemResizer.h"
#include "../surface/NodeSurfaceCanvas.h"
#include "editor/model/ModelRoot.h"
#include "editor/model/Project.h"
#include "editor/model/actions/CompositeAction.h"
#include "editor/model/actions/DeleteObjectAction.h"
#include "editor/model/actions/ExposeControlAction.h"
#include "editor/model/actions/GridItemMoveAction.h"
#include "editor/model/actions/GridItemSizeAction.h"
#include "editor/model/actions/SetShowNameAction.h"
#include "editor/model/actions/UnexposeControlAction.h"
#include "editor/model/objects/Connection.h"
#include "editor/model/objects/Control.h"
#include "editor/model/objects/ControlSurface.h"
#include "editor/model/objects/Node.h"
#include "editor/model/objects/NodeSurface.h"
#include "editor/util.h"

using namespace AxiomGui;
using namespace AxiomModel;

ControlItem::ControlItem(Control *control, NodeSurfaceCanvas *canvas) : control(control), canvas(canvas) {
    setAcceptHoverEvents(true);

    control->nameChanged.connectTo(this, &ControlItem::triggerUpdate);
    control->posChanged.connectTo(this, &ControlItem::setPos);
    control->beforeSizeChanged.connectTo(this, &ControlItem::triggerGeometryChange);
    control->sizeChanged.connectTo(this, &ControlItem::setSize);
    control->selectedChanged.connectTo(this, &ControlItem::updateSelected);
    control->isActiveChanged.connectTo(this, &ControlItem::triggerUpdate);
    control->showNameChanged.connectTo(this, &ControlItem::triggerUpdate);
    control->exposerUuidChanged.connectTo(this, &ControlItem::triggerUpdate);
    control->isEnabledChanged.connectTo(this, &ControlItem::triggerUpdate);
    control->removed.connectTo(this, &ControlItem::remove);

    // create resize items
    if (control->isResizable()) {
        ItemResizer::Direction directions[] = {
            ItemResizer::TOP,       ItemResizer::RIGHT,    ItemResizer::BOTTOM,       ItemResizer::LEFT,
            ItemResizer::TOP_RIGHT, ItemResizer::TOP_LEFT, ItemResizer::BOTTOM_RIGHT, ItemResizer::BOTTOM_LEFT};
        for (auto i = 0; i < 8; i++) {
            auto resizer = new ItemResizer(
                directions[i], QSize(NodeSurfaceCanvas::controlGridSize.width() * control->minSize().width(),
                                     NodeSurfaceCanvas::controlGridSize.height() * control->minSize().height()));
            resizer->enablePainting();
            resizer->setVisible(false);

            // ensure corners are on top of edges
            resizer->setZValue(i > 3 ? 3 : 2);

            connect(this, &ControlItem::resizerPosChanged, resizer, &ItemResizer::setPos);
            connect(this, &ControlItem::resizerSizeChanged, resizer, &ItemResizer::setSize);

            connect(resizer, &ItemResizer::startDrag, this, &ControlItem::resizerStartDrag);
            connect(resizer, &ItemResizer::changed, this, &ControlItem::resizerChanged);
            connect(resizer, &ItemResizer::endDrag, this, &ControlItem::resizerEndDrag);

            control->selected.connectTo(this, [resizer](bool) { resizer->setVisible(true); });
            control->deselected.connectTo(this, [resizer]() { resizer->setVisible(false); });

            resizer->setParentItem(this);
        }
    }

    // set initial state
    setPos(control->pos());
    setSize(control->size());

    // build a state machine for hovering animation
    auto machine = new QStateMachine(this);
    auto unhoveredState = new QState(machine);
    unhoveredState->assignProperty(this, "hoverState", 0);
    machine->setInitialState(unhoveredState);

    auto hoveredState = new QState(machine);
    hoveredState->assignProperty(this, "hoverState", 1);

    auto mouseEnterTransition = unhoveredState->addTransition(this, &ControlItem::mouseEnter, hoveredState);
    auto mouseLeaveTransition = hoveredState->addTransition(this, &ControlItem::mouseLeave, unhoveredState);

    auto anim = new QPropertyAnimation(this, "hoverState", this);
    anim->setDuration(100);
    mouseEnterTransition->addAnimation(anim);
    mouseLeaveTransition->addAnimation(anim);

    machine->start();
}

QRectF ControlItem::boundingRect() const {
    auto br = drawBoundingRect();
    // if (!showLabelInCenter()) {
    br.setHeight(br.height() + 20);
    //}
    return br;
}

QPainterPath ControlItem::shape() const {
    if (control->isSelected()) {
        QPainterPath path;
        path.addRect(drawBoundingRect());
        return path;
    } else {
        return controlPath();
    }
}

QRectF ControlItem::aspectBoundingRect() const {
    auto bound = drawBoundingRect();
    if (bound.size().width() > bound.size().height()) {
        return {
            QPointF(bound.topLeft().x() + bound.size().width() / 2 - bound.size().height() / 2, bound.topLeft().y()),
            QSizeF(bound.size().height(), bound.size().height())};
    } else {
        return {
            QPointF(bound.topLeft().x(), bound.topLeft().y() + bound.size().height() / 2 - bound.size().width() / 2),
            QSizeF(bound.size().width(), bound.size().width())};
    }
}

void ControlItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) {
    // draw an outline if we're exposed
    if (!control->exposerUuid().isNull()) {
        auto bounds = controlPath();
        painter->setPen(QPen(CommonColors::exposedBorder, control->connections().sequence().empty() ? 3 : 7));
        painter->setBrush(QBrush(CommonColors::exposedBorder));
        painter->drawPath(bounds);

        if (!control->connections().sequence().empty()) {
            painter->setPen(QPen(Qt::black, 4));
            painter->setBrush(QBrush(Qt::black));
            painter->drawPath(bounds);
        }
    }

    // draw an outline if we're connected to something
    if (!control->connections().sequence().empty()) {
        auto isEnabled = control->isEnabled();
        auto useNormalColor = isEnabled ? outlineNormalColor() : CommonColors::disabledNormal;
        auto useActiveColor = isEnabled ? outlineActiveColor() : CommonColors::disabledActive;

        auto bounds = controlPath();
        auto activeColor = AxiomUtil::mixColor(useNormalColor, useActiveColor, control->isActive());
        painter->setPen(QPen(activeColor, 3));
        painter->setBrush(QBrush(activeColor));
        painter->drawPath(bounds);
    }

    paintControl(painter);

    if (!control->showName()) return;

    painter->setPen(QPen(QColor(100, 100, 100)));
    painter->setBrush(Qt::NoBrush);

    auto pathBr = useBoundingRect();
    /*if (showLabelInCenter()) {
        painter->drawText(pathBr, Qt::AlignCenter, getLabelText());
    } else {*/
    auto br = boundingRect();
    auto nameBr = QRectF(br.left(), pathBr.bottom() + 5, br.width(), 20);
    painter->drawText(nameBr, Qt::AlignHCenter | Qt::AlignTop, getLabelText());
    //}
}

bool ControlItem::isEditable() const {
    return !control->isSelected();
}

void ControlItem::setHoverState(float hoverState) {
    if (hoverState != _hoverState) {
        _hoverState = hoverState;
        update();
    }
}

QRectF ControlItem::drawBoundingRect() const {
    return {QPoint(0, 0), NodeSurfaceCanvas::controlRealSize(control->size())};
}

QColor ControlItem::outlineNormalColor() const {
    switch (control->wireType()) {
    case ConnectionWire::WireType::NUM:
        return CommonColors::numNormal;
    case ConnectionWire::WireType::MIDI:
        return CommonColors::midiNormal;
    }
    unreachable;
}

QColor ControlItem::outlineActiveColor() const {
    switch (control->wireType()) {
    case ConnectionWire::WireType::NUM:
        return CommonColors::numActive;
    case ConnectionWire::WireType::MIDI:
        return CommonColors::midiActive;
    }
    unreachable;
}

void ControlItem::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    if (event->button() == Qt::LeftButton && event->modifiers() & Qt::ControlModifier) {
        event->accept();
        isConnecting = true;
        canvas->startConnecting(this);
    } else if (!isEditable() && event->button() == Qt::LeftButton) {
        event->accept();
        isMoving = true;
        mouseStartPoint = event->scenePos();
        control->startedDragging();
    } else {
        event->setAccepted(control->isMovable());
        control->surface()->grid().deselectAll();
    }
}

void ControlItem::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    if (isConnecting) {
        event->accept();

        canvas->updateConnecting(event->scenePos());
    } else if (isMoving) {
        event->accept();

        auto mouseDelta = event->scenePos() - mouseStartPoint;
        control->draggedTo(QPoint(qRound((float) mouseDelta.x() / NodeSurfaceCanvas::controlGridSize.width()),
                                  qRound((float) mouseDelta.y() / NodeSurfaceCanvas::controlGridSize.height())));
    } else {
        event->ignore();
    }
}

void ControlItem::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (isConnecting) {
        event->accept();

        isConnecting = false;
        canvas->endConnecting(event->scenePos());
    } else if (isMoving) {
        event->accept();

        isMoving = false;
        control->finishedDragging();

        if (control->dragStartPos() != control->pos()) {
            control->root()->history().append(
                GridItemMoveAction::create(control->uuid(), control->dragStartPos(), control->pos(), control->root()));
        }
    } else {
        event->ignore();
    }
}

void ControlItem::mouseDoubleClickEvent(QGraphicsSceneMouseEvent *event) {
    control->setIsActive(false);
    emit mouseLeave();
    control->select(true);
    mousePressEvent(event);
}

void ControlItem::hoverEnterEvent(QGraphicsSceneHoverEvent *event) {
    if (!isEditable()) return;

    control->setIsActive(true);
    emit mouseEnter();
}

void ControlItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *event) {
    if (!isEditable()) return;

    control->setIsActive(false);
    emit mouseLeave();
}

void ControlItem::setPos(QPoint newPos) {
    auto realPos = NodeSurfaceCanvas::controlRealPos(newPos);
    QGraphicsItem::setPos(realPos.x(), realPos.y());
    emit resizerPosChanged(realPos);
}

void ControlItem::setSize(QSize newSize) {
    emit resizerSizeChanged(NodeSurfaceCanvas::controlRealSize(newSize));
}

void ControlItem::updateSelected(bool selected) {
    if (selected) {
        setCursor(Qt::SizeAllCursor);
    } else {
        unsetCursor();
    }
}

void ControlItem::remove() {
    if (scene()) {
        scene()->removeItem(this);
    }
}

void ControlItem::resizerChanged(QPointF topLeft, QPointF bottomRight) {
    control->setCorners(QPoint(qRound(topLeft.x() / NodeSurfaceCanvas::controlGridSize.width()),
                               qRound(topLeft.y() / NodeSurfaceCanvas::controlGridSize.height())),
                        QPoint(qRound(bottomRight.x() / NodeSurfaceCanvas::controlGridSize.width()),
                               qRound(bottomRight.y() / NodeSurfaceCanvas::controlGridSize.height())));
}

void ControlItem::resizerStartDrag() {
    control->select(true);
    startDragRect = control->rect();
}

void ControlItem::resizerEndDrag() {
    auto endDragRect = control->rect();
    if (startDragRect != endDragRect) {
        control->root()->history().append(
            GridItemSizeAction::create(control->uuid(), startDragRect, endDragRect, control->root()));
    }
}

void ControlItem::triggerGeometryChange() {
    prepareGeometryChange();
}

void ControlItem::triggerUpdate() {
    update();
}

void ControlItem::buildMenuStart(QMenu &menu) {
    auto clearAction = menu.addAction("&Clear Connections");
    clearAction->setEnabled(!control->connections().sequence().empty());

    connect(clearAction, &QAction::triggered, this, [this]() {
        std::vector<std::unique_ptr<Action>> clearActions;
        for (const auto &connection : control->connections().sequence()) {
            clearActions.push_back(DeleteObjectAction::create(connection->uuid(), connection->root()));
        }
        control->root()->history().append(CompositeAction::create(std::move(clearActions), control->root()));
    });
}

void ControlItem::buildMenuEnd(QMenu &menu) {
    auto moveAction = menu.addAction("&Move");
    connect(moveAction, &QAction::triggered, this, [this]() { control->select(true); });

    auto nameShownAction = menu.addAction("Show &Name");
    nameShownAction->setCheckable(true);
    nameShownAction->setChecked(control->showName());
    connect(nameShownAction, &QAction::triggered, this, [this, nameShownAction]() {
        control->root()->history().append(SetShowNameAction::create(control->uuid(), control->showName(),
                                                                    nameShownAction->isChecked(), control->root()));
    });

    auto exposedAction = menu.addAction("&Expose");
    exposedAction->setEnabled(control->surface()->node()->surface()->canExposeControl());
    exposedAction->setCheckable(true);
    exposedAction->setChecked(!control->exposerUuid().isNull());
    connect(exposedAction, &QAction::triggered, this, [this, exposedAction]() {
        assert(exposedAction->isChecked() == control->exposerUuid().isNull());
        if (exposedAction->isChecked()) {
            control->root()->history().append(ExposeControlAction::create(control->uuid(), control->root()));
        } else {
            control->root()->history().append(UnexposeControlAction::create(control->uuid(), control->root()));
        }
    });
}

QString ControlItem::getLabelText() const {
    return control->name();
}
