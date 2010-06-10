/***************************************************************************
 *   Copyright 2009 Sandro Andrade <sandroandrade@kde.org>                 *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU Library General Public License as       *
 *   published by the Free Software Foundation; either version 2 of the    *
 *   License, or (at your option) any later version.                       *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this program; if not, write to the                 *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.         *
 ***************************************************************************/

#include "duchaincontrolflow.h"

#include <limits>

#include <QMutexLocker>

#include <KTextEditor/View>
#include <KTextEditor/Document>
#include <KTextEditor/Cursor>
#include <KLocale>
#include <ThreadWeaver/Weaver>

#include <interfaces/icore.h>
#include <interfaces/iproject.h>
#include <interfaces/iuicontroller.h>
#include <interfaces/iprojectcontroller.h>
#include <interfaces/ilanguagecontroller.h>
#include <interfaces/idocumentcontroller.h>

#include <language/duchain/use.h>
#include <language/duchain/duchain.h>
#include <language/duchain/ducontext.h>
#include <language/duchain/declaration.h>
#include <language/duchain/duchainlock.h>
#include <language/duchain/duchainutils.h>
#include <language/duchain/indexedstring.h>
#include <language/duchain/functiondefinition.h>
#include <language/duchain/types/functiontype.h>
#include <language/util/navigationtooltip.h>
#include <language/backgroundparser/backgroundparser.h>

#include <project/projectmodel.h>
#include <project/interfaces/ibuildsystemmanager.h>

#include "controlflowgraphusescollector.h"
#include "controlflowgraphnavigationwidget.h"

Q_DECLARE_METATYPE(KDevelop::Use)

using namespace KDevelop;

DUChainControlFlow::DUChainControlFlow()
: m_previousUppermostExecutableContext(IndexedDUContext()),
  m_currentProject(0),
  m_currentLevel(1),
  m_maxLevel(2),
  m_locked(false),
  m_drawIncomingArcs(true),
  m_useFolderName(true),
  m_useShortNames(true),
  m_controlFlowMode(ControlFlowClass),
  m_clusteringModes(ClusteringNamespace),
  m_graphThreadRunning(false),
  m_collector(0)
{
    qRegisterMetaType<Use>("Use");
    connect(this, SIGNAL(updateToolTip(const QString &, const QPoint&, QWidget *)),
            SLOT(slotUpdateToolTip(const QString &, const QPoint&, QWidget *)));
    connect(this, SIGNAL(done(ThreadWeaver::Job*)), SLOT(slotThreadDone(ThreadWeaver::Job*)));
    ICore::self()->uiController()->registerStatus(this);
}

DUChainControlFlow::~DUChainControlFlow()
{
    delete m_collector;
}

QString DUChainControlFlow::statusName() const
{
    return i18n("Control Flow Graph");
}

void DUChainControlFlow::setControlFlowMode(ControlFlowMode controlFlowMode)
{
    m_controlFlowMode = controlFlowMode;
}

void DUChainControlFlow::setClusteringModes(ClusteringModes clusteringModes)
{
    m_clusteringModes = clusteringModes;
}

DUChainControlFlow::ClusteringModes DUChainControlFlow::clusteringModes() const
{
    return m_clusteringModes;
}

void DUChainControlFlow::generateControlFlowForDeclaration(IndexedDeclaration idefinition, IndexedTopDUContext itopContext, IndexedDUContext iuppermostExecutableContext)
{
    DUChainReadLocker lock(DUChain::lock());

    Declaration *definition = idefinition.data();
    if (!definition)
        return;
    
    TopDUContext *topContext = itopContext.data();
    if (!topContext)
        return;

    DUContext *uppermostExecutableContext = iuppermostExecutableContext.data();
    if (!uppermostExecutableContext)
        return;

    emit showProgress(this, 0, 0, 0);
    emit showMessage(this, i18n("Generating graph for function %1", definition->identifier().toString()));

    // Convert to a declaration in accordance with control flow mode (function, class or namespace)
    Declaration *nodeDefinition = declarationFromControlFlowMode(definition);

    QStringList containers;
    prepareContainers(containers, definition);

    QString shortName = shortNameFromContainers(containers, prependFolderNames(nodeDefinition));

    if (m_maxLevel != 1 && !m_visitedFunctions.contains(idefinition))
    {
        emit foundRootNode(containers, (m_controlFlowMode == ControlFlowNamespace &&
                                        nodeDefinition->internalContext()->type() != DUContext::Namespace) ? 
                                                                          globalNamespaceOrFolderNames(nodeDefinition):
                                                                          shortName);
        ++m_currentLevel;
        m_visitedFunctions.insert(idefinition);
        m_identifierDeclarationMap[shortName] = IndexedDeclaration(nodeDefinition);
        useDeclarationsFromDefinition(definition, topContext, uppermostExecutableContext);
    }

    if (m_drawIncomingArcs)
    {
        Declaration *declaration = nodeDefinition;
        if (declaration->isDefinition())
            declaration = DUChainUtils::declarationForDefinition(declaration, topContext);

        delete m_collector;
        m_collector = new ControlFlowGraphUsesCollector(declaration);
        m_collector->setProcessDeclarations(true);
        connect(m_collector, SIGNAL(processFunctionCall(Declaration *, Declaration *, const Use &)), SLOT(processFunctionCall(Declaration *, Declaration *, const Use &)));
        m_collector->startCollecting();
    }

    emit hideProgress(this);
    emit clearMessage(this);
    emit graphDone();
    m_currentLevel = 1;
}

