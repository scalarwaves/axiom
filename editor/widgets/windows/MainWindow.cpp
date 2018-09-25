#include "MainWindow.h"

#include <QtCore/QStandardPaths>
#include <QtCore/QStringBuilder>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>

#include "../GlobalActions.h"
#include "../history/HistoryPanel.h"
#include "../modulebrowser/ModuleBrowserPanel.h"
#include "../surface/NodeSurfacePanel.h"
#include "AboutWindow.h"
#include "editor/AxiomApplication.h"
#include "editor/backend/AudioBackend.h"
#include "editor/model/Library.h"
#include "editor/model/LibraryEntry.h"
#include "editor/model/PoolOperators.h"
#include "editor/model/objects/RootSurface.h"
#include "editor/model/serialize/LibrarySerializer.h"
#include "editor/model/serialize/ProjectSerializer.h"
#include "editor/resources/resource.h"

using namespace AxiomGui;

MainWindow::MainWindow(AxiomBackend::AudioBackend *backend)
    : _backend(backend), _runtime(true, true), libraryLock(globalLibraryLockPath()) {
    setCentralWidget(nullptr);
    setWindowTitle(tr(VER_PRODUCTNAME_STR));
    setWindowIcon(QIcon(":/application.ico"));

    resize(1440, 810);

    setUnifiedTitleAndToolBarOnMac(true);
    setDockNestingEnabled(true);
    setTabPosition(Qt::AllDockWidgetAreas, QTabWidget::North);

    // build
    auto fileMenu = menuBar()->addMenu(tr("&File"));
    fileMenu->addAction(GlobalActions::fileNew);
    fileMenu->addSeparator();

    fileMenu->addAction(GlobalActions::fileImportLibrary);
    fileMenu->addAction(GlobalActions::fileExportLibrary);
    fileMenu->addSeparator();

    fileMenu->addAction(GlobalActions::fileOpen);
    fileMenu->addAction(GlobalActions::fileSave);
    fileMenu->addAction(GlobalActions::fileSaveAs);
    fileMenu->addSeparator();

    fileMenu->addAction(GlobalActions::fileExport);
    fileMenu->addSeparator();

    fileMenu->addAction(GlobalActions::fileQuit);

    auto editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(GlobalActions::editUndo);
    editMenu->addAction(GlobalActions::editRedo);
    editMenu->addSeparator();

    editMenu->addAction(GlobalActions::editCut);
    editMenu->addAction(GlobalActions::editCopy);
    editMenu->addAction(GlobalActions::editPaste);
    editMenu->addAction(GlobalActions::editDelete);
    editMenu->addSeparator();

    editMenu->addAction(GlobalActions::editSelectAll);
    editMenu->addSeparator();

    editMenu->addAction(GlobalActions::editPreferences);

    _viewMenu = menuBar()->addMenu(tr("&View"));

    auto helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(GlobalActions::helpAbout);

    // connect menu things
    connect(GlobalActions::fileNew, &QAction::triggered, this, &MainWindow::newProject);
    connect(GlobalActions::fileOpen, &QAction::triggered, this, &MainWindow::openProject);
    connect(GlobalActions::fileSave, &QAction::triggered, this, &MainWindow::saveProject);
    connect(GlobalActions::fileSaveAs, &QAction::triggered, this, &MainWindow::saveAsProject);
    connect(GlobalActions::fileExport, &QAction::triggered, this, &MainWindow::exportProject);
    connect(GlobalActions::fileQuit, &QAction::triggered, QApplication::quit);
    connect(GlobalActions::fileImportLibrary, &QAction::triggered, this, &MainWindow::importLibrary);
    connect(GlobalActions::fileExportLibrary, &QAction::triggered, this, &MainWindow::exportLibrary);

    connect(GlobalActions::helpAbout, &QAction::triggered, this, &MainWindow::showAbout);
}

MainWindow::~MainWindow() = default;

