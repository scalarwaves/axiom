#pragma once
#include <QtCore/QString>
#include <QtCore/QObject>

#include "Library.h"
#include "RootSchematic.h"

namespace AxiomModel {

    class Project {
    public:
        QString path;

        Library library;
        RootSchematic root;
    };

}
