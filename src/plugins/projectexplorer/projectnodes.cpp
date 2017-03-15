/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "projectnodes.h"

#include "projectexplorerconstants.h"
#include "projecttree.h"

#include <coreplugin/fileiconprovider.h>
#include <coreplugin/icore.h>
#include <coreplugin/iversioncontrol.h>
#include <coreplugin/vcsmanager.h>

#include <utils/algorithm.h>
#include <utils/fileutils.h>
#include <utils/hostosinfo.h>
#include <utils/mimetypes/mimedatabase.h>
#include <utils/mimetypes/mimetype.h>
#include <utils/qtcassert.h>

#include <QFileInfo>
#include <QDir>
#include <QIcon>
#include <QStyle>

#include <memory>

namespace ProjectExplorer {

static FolderNode *folderNode(const FolderNode *folder, const Utils::FileName &directory)
{
    return static_cast<FolderNode *>(Utils::findOrDefault(folder->nodes(),
                                                          [&directory](const Node *n) {
        const FolderNode *fn = n->asFolderNode();
        return fn && fn->filePath() == directory;
    }));
}

static FolderNode *recursiveFindOrCreateFolderNode(FolderNode *folder,
                                                   const Utils::FileName &directory,
                                                   const Utils::FileName &overrideBaseDir,
                                                   const FolderNode::FolderNodeFactory &factory)
{
    Utils::FileName path = overrideBaseDir.isEmpty() ? folder->filePath() : overrideBaseDir;

    Utils::FileName directoryWithoutPrefix;
    bool isRelative = false;

    if (path.isEmpty() || path.toFileInfo().isRoot()) {
        directoryWithoutPrefix = directory;
        isRelative = false;
    } else {
        if (directory.isChildOf(path) || directory == path) {
            isRelative = true;
            directoryWithoutPrefix = directory.relativeChildPath(path);
        } else {
            isRelative = false;
            directoryWithoutPrefix = directory;
        }
    }
    QStringList parts = directoryWithoutPrefix.toString().split('/', QString::SkipEmptyParts);
    if (!Utils::HostOsInfo::isWindowsHost() && !isRelative && parts.count() > 0)
        parts[0].prepend('/');

    ProjectExplorer::FolderNode *parent = folder;
    foreach (const QString &part, parts) {
        path.appendPath(part);
        // Find folder in subFolders
        FolderNode *next = folderNode(parent, path);
        if (!next) {
            // No FolderNode yet, so create it
            auto tmp = factory(path);
            tmp->setDisplayName(part);
            parent->addNode(tmp);
            next = tmp;
        }
        parent = next;
    }
    return parent;
}

/*!
  \class ProjectExplorer::Node

  \brief The Node class is the base class of all nodes in the node hierarchy.

  The nodes are arranged in a tree where leaves are FileNodes and non-leaves are FolderNodes
  A Project is a special Folder that manages the files and normal folders underneath it.

  The Watcher emits signals for structural changes in the hierarchy.
  A Visitor can be used to traverse all Projects and other Folders.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
  \sa ProjectExplorer::NodesWatcher
*/

Node::Node(NodeType nodeType, const Utils::FileName &filePath, int line) :
    m_filePath(filePath), m_line(line), m_nodeType(nodeType)
{ }

void Node::setPriority(int p)
{
    m_priority = p;
}

void Node::setAbsoluteFilePathAndLine(const Utils::FileName &path, int line)
{
    if (m_filePath == path && m_line == line)
        return;

    m_filePath = path;
    m_line = line;
}

Node::~Node() = default;

NodeType Node::nodeType() const
{
    return m_nodeType;
}

int Node::priority() const
{
    return m_priority;
}

/*!
  The project that owns and manages the node. It is the first project in the list
  of ancestors.
  */
ProjectNode *Node::parentProjectNode() const
{
    if (!m_parentFolderNode)
        return nullptr;
    auto pn = m_parentFolderNode->asProjectNode();
    if (pn)
        return pn;
    return m_parentFolderNode->parentProjectNode();
}

/*!
  The parent in the node hierarchy.
  */
FolderNode *Node::parentFolderNode() const
{
    return m_parentFolderNode;
}

ProjectNode *Node::managingProject()
{
    if (!m_parentFolderNode)
        return nullptr;
    ProjectNode *pn = parentProjectNode();
    return pn ? pn : asProjectNode(); // projects manage themselves...
}

const ProjectNode *Node::managingProject() const
{
    return const_cast<Node *>(this)->managingProject();
}

/*!
  The path of the file or folder in the filesystem the node represents.
  */
const Utils::FileName &Node::filePath() const
{
    return m_filePath;
}

int Node::line() const
{
    return m_line;
}

QString Node::displayName() const
{
    return filePath().fileName();
}

QString Node::tooltip() const
{
    return filePath().toUserOutput();
}

bool Node::isEnabled() const
{
    if (!m_isEnabled)
        return false;
    FolderNode *parent = parentFolderNode();
    return parent ? parent->isEnabled() : true;
}

QList<ProjectAction> Node::supportedActions(Node *node) const
{
    QList<ProjectAction> list = parentFolderNode()->supportedActions(node);
    list.append(InheritedFromParent);
    return list;
}

void Node::setEnabled(bool enabled)
{
    if (m_isEnabled == enabled)
        return;
    m_isEnabled = enabled;
}

bool Node::sortByPath(const Node *a, const Node *b)
{
    return a->filePath() < b->filePath();
}

void Node::setParentFolderNode(FolderNode *parentFolder)
{
    m_parentFolderNode = parentFolder;
}

FileType Node::fileTypeForMimeType(const Utils::MimeType &mt)
{
    FileType type = FileType::Source;
    if (mt.isValid()) {
        const QString mtName = mt.name();
        if (mtName == Constants::C_HEADER_MIMETYPE
                || mtName == Constants::CPP_HEADER_MIMETYPE)
            type = FileType::Header;
        else if (mtName == Constants::FORM_MIMETYPE)
            type = FileType::Form;
        else if (mtName == Constants::RESOURCE_MIMETYPE)
            type = FileType::Resource;
        else if (mtName == Constants::SCXML_MIMETYPE)
            type = FileType::StateChart;
        else if (mtName == Constants::QML_MIMETYPE)
            type = FileType::QML;
    } else {
        type = FileType::Unknown;
    }
    return type;
}

FileType Node::fileTypeForFileName(const Utils::FileName &file)
{
    return fileTypeForMimeType(Utils::mimeTypeForFile(file.toString()));
}

/*!
  \class ProjectExplorer::FileNode

  \brief The FileNode class is an in-memory presentation of a file.

  All file nodes are leaf nodes.

  \sa ProjectExplorer::FolderNode, ProjectExplorer::ProjectNode
*/

FileNode::FileNode(const Utils::FileName &filePath,
                   const FileType fileType,
                   bool generated, int line) : Node(NodeType::File, filePath, line),
    m_fileType(fileType),
    m_generated(generated)
{
    if (fileType == FileType::Project)
        setPriority(DefaultProjectFilePriority);
    else
        setPriority(DefaultFilePriority);
}

FileType FileNode::fileType() const
{
    return m_fileType;
}

/*!
  Returns \c true if the file is automatically generated by a compile step.
  */
bool FileNode::isGenerated() const
{
    return m_generated;
}

static QList<FileNode *> scanForFilesRecursively(const Utils::FileName &directory,
                                                 const std::function<FileNode *(const Utils::FileName &)> factory,
                                                 QSet<QString> &visited, QFutureInterface<QList<FileNode*>> *future,
                                                 double progressStart, double progressRange)
{
    QList<FileNode *> result;

    const QDir baseDir = QDir(directory.toString());

    // Do not follow directory loops:
    const int visitedCount = visited.count();
    visited.insert(baseDir.canonicalPath());
    if (visitedCount == visited.count())
        return result;

    const Core::IVersionControl *vcsControl
            = Core::VcsManager::findVersionControlForDirectory(baseDir.absolutePath(), nullptr);
    const QList<QFileInfo> entries = baseDir.entryInfoList(QStringList(), QDir::AllEntries|QDir::NoDotAndDotDot);
    double progress = 0;
    const double progressIncrement = progressRange / static_cast<double>(entries.count());
    int lastIntProgress = 0;
    for (const QFileInfo &entry : entries) {
        if (future && future->isCanceled())
            return result;

        const Utils::FileName entryName = Utils::FileName::fromString(entry.absoluteFilePath());
        if (!vcsControl || !vcsControl->isVcsFileOrDirectory(entryName)) {
            if (entry.isDir()) {
                result.append(scanForFilesRecursively(entryName, factory, visited, future, progress, progressIncrement));
            } else {
                FileNode *node = factory(entryName);
                if (node)
                    result.append(node);
            }
        }
        if (future) {
            progress += progressIncrement;
            const int intProgress = std::min(static_cast<int>(progressStart + progress), future->progressMaximum());
            if (lastIntProgress < intProgress) {
                future->setProgressValue(intProgress);
                lastIntProgress = intProgress;
            }
        }
    }
    if (future)
        future->setProgressValue(std::min(static_cast<int>(progressStart + progressRange), future->progressMaximum()));
    return result;
}

QList<FileNode *> FileNode::scanForFiles(const Utils::FileName &directory,
                                               const std::function<FileNode *(const Utils::FileName &)> factory,
                                               QFutureInterface<QList<FileNode *>> *future)
{
    QSet<QString> visited;
    if (future)
        future->setProgressRange(0, 1000000);
    return scanForFilesRecursively(directory, factory, visited, future, 0.0, 1000000.0);
}

/*!
  \class ProjectExplorer::FolderNode

  In-memory presentation of a folder. Note that the node itself + all children (files and folders) are "managed" by the owning project.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
FolderNode::FolderNode(const Utils::FileName &folderPath, NodeType nodeType, const QString &displayName) :
    Node(nodeType, folderPath, -1),
    m_displayName(displayName)
{
    setPriority(DefaultFolderPriority);
    if (m_displayName.isEmpty())
        m_displayName = folderPath.toUserOutput();
}

FolderNode::~FolderNode()
{
    qDeleteAll(m_nodes);
}

/*!
    Contains the display name that should be used in a view.
    \sa setFolderName()
 */

QString FolderNode::displayName() const
{
    return m_displayName;
}

/*!
  Contains the icon that should be used in a view. Default is the directory icon
 (QStyle::S_PDirIcon).
  s\a setIcon()
 */
QIcon FolderNode::icon() const
{
    // Instantiating the Icon provider is expensive.
    if (m_icon.isNull())
        m_icon = Core::FileIconProvider::icon(QFileIconProvider::Folder);
    return m_icon;
}

Node *FolderNode::findNode(const std::function<bool(Node *)> &filter)
{
    if (filter(this))
        return this;

    for (Node *n : m_nodes) {
        if (n->asFileNode() && filter(n)) {
            return n;
        } else if (FolderNode *folder = n->asFolderNode()) {
            Node *result = folder->findNode(filter);
            if (result)
                return result;
        }
    }
    return nullptr;
}

QList<Node *> FolderNode::findNodes(const std::function<bool(Node *)> &filter)
{
    QList<Node *> result;
    if (filter(this))
        result.append(this);
    for (Node *n : m_nodes) {
        if (n->asFileNode() && filter(n))
            result.append(n);
        else if (FolderNode *folder = n->asFolderNode())
            result.append(folder->findNode(filter));
    }
    return result;
}

void FolderNode::forEachNode(const std::function<void(FileNode *)> &fileTask,
                             const std::function<void(FolderNode *)> &folderTask,
                             const std::function<bool(const FolderNode *)> &folderFilterTask) const
{
    if (folderFilterTask) {
        if (!folderFilterTask(this))
            return;
    }
    if (fileTask) {
        for (Node *n : m_nodes) {
            if (FileNode *fn = n->asFileNode())
                fileTask(fn);
        }
    }
    for (Node *n : m_nodes) {
        if (FolderNode *fn = n->asFolderNode()) {
            if (folderTask)
                folderTask(fn);
            fn->forEachNode(fileTask, folderTask, folderFilterTask);
        }
    }
}

void FolderNode::forEachGenericNode(const std::function<void(Node *)> &genericTask) const
{
    for (Node *n : m_nodes) {
        genericTask(n);
        if (FolderNode *fn = n->asFolderNode())
            fn->forEachNode(genericTask);
    }
}

QList<FileNode*> FolderNode::fileNodes() const
{
    QList<FileNode *> result;
    for (Node *n : m_nodes) {
        if (FileNode *fn = n->asFileNode())
            result.append(fn);
    }
    return result;
}

FileNode *FolderNode::fileNode(const Utils::FileName &file) const
{
    return static_cast<FileNode *>(Utils::findOrDefault(m_nodes, [&file](const Node *n) {
        const FileNode *fn = n->asFileNode();
        return fn && fn->filePath() == file;
    }));
}

QList<FolderNode*> FolderNode::folderNodes() const
{
    QList<FolderNode *> result;
    for (Node *n : m_nodes) {
        if (FolderNode *fn = n->asFolderNode())
            result.append(fn);
    }
    return result;
}

void FolderNode::addNestedNode(FileNode *fileNode, const Utils::FileName &overrideBaseDir,
                               const FolderNodeFactory &factory)
{
    // Get relative path to rootNode
    QString parentDir = fileNode->filePath().toFileInfo().absolutePath();
    FolderNode *folder = recursiveFindOrCreateFolderNode(this, Utils::FileName::fromString(parentDir),
                                                         overrideBaseDir, factory);
    folder->addNode(fileNode);

}

void FolderNode::addNestedNodes(const QList<FileNode *> &files, const Utils::FileName &overrideBaseDir,
                                const FolderNodeFactory &factory)
{
    for (FileNode *fn : files)
        addNestedNode(fn, overrideBaseDir, factory);
}

// "Compress" a tree of foldernodes such that foldernodes with exactly one foldernode as a child
// are merged into one. This e.g. turns a sequence of FolderNodes "foo" "bar" "baz" into one
// FolderNode named "foo/bar/baz", saving a lot of clicks in the Project View to get to the actual
// files.
void FolderNode::compress()
{
    QList<Node *> children = nodes();
    if (auto subFolder = children.count() == 1 ? children.at(0)->asFolderNode() : nullptr) {
        // Only one subfolder: Compress!
        setDisplayName(QDir::toNativeSeparators(displayName() + "/" + subFolder->displayName()));
        for (Node *n : subFolder->nodes()) {
            subFolder->removeNode(n);
            n->setParentFolderNode(nullptr);
            addNode(n);
        }
        setAbsoluteFilePathAndLine(subFolder->filePath(), -1);

        removeNode(subFolder);
        delete subFolder;

        compress();
    } else {
        for (FolderNode *fn : folderNodes())
            fn->compress();
    }
}

bool FolderNode::isAncesterOf(Node *n)
{
    if (n == this)
        return true;
    FolderNode *p = n->parentFolderNode();
    while (p && p != this)
        p = p->parentFolderNode();
    return p == this;
}

bool FolderNode::replaceSubtree(Node *oldNode, Node *newNode)
{
    std::unique_ptr<Node> nn(newNode);
    if (!oldNode) {
        addNode(nn.release()); // Happens e.g. when a project is registered
    } else {
        auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                               [oldNode](const Node *n) { return oldNode == n; });
        QTC_ASSERT(it != m_nodes.end(), return false);
        if (nn) {
            nn->setParentFolderNode(this);
            *it = nn.release();
        } else {
            removeNode(oldNode); // Happens e.g. when project is shutting down
        }
        delete oldNode;
    }
    ProjectTree::emitSubtreeChanged(this);
    return true;
}