NodeSurfacePanel *MainWindow::showSurface(NodeSurfacePanel *fromPanel, AxiomModel::NodeSurface *surface, bool split,
                                          bool permanent) {
    auto openPanel = _openPanels.find(surface);
    if (openPanel != _openPanels.end()) {
        openPanel->second->raise();
        return openPanel->second.get();
    }

    auto newDock = std::make_unique<NodeSurfacePanel>(this, surface);
    auto newDockPtr = newDock.get();
    newDock->setAllowedAreas(Qt::AllDockWidgetAreas);
    if (!fromPanel) {
        addDockWidget(Qt::LeftDockWidgetArea, newDockPtr);
    } else if (split) {
        splitDockWidget(fromPanel, newDockPtr, Qt::Horizontal);
    } else {
        tabifyDockWidget(fromPanel, newDockPtr);

        // raise() doesn't seem to work when called synchronously after tabifyDockWidget, so we wait for the next
        // event loop iteration
        QTimer::singleShot(0, newDockPtr, [newDockPtr]() {
            newDockPtr->raise();
            newDockPtr->setFocus(Qt::OtherFocusReason);
        });
    }

    if (!permanent) {
        connect(newDockPtr, &NodeSurfacePanel::closed, this, [this, surface]() { removeSurface(surface); });
    }

    auto dockPtr = newDock.get();
    _openPanels.emplace(surface, std::move(newDock));
    return dockPtr;
}

void MainWindow::showAbout() {
    AboutWindow().exec();
}

void MainWindow::newProject() {
    if (_project && !checkCloseProject()) {
        return;
    }

    setProject(std::make_unique<AxiomModel::Project>(_backend->createDefaultConfiguration()));
    importLibraryFrom(":/default.axl");
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (checkCloseProject()) {
        event->accept();
    } else {
        event->ignore();
    }
}

bool MainWindow::event(QEvent *event) {
    if (event->type() == QEvent::WindowActivate) {
        // block until the global library is unlocked
        testLockGlobalLibrary();
    }

    return QMainWindow::event(event);
}

void MainWindow::setProject(std::unique_ptr<AxiomModel::Project> project) {
    // cleanup old project state
    _openPanels.clear();
    if (_historyPanel) {
        _historyPanel->close();
    }
    if (_modulePanel) {
        _modulePanel->close();
    }

    _project = std::move(project);

    // attach the backend and our runtime
    _project->attachBackend(_backend);
    _project->mainRoot().attachRuntime(runtime());

    // find root surface and show it
    auto defaultSurface =
        AxiomModel::getFirst(AxiomModel::findChildrenWatch(_project->mainRoot().nodeSurfaces(), QUuid()));
    assert(defaultSurface.value());
    auto surfacePanel = showSurface(nullptr, *defaultSurface.value(), false, true);

    _historyPanel = std::make_unique<HistoryPanel>(&_project->mainRoot().history(), this);
    addDockWidget(Qt::RightDockWidgetArea, _historyPanel.get());
    _historyPanel->hide();

    //_modulePanel = std::make_unique<ModuleBrowserPanel>(this, &_project->library(), this);
    // addDockWidget(Qt::BottomDockWidgetArea, _modulePanel.get());

    _viewMenu->addAction(surfacePanel->toggleViewAction());
    //_viewMenu->addAction(_modulePanel->toggleViewAction());
    _viewMenu->addAction(_historyPanel->toggleViewAction());

    updateWindowTitle(_project->linkedFile(), _project->isDirty());
    _project->linkedFileChanged.connect(
        [this](const QString &newName) { updateWindowTitle(newName, _project->isDirty()); });
    _project->isDirtyChanged.connect([this](bool isDirty) { updateWindowTitle(_project->linkedFile(), isDirty); });
}

QString MainWindow::globalLibraryLockPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("library.lock");
}

void MainWindow::lockGlobalLibrary() {
    libraryLock.lock();
}

void MainWindow::unlockGlobalLibrary() {
    libraryLock.unlock();
}

void MainWindow::testLockGlobalLibrary() {
    libraryLock.lock();
    libraryLock.unlock();
}

void MainWindow::removeSurface(AxiomModel::NodeSurface *surface) {
    _openPanels.erase(surface);
}

void MainWindow::saveProject() {
    if (_project->linkedFile().isEmpty()) {
        saveAsProject();
    } else {
        saveProjectTo(_project->linkedFile());
    }
}

