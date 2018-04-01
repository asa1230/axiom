#include "NodeMidiControl.h"

#include "compiler/runtime/Control.h"
#include "compiler/runtime/ControlGroup.h"
#include "compiler/runtime/Node.h"
#include "compiler/runtime/Runtime.h"

using namespace AxiomModel;

NodeMidiControl::NodeMidiControl(Node *node, MaximRuntime::Control *runtime, QPoint pos, QSize size)
    : NodeControl(node, runtime, pos, size), m_sink(runtime) {
    initSink();

    connect(&m_sink, &MidiConnectionSink::valueChanged,
            this, &NodeMidiControl::valueChanged);
}

void NodeMidiControl::doRuntimeUpdate() {
    setValue(runtime()->group()->getMidiValue());
}

void NodeMidiControl::setValue(MaximRuntime::MidiValue value, bool setRuntime) {
    m_sink.setValue(value);
    if (setRuntime) runtime()->group()->setMidiValue(m_sink.value());
}

void NodeMidiControl::pushEvent(MaximRuntime::MidiEventValue event) {
    runtime()->group()->pushMidiEvent(event);
}

void NodeMidiControl::setMode(Mode mode) {
    if (mode != m_mode) {
        m_mode = mode;
        emit modeChanged(mode);
    }
}

void NodeMidiControl::serialize(QDataStream &stream) const {
    m_sink.value().serialize(stream);
}

void NodeMidiControl::deserialize(QDataStream &stream) {
    MaximRuntime::MidiValue val;
    val.deserialize(stream);
    m_sink.setValue(val);
}
