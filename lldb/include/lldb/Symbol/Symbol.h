//===-- Symbol.h ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Symbol_h_
#define liblldb_Symbol_h_

#include "lldb/lldb-private.h"
#include "lldb/Core/AddressRange.h"
#include "lldb/Core/Mangled.h"
#include "lldb/Core/UserID.h"

namespace lldb_private {

class Symbol :
    public UserID   // Used to uniquely identify this symbol in its symbol table
{
public:
    // ObjectFile readers can classify their symbol table entries and searches can be made
    // on specific types where the symbol values will have drastically different meanings
    // and sorting requirements.
    Symbol();

    Symbol (lldb::user_id_t symID,
            const char *name,
            bool name_is_mangled,
            lldb::SymbolType type,
            bool external,
            bool is_debug,
            bool is_trampoline,
            bool is_artificial,
            const Section* section,
            lldb::addr_t value,
            uint32_t size,
            uint32_t flags);

    Symbol (lldb::user_id_t symID,
            const char *name,
            bool name_is_mangled,
            lldb::SymbolType type,
            bool external,
            bool is_debug,
            bool is_trampoline,
            bool is_artificial,
            const AddressRange &range,
            uint32_t flags);

    Symbol (const Symbol& rhs);

    const Symbol&
    operator= (const Symbol& rhs);

    bool
    Compare (const ConstString& name, lldb::SymbolType type) const;

    void
    Dump (Stream *s, Process *process, uint32_t index) const;

    AddressRange *
    GetAddressRangePtr ();

    const AddressRange *
    GetAddressRangePtr () const;

    AddressRange &
    GetAddressRangeRef();

    const AddressRange &
    GetAddressRangeRef() const;

    Mangled&
    GetMangled ();

    const Mangled&
    GetMangled () const;

    bool
    GetSizeIsSibling () const;

    bool
    GetSizeIsSynthesized() const;

    uint32_t
    GetSiblingIndex () const;

    uint32_t
    GetByteSize () const;

    lldb::SymbolType
    GetType () const;

    void
    SetType (lldb::SymbolType type);

    const char *
    GetTypeAsString () const;

    uint32_t
    GetFlags () const;

    void
    SetFlags (uint32_t flags);

    Function *
    GetFunction ();

    Address &
    GetValue ();

    const Address &
    GetValue () const;

    bool
    IsSynthetic () const;

    void
    SetIsSynthetic (bool b);

    void
    SetSizeIsSynthesized(bool b);

    bool
    IsDebug () const;

    void
    SetDebug (bool b);

    bool
    IsExternal () const;

    void
    SetExternal (bool b);

    bool
    IsTrampoline () const;

    void
    SetByteSize (uint32_t size);

    void
    SetSizeIsSibling (bool b);

    void
    SetValue (Address &value);

    void
    SetValue (const AddressRange &range);

    void
    SetValue (lldb::addr_t value);

    // If m_type is "Code" or "Function" then this will return the prologue size
    // in bytes, else it will return zero.
    uint32_t
    GetPrologueByteSize ();

protected:

    Mangled         m_mangled;              // uniqued symbol name/mangled name pair
    lldb::SymbolType m_type;                 // symbol type
    uint16_t        m_type_data;            // data specific to m_type
    uint16_t        m_type_data_resolved:1, // True if the data in m_type_data has already been calculated
                    m_is_synthetic:1,       // non-zero if this symbol is not actually in the symbol table, but synthesized from other info in the object file.
                    m_is_debug:1,           // non-zero if this symbol is debug information in a symbol
                    m_is_external:1,        // non-zero if this symbol is globally visible
                    m_size_is_sibling:1,    // m_size contains the index of this symbol's sibling
                    m_size_is_synthesized:1,// non-zero if this symbol's size was calculated using a delta between this symbol and the next
                    m_searched_for_function:1;// non-zero if we have looked for the function associated with this symbol already.
    AddressRange    m_addr_range;           // Contains the value, or the section offset address when the value is an address in a section, and the size (if any)
    uint32_t        m_flags;                // A copy of the flags from the original symbol table, the ObjectFile plug-in can interpret these
    Function *      m_function;
};

} // namespace lldb_private

#endif  // liblldb_Symbol_h_