bool DUChainControlFlow::isLocked()
{
    return m_locked;
}

void DUChainControlFlow::run()
{
    DUChainReadLocker lock(DUChain::lock());

    // Navigate to uppermost executable context
    DUContext *uppermostExecutableContext = m_currentContext.data();
    if (!uppermostExecutableContext)
        return;
    
    while (uppermostExecutableContext->parentContext()->type() == DUContext::Other)
        uppermostExecutableContext = uppermostExecutableContext->parentContext();

    // If cursor is in the same function definition
    if (IndexedDUContext(uppermostExecutableContext) == m_previousUppermostExecutableContext)
        return;

    m_previousUppermostExecutableContext = IndexedDUContext(uppermostExecutableContext);

    // Get the definition
    Declaration* definition = 0;
    if (!uppermostExecutableContext || !uppermostExecutableContext->owner())
        return;
    else
        definition = uppermostExecutableContext->owner();

    if (!definition) return;

    newGraph();
    emit prepareNewGraph();

    m_definition = IndexedDeclaration(definition);
    m_uppermostExecutableContext = IndexedDUContext(uppermostExecutableContext);

    generateControlFlowForDeclaration(m_definition, m_topContext, m_uppermostExecutableContext);
}

void DUChainControlFlow::cursorPositionChanged(KTextEditor::View *view, const KTextEditor::Cursor &cursor)
{
    if (!m_graphThreadRunning)
    {
        if (m_locked) return;
        if (!view->document()) return;

        DUChainReadLocker lock(DUChain::lock());

        TopDUContext *topContext = DUChainUtils::standardContextForUrl(view->document()->url());
        if (!topContext) return;

        DUContext *context = topContext->findContext(KDevelop::SimpleCursor(cursor));

        // If cursor is in a method arguments context change it to internal context
        if (context && context->type() == DUContext::Function && context->importers().size() == 1)
            context = context->importers()[0];

        Declaration *declarationUnderCursor = DUChainUtils::itemUnderCursor(view->document()->url(), KDevelop::SimpleCursor(cursor));
        if (declarationUnderCursor && (!context || context->type() != DUContext::Other) && declarationUnderCursor->internalContext())
            context = declarationUnderCursor->internalContext();

        if (!context || context->type() != DUContext::Other)
        {
            // If there is a previous graph
            if (!(m_previousUppermostExecutableContext == IndexedDUContext()))
            {
                newGraph();
                m_previousUppermostExecutableContext = IndexedDUContext();
            }
            return;
        }

        m_currentContext = IndexedDUContext(context);
        m_currentView = view;
        m_topContext = IndexedTopDUContext(topContext);

        m_currentProject = ICore::self()->projectController()->findProjectForUrl(m_currentView->document()->url());
        m_includeDirectories.clear();

        // Invoke includeDirectories in advance. Running it in the background thread may crash because
        // of thread-safety issues in KConfig / CMakeUtils.
        if (m_currentProject)
        {
            KDevelop::ProjectBaseItem *project_item = m_currentProject->projectItem();
            IBuildSystemManager *buildSystemManager = 0;
            if (project_item && (buildSystemManager = m_currentProject->buildSystemManager()))
                m_includeDirectories = buildSystemManager->includeDirectories(project_item);
        }

        m_graphThreadRunning = true;
        ThreadWeaver::Weaver::instance()->enqueue(this);
    }
}

