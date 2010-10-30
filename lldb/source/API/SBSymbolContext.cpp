//===-- SBSymbolContext.cpp -------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "lldb/API/SBSymbolContext.h"
#include "lldb/API/SBStream.h"
#include "lldb/Symbol/SymbolContext.h"
#include "lldb/Core/Log.h"

using namespace lldb;
using namespace lldb_private;



SBSymbolContext::SBSymbolContext () :
    m_opaque_ap ()
{
}

SBSymbolContext::SBSymbolContext (const SymbolContext *sc_ptr) :
    m_opaque_ap ()
{
    if (sc_ptr)
        m_opaque_ap.reset (new SymbolContext (*sc_ptr));
}

SBSymbolContext::SBSymbolContext (const SBSymbolContext& rhs) :
    m_opaque_ap ()
{
    if (rhs.IsValid())
    {
        if (m_opaque_ap.get())
            *m_opaque_ap = *rhs.m_opaque_ap;
        else
            ref() = *rhs.m_opaque_ap;
    }
}

SBSymbolContext::~SBSymbolContext ()
{
}

const SBSymbolContext &
SBSymbolContext::operator = (const SBSymbolContext &rhs)
{
    if (this != &rhs)
    {
        if (rhs.IsValid())
            m_opaque_ap.reset (new lldb_private::SymbolContext(*rhs.m_opaque_ap.get()));
    }
    return *this;
}

void
SBSymbolContext::SetSymbolContext (const SymbolContext *sc_ptr)
{
    if (sc_ptr)
    {
        if (m_opaque_ap.get())
            *m_opaque_ap = *sc_ptr;
        else
            m_opaque_ap.reset (new SymbolContext (*sc_ptr));
    }
    else
    {
        if (m_opaque_ap.get())
            m_opaque_ap->Clear();
    }
}

bool
SBSymbolContext::IsValid () const
{
    return m_opaque_ap.get() != NULL;
}



SBModule
SBSymbolContext::GetModule ()
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    SBModule sb_module;
    if (m_opaque_ap.get())
        sb_module.SetModule(m_opaque_ap->module_sp);

    if (log)
    {
        SBStream sstr;
        sb_module.GetDescription (sstr);
        log->Printf ("SBSymbolContext(%p)::GetModule () => SBModule(%p): %s", 
                     m_opaque_ap.get(), sb_module.get(), sstr.GetData());
    }

    return sb_module;
}

SBCompileUnit
SBSymbolContext::GetCompileUnit ()
{
    return SBCompileUnit (m_opaque_ap.get() ? m_opaque_ap->comp_unit : NULL);
}

SBFunction
SBSymbolContext::GetFunction ()
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    SBFunction ret_function (m_opaque_ap.get() ? m_opaque_ap->function : NULL);

    if (log)
        log->Printf ("SBSymbolContext(%p)::GetFunction () => SBFunction(%p): %s", 
                     m_opaque_ap.get(), ret_function.get(), ret_function.GetName());

    return ret_function;
}

SBBlock
SBSymbolContext::GetBlock ()
{
    return SBBlock (m_opaque_ap.get() ? m_opaque_ap->block : NULL);
}

SBLineEntry
SBSymbolContext::GetLineEntry ()
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    SBLineEntry sb_line_entry;
    if (m_opaque_ap.get())
        sb_line_entry.SetLineEntry (m_opaque_ap->line_entry);

    if (log)
    {
        SBStream sstr;
        sb_line_entry.GetDescription (sstr);
        log->Printf ("SBSymbolContext(%p)::GetLineEntry () => SBLineEntry(%p): %s", 
                     m_opaque_ap.get(),
                     sb_line_entry.get(), sstr.GetData());
    }

    return sb_line_entry;
}

SBSymbol
SBSymbolContext::GetSymbol ()
{
    Log *log = lldb_private::GetLogIfAllCategoriesSet (LIBLLDB_LOG_API);

    SBSymbol ret_symbol (m_opaque_ap.get() ? m_opaque_ap->symbol : NULL);

    if (log)
    {
        SBStream sstr;
        ret_symbol.GetDescription (sstr);
        log->Printf ("SBSymbolContext(%p)::GetSymbol () => SBSymbol(%p): %s", m_opaque_ap.get(),
                     ret_symbol.get(), sstr.GetData());
    }

    return ret_symbol; 
}

lldb_private::SymbolContext*
SBSymbolContext::operator->() const
{
    return m_opaque_ap.get();
}


const lldb_private::SymbolContext&
SBSymbolContext::operator*() const
{
    assert (m_opaque_ap.get());
    return *m_opaque_ap.get();
}


lldb_private::SymbolContext&
SBSymbolContext::operator*()
{
    if (m_opaque_ap.get() == NULL)
        m_opaque_ap.reset (new SymbolContext);
    return *m_opaque_ap.get();
}

lldb_private::SymbolContext&
SBSymbolContext::ref()
{
    if (m_opaque_ap.get() == NULL)
        m_opaque_ap.reset (new SymbolContext);
    return *m_opaque_ap.get();
}

lldb_private::SymbolContext *
SBSymbolContext::get() const
{
    return m_opaque_ap.get();
}

bool
SBSymbolContext::GetDescription (SBStream &description)
{
    if (m_opaque_ap.get())
    {
        description.ref();
        m_opaque_ap->GetDescription (description.get(), lldb::eDescriptionLevelFull, NULL);
    }
    else
        description.Printf ("No value");

    return true;
}
