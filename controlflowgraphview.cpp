/***************************************************************************
 *   Copyright 2009 Sandro Andrade <sandro.andrade@gmail.com>              *
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

#include "controlflowgraphview.h"

#include <kservice.h>
#include <klibloader.h>
#include <kparts/part.h>
#include <kmessagebox.h>
#include <kactioncollection.h>
#include <ktexteditor/document.h>
#include <ktexteditor/view.h>

#include <interfaces/idocument.h>
#include <interfaces/icore.h>

#include "duchaincontrolflow.h"
#include "dotcontrolflowgraph.h"

ControlFlowGraphView::ControlFlowGraphView(QWidget *parent)
: QWidget(parent), m_part(0),
m_duchainControlFlow(new DUChainControlFlow), m_dotControlFlowGraph(new DotControlFlowGraph)
{
    setupUi(this);
    KLibFactory *factory = KLibLoader::self()->factory("kgraphviewerpart");
    if (factory)
    {
        m_part = factory->create<KParts::ReadOnlyPart>(this);
  	if (m_part)
	{
	    connect(this, SIGNAL(setReadWrite()), m_part, SLOT(setReadWrite()));
	    emit setReadWrite();

	    horizontalLayout->addWidget(m_part->widget());
	    connect(zoomoutToolButton, SIGNAL(clicked()), m_part->actionCollection()->action("view_zoom_out"), SIGNAL(triggered()));
	    connect(zoominToolButton, SIGNAL(clicked()), m_part->actionCollection()->action("view_zoom_in"), SIGNAL(triggered()));
	    connect(m_part, SIGNAL(selectionIs(const QList<QString>, const QPoint&)),
		 m_duchainControlFlow, SLOT(selectionIs(const QList<QString>, const QPoint&)));

	    connect(m_duchainControlFlow,  SIGNAL(foundRootNode(const Declaration*)),
                    m_dotControlFlowGraph, SLOT  (foundRootNode(const Declaration*)));
	    connect(m_duchainControlFlow,  SIGNAL(foundFunctionCall(const Declaration*, const Declaration*)),
                    m_dotControlFlowGraph, SLOT  (foundFunctionCall(const Declaration*, const Declaration*)));
	    connect(m_duchainControlFlow,  SIGNAL(clearGraph()), m_dotControlFlowGraph, SLOT(clearGraph()));
	    connect(m_duchainControlFlow,  SIGNAL(graphDone()), m_dotControlFlowGraph, SLOT(graphDone()));
	    connect(m_dotControlFlowGraph, SIGNAL(loadLibrary(graph_t*)), m_part, SLOT(slotLoadLibrary(graph_t*)));
	}
        else
	    KMessageBox::error(this, i18n("Could not load the KGraphViewer kpart"));
    }
    else
        KMessageBox::error(this, i18n("Could not find the KGraphViewer factory"));
}

ControlFlowGraphView::~ControlFlowGraphView()
{
    if (m_duchainControlFlow != 0) delete m_duchainControlFlow;
    if (m_dotControlFlowGraph != 0) delete m_dotControlFlowGraph;
    if (m_part != 0) delete m_part;
}

void ControlFlowGraphView::textDocumentCreated(KDevelop::IDocument *document)
{
    disconnect(document->textDocument(), SIGNAL(viewCreated(KTextEditor::Document *, KTextEditor::View *)),
	    this, SLOT(viewCreated(KTextEditor::Document *, KTextEditor::View *)));
    connect(document->textDocument(), SIGNAL(viewCreated(KTextEditor::Document *, KTextEditor::View *)),
	    this, SLOT(viewCreated(KTextEditor::Document *, KTextEditor::View *)));
}

void ControlFlowGraphView::viewCreated(KTextEditor::Document * /* document */, KTextEditor::View *view)
{
    disconnect(view, SIGNAL(cursorPositionChanged(KTextEditor::View *, const KTextEditor::Cursor &)),
	    m_duchainControlFlow, SLOT(cursorPositionChanged(KTextEditor::View *, const KTextEditor::Cursor &)));
    connect(view, SIGNAL(cursorPositionChanged(KTextEditor::View *, const KTextEditor::Cursor &)),
	    m_duchainControlFlow, SLOT(cursorPositionChanged(KTextEditor::View *, const KTextEditor::Cursor &)));
    connect(view, SIGNAL(destroyed(QObject *)), m_duchainControlFlow, SLOT(viewDestroyed(QObject *)));
    connect(view, SIGNAL(focusIn(KTextEditor::View *)), m_duchainControlFlow, SLOT(focusIn(KTextEditor::View *)));
}
