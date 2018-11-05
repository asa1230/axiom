#include "MainWindow.h"

#include <QIODevice>
#include <QStandardPaths>
#include <QtCore/QDateTime>
#include <QtCore/QStandardPaths>
#include <QtCore/QStringBuilder>
#include <QtCore/QTimer>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMenuBar>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <chrono>
#include <iostream>

#include "../GlobalActions.h"
#include "../InteractiveImport.h"
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
#include "vendor/dock/DockManager.h"

using namespace AxiomGui;

MainWindow::MainWindow(AxiomBackend::AudioBackend *backend)
    : _backend(backend), _runtime(true, true), libraryLock(globalLibraryLockPath()) {
    setCentralWidget(nullptr);
    setWindowTitle(tr(VER_PRODUCTNAME_STR));
    setWindowIcon(QIcon(":/application.ico"));

    resize(1440, 810);

    setUnifiedTitleAndToolBarOnMac(true);

    dockManager = new ads::CDockManager(this);

    auto startTime = std::chrono::high_resolution_clock::now();
    lockGlobalLibrary();
    // load the library - if the file does not exist, use an empty project
    auto library = loadGlobalLibrary();
    if (!library) {
        library = std::make_unique<AxiomModel::Library>();
    }

    // merge the internal library into the new library, using a strategy to always keep theirs in case of conflict
    auto defaultLibrary = loadDefaultLibrary();
    library->import(defaultLibrary.get(), [](AxiomModel::LibraryEntry *, AxiomModel::LibraryEntry *) {
        return AxiomModel::Library::ConflictResolution::KEEP_OLD;
    });

    _library = std::move(library);
    saveGlobalLibrary();
    unlockGlobalLibrary();
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(endTime - startTime);
    std::cout << "Loading module library took " << duration.count() / 1000000000. << "s" << std::endl;

    _library->changed.connectTo(this, &MainWindow::triggerLibraryChanged);

    saveDebounceTimer.setSingleShot(true);
    saveDebounceTimer.setInterval(500);
    connect(&saveDebounceTimer, &QTimer::timeout, this, &MainWindow::triggerLibraryChangeDebounce);

    globalLibraryWatcher.addPath(globalLibraryFilePath());
    connect(&globalLibraryWatcher, &QFileSystemWatcher::fileChanged, this, &MainWindow::triggerLibraryReload);

    loadDebounceTimer.setSingleShot(true);
    loadDebounceTimer.setInterval(500);
    connect(&loadDebounceTimer, &QTimer::timeout, this, &MainWindow::triggerLibraryReloadDebounce);

    _modulePanel = std::make_unique<ModuleBrowserPanel>(this, _library.get(), this);
    dockManager->addDockWidget(ads::BottomDockWidgetArea, _modulePanel.get());

    // build menus
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
    _viewMenu->addAction(_modulePanel->toggleViewAction());

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

MainWindow::~MainWindow() {
    unlockGlobalLibrary();
}

NodeSurfacePanel *MainWindow::showSurface(NodeSurfacePanel *fromPanel, AxiomModel::NodeSurface *surface, bool split,
                                          bool permanent) {
    auto openPanel = _openPanels.find(surface);
    if (openPanel != _openPanels.end()) {
        openPanel->second->raise();
        return openPanel->second.get();
    }

    auto newDock = std::make_unique<NodeSurfacePanel>(this, surface);
    auto newDockPtr = newDock.get();

    if (!fromPanel) {
        auto rootSurface = _project->rootSurface();
        if (surface == rootSurface) {
            dockManager->addDockWidget(ads::TopDockWidgetArea, newDockPtr);
        } else {
            fromPanel = _openPanels[_project->rootSurface()].get();
        }
    }

    if (fromPanel) {
        auto area = split ? ads::RightDockWidgetArea : ads::CenterDockWidgetArea;
        dockManager->addDockWidget(area, newDockPtr, fromPanel->dockAreaWidget());
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
}

bool MainWindow::isInputFieldFocused() const {
    return dynamic_cast<QLineEdit *>(focusWidget()) || dynamic_cast<QTextEdit *>(focusWidget());
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    QMainWindow::keyPressEvent(event);
    if (event->isAccepted() || isInputFieldFocused() || event->isAutoRepeat()) return;

    auto midiCode = AxiomUtil::noteKeyToMidi(event->key());
    if (midiCode) {
        AxiomBackend::MidiEvent midiEvent;
        midiEvent.event = AxiomBackend::MidiEventType::NOTE_ON;
        midiEvent.note = *midiCode;
        midiEvent.param = 255;
        _backend->previewEvent(midiEvent);
    }
}

void MainWindow::keyReleaseEvent(QKeyEvent *event) {
    QMainWindow::keyReleaseEvent(event);
    if (event->isAccepted() || isInputFieldFocused() || event->isAutoRepeat()) return;

    auto midiCode = AxiomUtil::noteKeyToMidi(event->key());
    if (midiCode) {
        AxiomBackend::MidiEvent midiEvent;
        midiEvent.event = AxiomBackend::MidiEventType::NOTE_OFF;
        midiEvent.note = *midiCode;
        _backend->previewEvent(midiEvent);
    }
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (checkCloseProject()) {
        // save the global library
        lockGlobalLibrary();
        saveGlobalLibrary();
        unlockGlobalLibrary();

        event->accept();
    } else {
        event->ignore();
    }
}

void MainWindow::setProject(std::unique_ptr<AxiomModel::Project> project) {
    // cleanup old project state
    if (_project) {
        _openPanels[_project->rootSurface()]->toggleView(false);
        removeSurface(_project->rootSurface());
    }
    _project = std::move(project);

    _openPanels.clear();
    if (_historyPanel) {
        _historyPanel->close();
    }
    if (_modulePanel) {
        _modulePanel->close();
    }

    // attach the backend and our runtime
    _project->attachBackend(_backend);
    _project->mainRoot().attachRuntime(runtime());

    // find root surface and show it
    auto defaultSurface =
        AxiomCommon::getFirst(AxiomModel::findChildrenWatch(_project->mainRoot().nodeSurfaces(), QUuid()));
    assert(defaultSurface->value());
    auto surfacePanel = showSurface(nullptr, *defaultSurface->value(), false, true);

    _modulePanel->show();

    _historyPanel = std::make_unique<HistoryPanel>(&_project->mainRoot().history(), this);
    dockManager->addDockWidget(ads::RightDockWidgetArea, _historyPanel.get());
    _historyPanel->toggleView(false);

    _viewMenu->addAction(surfacePanel->toggleViewAction());
    _viewMenu->addAction(_historyPanel->toggleViewAction());

    updateWindowTitle(_project->linkedFile(), _project->isDirty());
    _project->linkedFileChanged.connectTo(
        [this](const QString &newName) { updateWindowTitle(newName, _project->isDirty()); });
    _project->isDirtyChanged.connectTo([this](bool isDirty) { updateWindowTitle(_project->linkedFile(), isDirty); });
}

QString MainWindow::globalLibraryLockPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("library.lock");
}

QString MainWindow::globalLibraryFilePath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath("library.axl");
}