void MainWindow::saveAsProject() {
    auto selectedFile = QFileDialog::getSaveFileName(this, "Save Project", QString(),
                                                     tr("Axiom Project Files (*.axp);;All Files (*.*)"));
    if (selectedFile.isNull()) return;
    saveProjectTo(selectedFile);
}

void MainWindow::saveProjectTo(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox(QMessageBox::Critical, "Failed to save project", "The file you selected couldn't be opened.",
                    QMessageBox::Ok)
            .exec();
        return;
    }

    QDataStream stream(&file);
    AxiomModel::ProjectSerializer::serialize(_project.get(), stream, [](QDataStream &) {});
    file.close();

    _project->setIsDirty(false);
    _project->setLinkedFile(path);
}

void MainWindow::openProject() {
    if (!checkCloseProject()) return;

    auto selectedFile = QFileDialog::getOpenFileName(this, "Open Project", QString(),
                                                     tr("Axiom Project Files (*.axp);;All Files (*.*)"));
    if (selectedFile.isNull()) return;

    QFile file(selectedFile);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox(QMessageBox::Critical, "Failed to open project", "The file you selected couldn't be opened.",
                    QMessageBox::Ok)
            .exec();
        return;
    }

    QDataStream stream(&file);
    uint32_t readVersion = 0;
    auto newProject = AxiomModel::ProjectSerializer::deserialize(
        stream, &readVersion, [selectedFile](QDataStream &, uint32_t) { return selectedFile; });
    file.close();

    if (!newProject) {
        if (readVersion) {
            QMessageBox(QMessageBox::Critical, "Failed to load project",
                        "The file you selected was created with an incompatible version of Axiom.\n\n"
                        "Expected version: between " +
                            QString::number(AxiomModel::ProjectSerializer::minSchemaVersion) + " and " +
                            QString::number(AxiomModel::ProjectSerializer::schemaVersion) +
                            ", actual version: " + QString::number(readVersion) + ".",
                        QMessageBox::Ok)
                .exec();
        } else {
            QMessageBox(QMessageBox::Critical, "Failed to load project",
                        "The file you selected is an invalid project file (bad magic header).\n"
                        "Maybe it's corrupt?",
                        QMessageBox::Ok)
                .exec();
        }
    } else {
        setProject(std::move(newProject));
    }
}

void MainWindow::exportProject() {
    /*project()->rootSurface()->saveValue();
    MaximRuntime::Exporter exporter(runtime->ctx(), &runtime->libModule());
    exporter.addRuntime(runtime, "definition");
    std::error_code err;
    llvm::raw_fd_ostream dest("output.o", err, llvm::sys::fs::F_None);
    exporter.exportObject(dest, 2, 2);
    project()->rootSurface()->restoreValue();*/
}

void MainWindow::importLibrary() {
    auto selectedFile = QFileDialog::getOpenFileName(this, "Import Library", QString(),
                                                     tr("Axiom Library Files (*.axl);;All Files (*.*)"));
    if (selectedFile.isNull()) return;
    importLibraryFrom(selectedFile);
}

void MainWindow::exportLibrary() {
    auto selectedFile = QFileDialog::getSaveFileName(this, "Export Library", QString(),
                                                     tr("Axiom Library Files (*.axl);;All Files (*.*)"));
    if (selectedFile.isNull()) return;

    QFile file(selectedFile);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox(QMessageBox::Critical, "Failed to export library", "The file you selected couldn't be opened.",
                    QMessageBox::Ok)
            .exec();
        return;
    }

    QDataStream stream(&file);
    AxiomModel::ProjectSerializer::writeHeader(stream, AxiomModel::ProjectSerializer::librarySchemaMagic);
    // AxiomModel::LibrarySerializer::serialize(&_project->library(), stream);
    file.close();
}

