#include "NodeSurfaceCanvas.h"

#define _USE_MATH_DEFINES

#include <QtCore/QMimeData>
#include <QtCore/QTimer>
#include <QtGui/QClipboard>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QGraphicsPathItem>
#include <QtWidgets/QGraphicsSceneMouseEvent>
#include <QtWidgets/QLineEdit>
#include <cmath>

#include "../FloatingValueEditor.h"
#include "../GlobalActions.h"
#include "../IConnectable.h"
#include "../connection/WireItem.h"
#include "../node/NodeItem.h"
#include "../windows/MainWindow.h"
#include "AddNodeMenu.h"
#include "NodeSurfacePanel.h"
#include "editor/AxiomApplication.h"
#include "editor/model/ModelRoot.h"
#include "editor/model/PoolOperators.h"
#include "editor/model/actions/CompositeAction.h"
#include "editor/model/actions/CreateConnectionAction.h"
#include "editor/model/actions/CreateCustomNodeAction.h"
#include "editor/model/actions/CreateGroupNodeAction.h"
#include "editor/model/actions/CreatePortalNodeAction.h"
#include "editor/model/actions/DeleteObjectAction.h"
#include "editor/model/actions/PasteBufferAction.h"
#include "editor/model/objects/Connection.h"
#include "editor/model/objects/NodeSurface.h"

using namespace AxiomGui;
using namespace AxiomModel;

QSize NodeSurfaceCanvas::nodeGridSize = QSize(50, 50);

QSize NodeSurfaceCanvas::controlGridSize = QSize(25, 25);

int NodeSurfaceCanvas::wireZVal = 0;
int NodeSurfaceCanvas::activeWireZVal = 1;
int NodeSurfaceCanvas::nodeZVal = 2;
int NodeSurfaceCanvas::activeNodeZVal = 3;
int NodeSurfaceCanvas::panelZVal = 4;
int NodeSurfaceCanvas::selectionZVal = 5;

NodeSurfaceCanvas::NodeSurfaceCanvas(NodeSurfacePanel *panel, NodeSurface *surface) : panel(panel), surface(surface) {
    // build selection
    auto selectionPen = QPen(QColor::fromRgb(52, 152, 219));
    auto selectionBrush = QBrush(QColor::fromRgb(52, 152, 219, 50));

    selectionPath = addPath(QPainterPath(), selectionPen, selectionBrush);
    selectionPath->setVisible(false);
    selectionPath->setZValue(selectionZVal);

    // create items for all nodes & wires that already exist
    for (const auto &node : surface->nodes().sequence()) {
        addNode(node);
    }

    for (const auto &connection : surface->connections().sequence()) {
        connection->wire().then(this, [this](std::unique_ptr<ConnectionWire> &wire) { addWire(wire.get()); });
    }

    // connect to model
    surface->nodes().events().itemAdded().connectTo(this, &NodeSurfaceCanvas::addNode);
    surface->connections().events().itemAdded().connectTo(this, [this](Connection *connection) {
        connection->wire().then([this](std::unique_ptr<ConnectionWire> &wire) { addWire(wire.get()); });
    });

    // start update timer
    auto timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &NodeSurfaceCanvas::doRuntimeUpdate);
    timer->start(16);
}

QPoint NodeSurfaceCanvas::nodeRealPos(const QPoint &p) {
    return {p.x() * NodeSurfaceCanvas::nodeGridSize.width(), p.y() * NodeSurfaceCanvas::nodeGridSize.height()};
}

QPointF NodeSurfaceCanvas::nodeRealPos(const QPointF &p) {
    return {p.x() * NodeSurfaceCanvas::nodeGridSize.width(), p.y() * NodeSurfaceCanvas::nodeGridSize.height()};
}

QSize NodeSurfaceCanvas::nodeRealSize(const QSize &s) {
    return {s.width() * NodeSurfaceCanvas::nodeGridSize.width(), s.height() * NodeSurfaceCanvas::nodeGridSize.height()};
}

QPoint NodeSurfaceCanvas::controlRealPos(const QPoint &p) {
    return {p.x() * NodeSurfaceCanvas::controlGridSize.width(), p.y() * NodeSurfaceCanvas::controlGridSize.height()};
}

QPointF NodeSurfaceCanvas::controlRealPos(const QPointF &p) {
    return {p.x() * NodeSurfaceCanvas::controlGridSize.width(), p.y() * NodeSurfaceCanvas::controlGridSize.height()};
}

QSize NodeSurfaceCanvas::controlRealSize(const QSize &s) {
    return {s.width() * NodeSurfaceCanvas::controlGridSize.width(),
            s.height() * NodeSurfaceCanvas::controlGridSize.height()};
}

void NodeSurfaceCanvas::startConnecting(IConnectable *control) {
    if (connectionWire) return;

    sourceControl = control->sink();
    auto startPos = sourceControl->worldPos();
    connectionWire = std::make_unique<ConnectionWire>(&surface->grid(), &surface->wireGrid(), sourceControl->wireType(),
                                                      startPos, startPos);
    connectionWire->setStartActive(true);
    addWire(&*connectionWire);

    connectionWire->removed.connectTo(this, [this]() { connectionWire.reset(); });
}

