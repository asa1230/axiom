#pragma once

#include "Schematic.h"

namespace MaximRuntime {

    class RootSchematic : public Schematic {
    public:
        explicit RootSchematic(Runtime *runtime);

        void *getValuePtr(void *parentCtx) override;

    };

}