void MainWindow::lockGlobalLibrary() {
    if (isLibraryLocked) return;
    isLibraryLocked = true;
    libraryLock.lock();
}

void MainWindow::unlockGlobalLibrary() {
    if (!isLibraryLocked) return;
    libraryLock.unlock();
    isLibraryLocked = false;
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

std::unique_ptr<AxiomModel::Library> MainWindow::loadGlobalLibrary() {
    QFile libraryFile(globalLibraryFilePath());
    if (!libraryFile.open(QIODevice::ReadOnly)) {
        return nullptr;
    }

    QDataStream stream(&libraryFile);
    uint32_t readVersion = 0;
    if (!AxiomModel::ProjectSerializer::readHeader(stream, AxiomModel::ProjectSerializer::librarySchemaMagic,
                                                   &readVersion)) {
        // we can't load the file - rename it as a backup, and return empty
        auto newName = "library (" + QDateTime::currentDateTime().toString("yyyy/MM/dd hh:mm:ss.z") + ").axl";

        std::cout << "Failed to load global project (";
        if (readVersion) {
            std::cout << "schema version is " << readVersion << ", expected between "
                      << AxiomModel::ProjectSerializer::minSchemaVersion << " and "
                      << AxiomModel::ProjectSerializer::schemaVersion;
        } else {
            std::cout << "bad magic header";
        }
        std::cout << "), backing it up as '" << newName.toStdString() << "' and resetting library" << std::endl;

        libraryFile.rename(newName);
        return nullptr;
    }

    auto library = AxiomModel::LibrarySerializer::deserialize(stream, readVersion);
    libraryFile.close();
    return library;
}

std::unique_ptr<AxiomModel::Library> MainWindow::loadDefaultLibrary() {
    QFile defaultFile(":/default.axl");
    auto couldOpenFile = defaultFile.open(QIODevice::ReadOnly);
    assert(couldOpenFile);
    QDataStream stream(&defaultFile);
    uint32_t readVersion;
    auto couldReadHeader = AxiomModel::ProjectSerializer::readHeader(
        stream, AxiomModel::ProjectSerializer::librarySchemaMagic, &readVersion);
    assert(couldReadHeader);
    auto library = AxiomModel::LibrarySerializer::deserialize(stream, readVersion);
    defaultFile.close();
    return library;
}

void MainWindow::saveGlobalLibrary() {
    QFile file(globalLibraryFilePath());
    if (!file.open(QIODevice::WriteOnly)) {
        return;
    }

    QDataStream stream(&file);
    AxiomModel::ProjectSerializer::writeHeader(stream, AxiomModel::ProjectSerializer::librarySchemaMagic);
    AxiomModel::LibrarySerializer::serialize(_library.get(), stream);
    file.close();
}

void MainWindow::triggerLibraryChanged() {
    // ignore any changes if they were caused by the library being loaded
    if (isLoadingLibrary) {
        return;
    }

    lockGlobalLibrary();
    saveDebounceTimer.start();
}

void MainWindow::triggerLibraryChangeDebounce() {
    didJustSaveLibrary = true;

    std::cout << "Saving module library after internal change" << std::endl;
    saveGlobalLibrary();
    unlockGlobalLibrary();
}

void MainWindow::triggerLibraryReload() {
    loadDebounceTimer.start();
}

void MainWindow::triggerLibraryReloadDebounce() {
    // ignore any changes if they were caused by the library being saved
    if (didJustSaveLibrary) {
        didJustSaveLibrary = false;
        return;
    }

    isLoadingLibrary = true;
    std::cout << "Reloading module library after filesystem change" << std::endl;

    lockGlobalLibrary();
    auto library = loadGlobalLibrary();
    if (library) {
        _library->import(library.get(), [](AxiomModel::LibraryEntry *, AxiomModel::LibraryEntry *) {
            return AxiomModel::Library::ConflictResolution::KEEP_NEW;
        });
    }
    unlockGlobalLibrary();

    isLoadingLibrary = false;
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

void MainWindow::openProjectFrom(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox(QMessageBox::Critical, "Failed to open project", "The file you selected couldn't be opened.",
                    QMessageBox::Ok)
            .exec();
        return;
    }

    QDataStream stream(&file);
    uint32_t readVersion = 0;
    auto newProject = AxiomModel::ProjectSerializer::deserialize(
        stream, &readVersion,
        [this](AxiomModel::Library *importLibrary) { doInteractiveLibraryImport(library(), importLibrary); },
        [path](QDataStream &, uint32_t) { return path; });
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

void MainWindow::openProject() {
    if (!checkCloseProject()) return;

    auto selectedFile = QFileDialog::getOpenFileName(this, "Open Project", QString(),
                                                     tr("Axiom Project Files (*.axp);;All Files (*.*)"));
    if (selectedFile.isNull()) return;
    openProjectFrom(selectedFile);
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
    AxiomModel::LibrarySerializer::serialize(_library.get(), stream);
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

    auto mergeLibrary = AxiomModel::LibrarySerializer::deserialize(stream, readVersion);
    file.close();
    doInteractiveLibraryImport(_library.get(), mergeLibrary.get());
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