void DUChainControlFlow::processFunctionCall(Declaration *source, Declaration *target, const Use &use)
{
    FunctionDefinition *calledFunctionDefinition;
    DUContext *calledFunctionContext;

    // Convert to a declaration in accordance with control flow mode (function, class or namespace)
    Declaration *nodeSource = declarationFromControlFlowMode(source);
    Declaration *nodeTarget = declarationFromControlFlowMode(target);

    // Try to acquire the called function definition
    calledFunctionDefinition = FunctionDefinition::definition(target);

    QStringList sourceContainers, targetContainers;

    prepareContainers(sourceContainers, source);
    prepareContainers(targetContainers, target);

    QString sourceLabel = shortNameFromContainers(sourceContainers,
                          (m_controlFlowMode == ControlFlowNamespace &&
                           nodeSource->internalContext()->type() != DUContext::Namespace) ?
                                            globalNamespaceOrFolderNames(nodeSource) :
                                            prependFolderNames(nodeSource));

    QString targetLabel = shortNameFromContainers(targetContainers,
                          (m_controlFlowMode == ControlFlowNamespace &&
                           nodeTarget->internalContext()->type() != DUContext::Namespace) ?
                                            globalNamespaceOrFolderNames(nodeTarget) :
                                            prependFolderNames(nodeTarget));

    QString targetShortName = shortNameFromContainers(targetContainers, prependFolderNames(nodeTarget));
    QString sourceShortName = shortNameFromContainers(sourceContainers, prependFolderNames(nodeSource));

    if (sender() && dynamic_cast<ControlFlowGraphUsesCollector *>(sender()))
    {
        m_identifierDeclarationMap[sourceShortName] = IndexedDeclaration(nodeSource);
        sourceContainers.prepend(i18n("Uses of") + ' ' + targetLabel);
    }

    IndexedDeclaration ideclaration = IndexedDeclaration(calledFunctionDefinition);
    // If there is a flow (in accordance with control flow mode) emit signal
    if (targetLabel != sourceLabel ||
        m_controlFlowMode == ControlFlowFunction ||
        (calledFunctionDefinition && m_visitedFunctions.contains(ideclaration)))
        emit foundFunctionCall(sourceContainers, sourceLabel, targetContainers, targetLabel); 

    if (calledFunctionDefinition)
        calledFunctionContext = calledFunctionDefinition->internalContext();
    else
    {
        // Store method declaration for navigation
        m_identifierDeclarationMap[targetShortName] = IndexedDeclaration(nodeTarget);
        // Store use for edge inspection
        m_arcUsesMap.insert(sourceLabel + "->" + targetLabel, QPair<Use, IndexedString>(use, source->url()));
        return;
    }

    // Store use for edge inspection
    m_arcUsesMap.insert(sourceLabel + "->" + targetLabel, QPair<Use, IndexedString>(use, source->url()));
    // Store method definition for navigation
    m_identifierDeclarationMap[targetShortName] = IndexedDeclaration(declarationFromControlFlowMode(calledFunctionDefinition));

    if (calledFunctionContext && (m_currentLevel < m_maxLevel || m_maxLevel == 0))
    {
        // For prevent endless loop in recursive methods
        if (!m_visitedFunctions.contains(ideclaration))
        {
            ++m_currentLevel;
            m_visitedFunctions.insert(ideclaration);
            // Recursive call for method invocation
            useDeclarationsFromDefinition(calledFunctionDefinition, calledFunctionDefinition->topContext(), calledFunctionContext);
        }
    }
}

void DUChainControlFlow::slotUpdateToolTip(const QString &edge, const QPoint& point, QWidget *partWidget)
{
    ControlFlowGraphNavigationWidget *navigationWidget =
                new ControlFlowGraphNavigationWidget(edge, m_arcUsesMap.values(edge));
    
    KDevelop::NavigationToolTip *usesToolTip = new KDevelop::NavigationToolTip(
                                  partWidget,
                                  partWidget->mapToGlobal(QPoint(20, 20)) + point,
                                  navigationWidget);

    usesToolTip->resize(navigationWidget->sizeHint() + QSize(10, 10));
    ActiveToolTip::showToolTip(usesToolTip);
}