void MainWindow::importLibraryFrom(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox(QMessageBox::Critical, "Failed to import library", "The file you selected couldn't be opened.",
                    QMessageBox::Ok)
            .exec();
        return;
    }

    QDataStream stream(&file);
    uint32_t readVersion = 0;
    if (!AxiomModel::ProjectSerializer::readHeader(stream, AxiomModel::ProjectSerializer::librarySchemaMagic,
                                                   &readVersion)) {
        if (readVersion) {
            QMessageBox(QMessageBox::Critical, "Failed to load library",
                        "The file you selected was created with an incompatible version of Axiom.\n\n"
                        "Expected version: between " +
                            QString::number(AxiomModel::ProjectSerializer::minSchemaVersion) + " and " +
                            QString::number(AxiomModel::ProjectSerializer::schemaVersion) +
                            ", actual version: " + QString::number(readVersion) + ".",
                        QMessageBox::Ok)
                .exec();
        } else {
            QMessageBox(QMessageBox::Critical, "Failed to load library",
                        "The file you selected is an invalid library file (bad magic header).\n"
                        "Maybe it's corrupt?",
                        QMessageBox::Ok)
                .exec();
        }
        return;
    }

    auto mergeLibrary = AxiomModel::LibrarySerializer::deserialize(stream, readVersion, _project.get());
    file.close();
    /*_project->library().import(
        mergeLibrary.get(), [](AxiomModel::LibraryEntry *oldEntry, AxiomModel::LibraryEntry *newEntry) {
            auto currentNewer = oldEntry->modificationDateTime() > newEntry->modificationDateTime();

            QMessageBox msgBox(
                QMessageBox::Warning, "Module import conflict",
                tr("Heads up! One of the modules in the imported library is conflicting with one you already had.\n\n"
                   "Original module (") +
                    (currentNewer ? "newer" : "older") +
                    ")\n"
                    "Name: " +
                    oldEntry->name() +
                    "\n"
                    "Last edit: " +
                    oldEntry->modificationDateTime().toLocalTime().toString() +
                    "\n\n"
                    "Imported module (" +
                    (currentNewer ? "older" : "newer") +
                    ")\n"
                    "Name: " +
                    newEntry->name() +
                    "\n"
                    "Last edit: " +
                    newEntry->modificationDateTime().toLocalTime().toString() +
                    "\n\n"
                    "Would you like to keep the current module, imported one, or both?");
            auto cancelBtn = msgBox.addButton(QMessageBox::Cancel);
            auto currentBtn = msgBox.addButton("Current", QMessageBox::ActionRole);
            auto importedBtn = msgBox.addButton("Imported", QMessageBox::ActionRole);
            auto bothBtn = msgBox.addButton("Both", QMessageBox::ActionRole);
            msgBox.setDefaultButton(importedBtn);
            msgBox.exec();

            if (msgBox.clickedButton() == cancelBtn)
                return AxiomModel::Library::ConflictResolution::CANCEL;
            else if (msgBox.clickedButton() == currentBtn)
                return AxiomModel::Library::ConflictResolution::KEEP_OLD;
            else if (msgBox.clickedButton() == importedBtn)
                return AxiomModel::Library::ConflictResolution::KEEP_NEW;
            else if (msgBox.clickedButton() == bothBtn)
                return AxiomModel::Library::ConflictResolution::KEEP_BOTH;
            else
                unreachable;
        });*/
}

bool MainWindow::checkCloseProject() {
    if (!_project->isDirty()) {
        return true;
    }

    QMessageBox msgBox(QMessageBox::Information, "Unsaved Changes",
                       "You have unsaved changes. Would you like to save before closing your project?");
    auto saveBtn = msgBox.addButton(QMessageBox::Save);
    msgBox.addButton(QMessageBox::Discard);
    auto cancelBtn = msgBox.addButton(QMessageBox::Cancel);
    msgBox.setDefaultButton(saveBtn);
    msgBox.exec();

    if (msgBox.clickedButton() == saveBtn) {
        saveProject();
    }
    return msgBox.clickedButton() != cancelBtn;
}

void MainWindow::updateWindowTitle(const QString &linkedFile, bool isDirty) {
    if (linkedFile.isEmpty()) {
        if (isDirty) {
            setWindowTitle("Axiom - <unsaved> *");
        } else {
            setWindowTitle("Axiom");
        }
    } else {
        if (isDirty) {
            setWindowTitle("Axiom - " % linkedFile % " *");
        } else {
            setWindowTitle("Axiom - " % linkedFile);
        }
    }
}