void FolderNode::setDisplayName(const QString &name)
{
    if (m_displayName == name)
        return;
    m_displayName = name;
}

void FolderNode::setIcon(const QIcon &icon)
{
    m_icon = icon;
}

QString FolderNode::addFileFilter() const
{
    return parentFolderNode()->addFileFilter();
}

bool FolderNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->addFiles(filePaths, notAdded);
    return false;
}

bool FolderNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->removeFiles(filePaths, notRemoved);
    return false;
}

bool FolderNode::deleteFiles(const QStringList &filePaths)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->deleteFiles(filePaths);
    return false;
}

bool FolderNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->canRenameFile(filePath, newFilePath);
    return false;
}

bool FolderNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    ProjectNode *pn = managingProject();
    if (pn)
        return pn->renameFile(filePath, newFilePath);
    return false;
}

FolderNode::AddNewInformation FolderNode::addNewInformation(const QStringList &files, Node *context) const
{
    Q_UNUSED(files);
    return AddNewInformation(displayName(), context == this ? 120 : 100);
}

/*!
  Adds a node specified by \a node to the internal list of nodes.
*/

void FolderNode::addNode(Node *node)
{
    QTC_ASSERT(node, return);
    QTC_ASSERT(!node->parentFolderNode(), qDebug("File node has already a parent folder"));
    node->setParentFolderNode(this);
    m_nodes.append(node);
}