void DUChainControlFlow::slotGraphElementSelected(const QList<QString> list, const QPoint& point)
{
    if (!list.isEmpty())
    {
        QString label = list[0];
        Declaration *declaration = m_identifierDeclarationMap[label].data();
        
        if (!declaration)
            return;
        
        DUChainReadLocker lock(DUChain::lock());
        
        if (declaration) // Node click, jump to definition/declaration
        {
            KUrl url(declaration->url().str());
            KTextEditor::Range range = declaration->range().textRange();
            
            lock.unlock();
            
            ICore::self()->documentController()->openDocument(url, range.start());
        }
        else if (label.contains("->")) // Edge click, show uses contained in the edge
        {
            KParts::ReadOnlyPart *part = dynamic_cast<KParts::ReadOnlyPart *>(sender());
            if (!part) return;
            emit updateToolTip(label, point, part->widget());
        }
    }
}

void DUChainControlFlow::setLocked(bool locked)
{
    m_locked = locked;
}

void DUChainControlFlow::setUseFolderName(bool useFolderName)
{
    m_useFolderName = useFolderName;
}

void DUChainControlFlow::setUseShortNames(bool useShortNames)
{
    m_useShortNames = useShortNames;
}

void DUChainControlFlow::setDrawIncomingArcs(bool drawIncomingArcs)
{
    m_drawIncomingArcs = drawIncomingArcs;
}

void DUChainControlFlow::setMaxLevel(int maxLevel)
{
    m_maxLevel = maxLevel;
}

void DUChainControlFlow::refreshGraph()
{
    if (!m_locked)
    {
        if(ICore::self()->documentController()->activeDocument() &&
           ICore::self()->documentController()->activeDocument()->textDocument() &&
           ICore::self()->documentController()->activeDocument()->textDocument()->activeView())
        {
            m_previousUppermostExecutableContext = IndexedDUContext();
            KTextEditor::View *view = ICore::self()->documentController()->activeDocument()->textDocument()->activeView();
            cursorPositionChanged(view, view->cursorPosition());
        }
    }
}

void DUChainControlFlow::newGraph()
{
    m_visitedFunctions.clear();
    m_identifierDeclarationMap.clear();
    m_arcUsesMap.clear();
    m_currentProject = 0;
    emit clearGraph();
}

void DUChainControlFlow::slotThreadDone (ThreadWeaver::Job* job)
{
    if (job == this)
        m_graphThreadRunning = false;
}

void DUChainControlFlow::useDeclarationsFromDefinition (Declaration *definition, TopDUContext *topContext, DUContext *context)
{
    if (!topContext) return;

    const Use *uses = context->uses();
    unsigned int usesCount = context->usesCount();
    QVector<DUContext *> subContexts = context->childContexts();
    QVector<DUContext *>::iterator subContextsIterator = subContexts.begin();
    QVector<DUContext *>::iterator subContextsEnd      = subContexts.end();

    Declaration *declaration;
    for (unsigned int i = 0; i < usesCount; ++i)
    {
        declaration = topContext->usedDeclarationForIndex(uses[i].m_declarationIndex);
        if (declaration && declaration->type<KDevelop::FunctionType>())
        {
            if (subContextsIterator != subContextsEnd)
            {
                if (uses[i].m_range.start < (*subContextsIterator)->range().start)
                    processFunctionCall(definition, declaration, uses[i]);
                else if ((*subContextsIterator)->type() == DUContext::Other)
                {
                    // Recursive call for sub-contexts
                    useDeclarationsFromDefinition(definition, topContext, *subContextsIterator);
                    subContextsIterator++;
                    --i;
                }
            }
            else
                processFunctionCall(definition, declaration, uses[i]);
        }
    }
    while (subContextsIterator != subContextsEnd)
        if ((*subContextsIterator)->type() == DUContext::Other)
        {
            // Recursive call for remaining sub-contexts
            useDeclarationsFromDefinition(definition, topContext, *subContextsIterator);
            subContextsIterator++;
        }
}

Declaration *DUChainControlFlow::declarationFromControlFlowMode(Declaration *definitionDeclaration)
{
    Declaration *nodeDeclaration = definitionDeclaration;

    if (m_controlFlowMode != ControlFlowFunction)
    {
        if (nodeDeclaration->isDefinition())
            nodeDeclaration = DUChainUtils::declarationForDefinition(nodeDeclaration, nodeDeclaration->topContext());
        if (!nodeDeclaration || !nodeDeclaration->context() || !nodeDeclaration->context()->owner()) return definitionDeclaration;
        while (nodeDeclaration->context() &&
               nodeDeclaration->context()->owner() &&
               ((m_controlFlowMode == ControlFlowClass && nodeDeclaration->context()->type() == DUContext::Class) ||
                (m_controlFlowMode == ControlFlowNamespace && (
                                                              nodeDeclaration->context()->type() == DUContext::Class ||
                                                              nodeDeclaration->context()->type() == DUContext::Namespace)
              )))
            nodeDeclaration = nodeDeclaration->context()->owner();
    }
    return nodeDeclaration;
}