static bool canConnectTo(IConnectable *connectable, ConnectionWire *connectionWire, Control *sourceControl) {
    return connectable->sink()->wireType() == connectionWire->wireType() && connectable->sink() != sourceControl;
}

void NodeSurfaceCanvas::updateConnecting(QPointF mousePos) {
    if (!connectionWire) return;

    auto hoverItems = items(mousePos);
    bool foundHoverItem = false;
    for (const auto &hoverItem : hoverItems) {
        if (auto connectable = dynamic_cast<IConnectable *>(hoverItem);
            connectable && canConnectTo(connectable, connectionWire.get(), sourceControl)) {
            connectionWire->setEndPos(connectable->sink()->worldPos());
            foundHoverItem = true;
            break;
        }
    }

    if (!foundHoverItem) {
        connectionWire->setEndPos(QPointF(mousePos.x() / NodeSurfaceCanvas::nodeGridSize.width(),
                                          mousePos.y() / NodeSurfaceCanvas::nodeGridSize.height()));
    }
}

void NodeSurfaceCanvas::endConnecting(QPointF mousePos) {
    if (!connectionWire) return;

    auto hoverItems = items(mousePos);

    for (const auto &hoverItem : hoverItems) {
        if (auto connectable = dynamic_cast<IConnectable *>(hoverItem);
            connectable && canConnectTo(connectable, connectionWire.get(), sourceControl)) {
            // if the sinks are already connected, remove the connection
            auto otherUuid = connectable->sink()->uuid();
            auto connectors =
                filter(sourceControl->connections().sequence(), [otherUuid](Connection *const &connection) {
                    return connection->controlAUuid() == otherUuid || connection->controlBUuid() == otherUuid;
                });
            auto firstConnector = connectors.begin();
            if (firstConnector == connectors.end()) {
                // there isn't currently a connection, create a new one
                surface->root()->history().append(CreateConnectionAction::create(
                    surface->uuid(), sourceControl->uuid(), connectable->sink()->uuid(), surface->root()));
            } else {
                // there is a connection, remove it
                surface->root()->history().append(
                    DeleteObjectAction::create((*firstConnector)->uuid(), surface->root()));
            }

            break;
        }
    }

    connectionWire->remove();
}

void NodeSurfaceCanvas::addNode(AxiomModel::Node *node) {
    auto item = new NodeItem(node, this, panel->window->runtime());
    item->setZValue(nodeZVal);
    addItem(item);
}

void NodeSurfaceCanvas::newNode(QPointF scenePos, QString name, AxiomModel::Node::NodeType type,
                                AxiomModel::ConnectionWire::WireType portalWireType,
                                AxiomModel::PortalControl::PortalType portalType) {
    auto targetPos = QPoint(qRound((float) scenePos.x() / NodeSurfaceCanvas::nodeGridSize.width()),
                            qRound((float) scenePos.y() / NodeSurfaceCanvas::nodeGridSize.height()));

    switch (type) {
    case Node::NodeType::CUSTOM_NODE:
        surface->root()->history().append(
            CreateCustomNodeAction::create(surface->uuid(), targetPos, name, surface->root()));
        break;
    case Node::NodeType::GROUP_NODE:
        surface->root()->history().append(
            CreateGroupNodeAction::create(surface->uuid(), targetPos, name, surface->root()));
        break;
    case Node::NodeType::PORTAL_NODE:
        surface->root()->history().append(CreatePortalNodeAction::create(surface->uuid(), targetPos, name,
                                                                         portalWireType, portalType, surface->root()));
        break;
    }
}

void NodeSurfaceCanvas::addWire(AxiomModel::ConnectionWire *wire) {
    auto item = new WireItem(this, wire);
    item->setZValue(wireZVal);
    addItem(item);
}

void NodeSurfaceCanvas::doRuntimeUpdate() {
    surface->doRuntimeUpdate();
}

void NodeSurfaceCanvas::drawBackground(QPainter *painter, const QRectF &rect) {
    drawGrid(painter, rect, nodeGridSize, QColor::fromRgb(60, 60, 60), 2);
}

void NodeSurfaceCanvas::mousePressEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mousePressEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    switch (event->button()) {
    case Qt::LeftButton:
        leftMousePressEvent(event);
        break;
    default:
        break;
    }
}

void NodeSurfaceCanvas::mouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mouseReleaseEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    switch (event->button()) {
    case Qt::LeftButton:
        leftMouseReleaseEvent(event);
        break;
    default:
        break;
    }
}