/*!
  Removes a node specified by \a node from the internal list of nodes.
  The node object itself is not deleted.
*/

void FolderNode::removeNode(Node *node)
{
    m_nodes.removeOne(node);
}

bool FolderNode::showInSimpleTree() const
{
    return false;
}

/*!
  \class ProjectExplorer::VirtualFolderNode

  In-memory presentation of a virtual folder.
  Note that the node itself + all children (files and folders) are "managed" by the owning project.
  A virtual folder does not correspond to a actual folder on the file system. See for example the
  sources, headers and forms folder the QmakeProjectManager creates
  VirtualFolderNodes are always sorted before FolderNodes and are sorted according to their priority.

  \sa ProjectExplorer::FileNode, ProjectExplorer::ProjectNode
*/
VirtualFolderNode::VirtualFolderNode(const Utils::FileName &folderPath, int priority) :
    FolderNode(folderPath, NodeType::VirtualFolder, QString())
{
    setPriority(priority);
}

QString VirtualFolderNode::addFileFilter() const
{
    if (!m_addFileFilter.isNull())
        return m_addFileFilter;
    return FolderNode::addFileFilter();
}

/*!
  \class ProjectExplorer::ProjectNode

  \brief The ProjectNode class is an in-memory presentation of a Project.

  A concrete subclass must implement the persistent data.

  \sa ProjectExplorer::FileNode, ProjectExplorer::FolderNode
*/