void DUChainControlFlow::prepareContainers(QStringList &containers, Declaration* definition)
{
    ControlFlowMode originalControlFlowMode = m_controlFlowMode;
    QString strGlobalNamespaceOrFolderNames;

    // Handling project clustering
    if (m_clusteringModes.testFlag(ClusteringProject) && ICore::self()->projectController()->findProjectForUrl(definition->url().str()))
        containers << ICore::self()->projectController()->findProjectForUrl(definition->url().str())->name();

    // Handling namespace clustering
    if (m_clusteringModes.testFlag(ClusteringNamespace))
    {
        m_controlFlowMode = ControlFlowNamespace;
        Declaration *namespaceDefinition = declarationFromControlFlowMode(definition);

        strGlobalNamespaceOrFolderNames = ((namespaceDefinition->internalContext()->type() != DUContext::Namespace) ?
                                                              globalNamespaceOrFolderNames(namespaceDefinition):
                                                              shortNameFromContainers(containers, prependFolderNames(namespaceDefinition)));
        foreach(const QString &container, strGlobalNamespaceOrFolderNames.split("::"))
            containers << container;
    }

    // Handling class clustering
    if (m_clusteringModes.testFlag(ClusteringClass))
    {
        m_controlFlowMode = ControlFlowClass;
        Declaration *classDefinition = declarationFromControlFlowMode(definition);
        
        if (classDefinition->internalContext() && classDefinition->internalContext()->type() == DUContext::Class)
            containers << shortNameFromContainers(containers, prependFolderNames(classDefinition));
    }

    m_controlFlowMode = originalControlFlowMode;
}

QString DUChainControlFlow::globalNamespaceOrFolderNames(Declaration *declaration)
{
    if (m_useFolderName && m_currentProject && m_includeDirectories.count() > 0)
    {
        int minLength = std::numeric_limits<int>::max();

        QString folderName, smallestDirectory, declarationUrl = declaration->url().str();

        foreach (const KUrl &url, m_includeDirectories)
        {
            QString urlString = url.toLocalFile();
            if (urlString.length() <= minLength && declarationUrl.startsWith(urlString))
            {
                smallestDirectory = urlString;
                minLength = urlString.length();
            }
        }
        declarationUrl = declarationUrl.remove(0, smallestDirectory.length());
        declarationUrl = declarationUrl.remove(KUrl(declaration->url().str()).fileName());
        if (declarationUrl.endsWith('/')) declarationUrl.chop(1);
        if (declarationUrl.startsWith('/')) declarationUrl.remove(0, 1);
        declarationUrl = declarationUrl.replace('/', "::");
        if (!declarationUrl.isEmpty())
            return declarationUrl;
    }
    return i18n("Global Namespace");
}

QString DUChainControlFlow::prependFolderNames(Declaration *declaration)
{
    QString prependedQualifiedName = declaration->qualifiedIdentifier().toString();
    if (m_useFolderName)
    {
        ControlFlowMode originalControlFlowMode = m_controlFlowMode;
        m_controlFlowMode = ControlFlowNamespace;
        Declaration *namespaceDefinition = declarationFromControlFlowMode(declaration);
        m_controlFlowMode = originalControlFlowMode;

        QString prefix = globalNamespaceOrFolderNames(namespaceDefinition);
        
        if (namespaceDefinition->internalContext()->type() != DUContext::Namespace &&
            prefix != i18n("Global Namespace"))
            prependedQualifiedName.prepend(prefix + "::");
    }

    return prependedQualifiedName;
}

QString DUChainControlFlow::shortNameFromContainers(const QList<QString> &containers, const QString &qualifiedIdentifier)
{
    QString shortName = qualifiedIdentifier;

    if (m_useShortNames)
    {
        foreach(const QString &container, containers)
            if (shortName.contains(container))
                shortName.remove(shortName.indexOf(container + "::"), (container + "::").length());
    }
    return shortName;
}