void NodeSurfaceCanvas::mouseMoveEvent(QGraphicsSceneMouseEvent *event) {
    QGraphicsScene::mouseMoveEvent(event);
    if (event->isAccepted() && itemAt(event->scenePos(), QTransform()) != selectionPath) return;

    event->ignore();

    if (isSelecting) {
        selectionPoints.append(event->scenePos());

        auto path = QPainterPath();
        path.moveTo(selectionPoints.first());
        for (auto i = 1; i < selectionPoints.size(); i++) {
            path.lineTo(selectionPoints[i]);
        }
        path.closeSubpath();

        selectionPath->setPath(path);
        selectionPath->setVisible(true);

        auto selectItems = items(path);

        std::set<AxiomModel::GridItem *> newSelectedItems;
        for (auto &item : selectItems) {
            auto nodeItem = dynamic_cast<NodeItem *>(item);
            if (!nodeItem) continue;
            newSelectedItems.emplace(nodeItem->node);
        }

        std::vector<AxiomModel::GridItem *> edgeItems;
        std::set_symmetric_difference(lastSelectedItems.begin(), lastSelectedItems.end(), newSelectedItems.begin(),
                                      newSelectedItems.end(), std::back_inserter(edgeItems));

        for (auto &item : edgeItems) {
            if (item->isSelected()) {
                item->deselect();
            } else {
                item->select(false);
            }
        }

        lastSelectedItems = newSelectedItems;

        event->accept();
    }
}

void NodeSurfaceCanvas::contextMenuEvent(QGraphicsSceneContextMenuEvent *event) {
    QGraphicsScene::contextMenuEvent(event);
    if (event->isAccepted()) return;

    auto scenePos = event->scenePos();
    AddNodeMenu menu(surface, "");

    connect(&menu, &AddNodeMenu::newNodeAdded, [this, scenePos]() {
        auto editor = new FloatingValueEditor("New Node", scenePos);
        addItem(editor);
        connect(editor, &FloatingValueEditor::valueSubmitted, this, [this, scenePos](QString value) {
            newNode(scenePos, value, Node::NodeType::CUSTOM_NODE, ConnectionWire::WireType::NUM,
                    PortalControl::PortalType::AUTOMATION);
        });
    });
    connect(&menu, &AddNodeMenu::newGroupAdded, [this, scenePos]() {
        auto editor = new FloatingValueEditor("New Group", scenePos);
        addItem(editor);
        connect(editor, &FloatingValueEditor::valueSubmitted, this, [this, scenePos](QString value) {
            newNode(scenePos, value, Node::NodeType::GROUP_NODE, ConnectionWire::WireType::NUM,
                    PortalControl::PortalType::AUTOMATION);
        });
    });
    connect(&menu, &AddNodeMenu::newPortalAdded,
            [this, scenePos](PortalControl::PortalType portalType, ConnectionWire::WireType wireType) {
                auto defaultText = "";
                switch (portalType) {
                case AxiomModel::PortalControl::PortalType::INPUT:
                    defaultText = "New Input";
                    break;
                case AxiomModel::PortalControl::PortalType::OUTPUT:
                    defaultText = "New Output";
                    break;
                case AxiomModel::PortalControl::PortalType::AUTOMATION:
                    defaultText = "New Automation";
                    break;
                }

                auto editor = new FloatingValueEditor(defaultText, scenePos);
                addItem(editor);
                connect(editor, &FloatingValueEditor::valueSubmitted, this,
                        [this, scenePos, wireType, portalType](QString value) {
                            newNode(scenePos, value, Node::NodeType::PORTAL_NODE, wireType, portalType);
                        });
            });

    menu.exec(event->screenPos());
}

void NodeSurfaceCanvas::leftMousePressEvent(QGraphicsSceneMouseEvent *event) {
    isSelecting = true;
    if (!(event->modifiers() & Qt::ShiftModifier)) {
        surface->grid().deselectAll();
        if (focusItem()) focusItem()->clearFocus();
    }
    lastSelectedItems.clear();
    selectionPoints.append(event->scenePos());
    event->accept();
}

void NodeSurfaceCanvas::leftMouseReleaseEvent(QGraphicsSceneMouseEvent *event) {
    if (!isSelecting) {
        event->ignore();
        return;
    }

    isSelecting = false;
    selectionPoints.clear();
    selectionPath->setVisible(false);
    event->accept();
}

void NodeSurfaceCanvas::drawGrid(QPainter *painter, const QRectF &rect, const QSize &size, const QColor &color,
                                 qreal pointSize) {
    QPointF topLeft = rect.topLeft();
    topLeft.setX(std::floor(topLeft.x() / size.width()) * size.width());
    topLeft.setY(std::floor(topLeft.y() / size.height()) * size.height());

    QPointF bottomRight = rect.bottomRight();
    bottomRight.setX(std::ceil(bottomRight.x() / size.width()) * size.width());
    bottomRight.setY(std::ceil(bottomRight.y() / size.height()) * size.height());

    auto drawPen = QPen(color);
    drawPen.setWidthF(pointSize);
    painter->setPen(drawPen);

    for (auto x = topLeft.x(); x < bottomRight.x(); x += size.width()) {
        for (auto y = topLeft.y(); y < bottomRight.y(); y += size.height()) {
            painter->drawPoint((int) x + 1, (int) y + 1);
        }
    }
}