/*!
  Creates an uninitialized project node object.
  */
ProjectNode::ProjectNode(const Utils::FileName &projectFilePath) :
    FolderNode(projectFilePath, NodeType::Project)
{
    setPriority(DefaultProjectPriority);
    setDisplayName(projectFilePath.fileName());
}

QString ProjectNode::vcsTopic() const
{
    const QFileInfo fi = filePath().toFileInfo();
    const QString dir = fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();

    if (Core::IVersionControl *const vc =
            Core::VcsManager::findVersionControlForDirectory(dir))
        return vc->vcsTopic(dir);

    return QString();
}

bool ProjectNode::canAddSubProject(const QString &proFilePath) const
{
    Q_UNUSED(proFilePath)
    return false;
}

bool ProjectNode::addSubProject(const QString &proFilePath)
{
    Q_UNUSED(proFilePath)
    return false;
}

bool ProjectNode::removeSubProject(const QString &proFilePath)
{
    Q_UNUSED(proFilePath)
    return false;
}

bool ProjectNode::addFiles(const QStringList &filePaths, QStringList *notAdded)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notAdded)
    return false;
}

bool ProjectNode::removeFiles(const QStringList &filePaths, QStringList *notRemoved)
{
    Q_UNUSED(filePaths)
    Q_UNUSED(notRemoved)
    return false;
}

