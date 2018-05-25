#include "LibraryEntry.h"

#include "PoolOperators.h"
#include "ModelRoot.h"
#include "objects/RootSurface.h"

using namespace AxiomModel;

LibraryEntry::LibraryEntry(QString name, const QUuid &baseUuid, const QUuid &modificationUuid,
                           const QDateTime &modificationDateTime, std::set<QString> tags, std::unique_ptr<AxiomModel::ModelRoot> root)
   : _name(std::move(name)), _baseUuid(baseUuid), _modificationUuid(modificationUuid),
   _modificationDateTime(modificationDateTime), _tags(std::move(tags)), _root(std::move(root)) {
    auto rootSurfaces = findChildren(_root->nodeSurfaces(), QUuid());
    assert(rootSurfaces.size() == 1);
    _rootSurface = dynamic_cast<RootSurface*>(takeAt(rootSurfaces, 0));
    assert(_rootSurface);
}

std::unique_ptr<LibraryEntry> LibraryEntry::create(QString name, const QUuid &baseUuid, const QUuid &modificationUuid,
                                                   const QDateTime &modificationDateTime, std::set<QString> tags,
                                                   std::unique_ptr<AxiomModel::ModelRoot> root) {
    return std::make_unique<LibraryEntry>(std::move(name), baseUuid, modificationUuid, modificationDateTime,
        std::move(tags), std::move(root));
}

std::unique_ptr<LibraryEntry> LibraryEntry::create(QString name, std::set<QString> tags, Project *project) {
    auto newRoot = std::make_unique<ModelRoot>(project);
    newRoot->pool().registerObj(std::make_unique<RootSurface>(QUuid::createUuid(), QPointF(0, 0), 0, newRoot.get()));
    return create(std::move(name), QUuid::createUuid(), QUuid::createUuid(), QDateTime::currentDateTimeUtc(), std::move(tags), std::move(newRoot));
}

std::unique_ptr<LibraryEntry> LibraryEntry::deserialize(QDataStream &stream, Project *project) {
    QString name; stream >> name;
    QUuid baseUuid; stream >> baseUuid;
    QUuid modificationUuid; stream >> modificationUuid;
    QDateTime modificationDateTime; stream >> modificationDateTime;

    quint32 tagSize; stream >> tagSize;
    std::set<QString> tags;
    for (quint32 i = 0; i < tagSize; i++) {
        QString tag; stream >> tag;
        tags.emplace(tag);
    }

    auto root = std::make_unique<ModelRoot>(project, stream);

    return create(std::move(name), baseUuid, modificationUuid, modificationDateTime, std::move(tags), std::move(root));
}

void LibraryEntry::serialize(QDataStream &stream) {
    stream << _name;
    stream << _baseUuid;
    stream << _modificationUuid;
    stream << _modificationDateTime;

    stream << (quint32) _tags.size();
    for (const auto &tag : _tags) {
        stream << tag;
    }

    _root->serialize(stream);
}

void LibraryEntry::setName(const QString &newName) {
    if (newName != _name) {
        _name = newName;
        nameChanged.trigger(newName);
    }
}

void LibraryEntry::addTag(const QString &tag) {
    if (_tags.insert(tag).second) tagAdded.trigger(tag);
}

void LibraryEntry::removeTag(const QString &tag) {
    if (_tags.erase(tag)) tagRemoved.trigger(tag);
}


void LibraryEntry::modified() {
    _modificationUuid = QUuid::createUuid();
    _modificationDateTime = QDateTime::currentDateTimeUtc();
}

void LibraryEntry::remove() {
    _root->destroy();
    removed.trigger();
    cleanup.trigger();
}