bool ProjectNode::deleteFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths)
    return false;
}

bool ProjectNode::canRenameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath);
    Q_UNUSED(newFilePath);
    return true;
}

bool ProjectNode::renameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath)
    Q_UNUSED(newFilePath)
    return false;
}

bool ProjectNode::deploysFolder(const QString &folder) const
{
    Q_UNUSED(folder);
    return false;
}

/*!
  \function bool ProjectNode::runConfigurations() const

  Returns a list of \c RunConfiguration suitable for this node.
  */
QList<RunConfiguration *> ProjectNode::runConfigurations() const
{
    return QList<RunConfiguration *>();
}

ProjectNode *ProjectNode::projectNode(const Utils::FileName &file) const
{
    for (Node *node : m_nodes) {
        if (ProjectNode *pnode = node->asProjectNode())
            if (pnode->filePath() == file)
                return pnode;
    }
    return nullptr;
}

bool FolderNode::isEmpty() const
{
    return m_nodes.isEmpty();
}

/*!
  \class ProjectExplorer::SessionNode
*/

SessionNode::SessionNode() :
    FolderNode(Utils::FileName::fromString("session"), NodeType::Session)
{ }

QList<ProjectAction> SessionNode::supportedActions(Node *node) const
{
    Q_UNUSED(node)
    return QList<ProjectAction>();
}

bool SessionNode::showInSimpleTree() const
{
    return true;
}

QString SessionNode::addFileFilter() const
{
    return QString::fromLatin1("*.c; *.cc; *.cpp; *.cp; *.cxx; *.c++; *.h; *.hh; *.hpp; *.hxx;");
}

} // namespace ProjectExplorer
