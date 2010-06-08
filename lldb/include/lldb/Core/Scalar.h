//===-- Scalar.h ------------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef liblldb_Scalar_h_
#define liblldb_Scalar_h_

#include "lldb/lldb-private.h"

namespace lldb_private {

//----------------------------------------------------------------------
// A class designed to hold onto values and their corresponding types.
// Operators are defined and Scalar objects will correctly promote
// their types and values before performing these operations. Type
// promotion currently follows the ANSI C type promotion rules.
//----------------------------------------------------------------------
class Scalar
{
public:
    enum Type
    {
        e_void = 0,
        e_sint,
        e_uint,
        e_slong,
        e_ulong,
        e_slonglong,
        e_ulonglong,
        e_float,
        e_double,
        e_long_double
    };

    //------------------------------------------------------------------
    // Constructors and Destructors
    //------------------------------------------------------------------
    Scalar();
    Scalar(int v)               : m_type(e_sint),           m_data() { m_data.sint      = v; }
    Scalar(unsigned int v)      : m_type(e_uint),           m_data() { m_data.uint      = v; }
    Scalar(long v)              : m_type(e_slong),          m_data() { m_data.slong     = v; }
    Scalar(unsigned long v)     : m_type(e_ulong),          m_data() { m_data.ulong     = v; }
    Scalar(long long v)         : m_type(e_slonglong),      m_data() { m_data.slonglong = v; }
    Scalar(unsigned long long v): m_type(e_ulonglong),      m_data() { m_data.ulonglong = v; }
    Scalar(float v)             : m_type(e_float),          m_data() { m_data.flt       = v; }
    Scalar(double v)            : m_type(e_double),         m_data() { m_data.dbl       = v; }
    Scalar(long double v)       : m_type(e_long_double),    m_data() { m_data.ldbl      = v; }
    Scalar(const Scalar& rhs);
    //Scalar(const RegisterValue& reg_value);
    virtual ~Scalar();

    size_t
    GetByteSize() const;

    bool
    GetData (DataExtractor &data, size_t limit_byte_size = UINT32_MAX) const;

    bool
    IsZero() const;

    void
    Clear() { m_type = e_void; m_data.ulonglong = 0; }

    const char *
    GetTypeAsCString() const;

    void
    GetValue (Stream *s, bool show_type) const;

    bool
    IsValid() const
    {
        return (m_type >= e_sint) && (m_type <= e_long_double);
    }

    bool
    Promote(Scalar::Type type);

    bool
    Cast (Scalar::Type type);

    static const char *
    GetValueTypeAsCString (Scalar::Type value_type);

    static Scalar::Type
    GetValueTypeForSignedIntegerWithByteSize (size_t byte_size);

    static Scalar::Type
    GetValueTypeForUnsignedIntegerWithByteSize (size_t byte_size);

    static Scalar::Type
    GetValueTypeForFloatWithByteSize (size_t byte_size);

    //----------------------------------------------------------------------
    // All operators can benefits from the implicit conversions that will
    // happen automagically by the compiler, so no temporary objects will
    // need to be created. As a result, we currently don't need a variety of
    // overloaded set value accessors.
    //----------------------------------------------------------------------
    Scalar& operator= (const int i);
    Scalar& operator= (unsigned int v);
    Scalar& operator= (long v);
    Scalar& operator= (unsigned long v);
    Scalar& operator= (long long v);
    Scalar& operator= (unsigned long long v);
    Scalar& operator= (float v);
    Scalar& operator= (double v);
    Scalar& operator= (long double v);
    Scalar& operator= (const Scalar& rhs);      // Assignment operator
    Scalar& operator+= (const Scalar& rhs);
    Scalar& operator<<= (const Scalar& rhs);    // Shift left
    Scalar& operator>>= (const Scalar& rhs);    // Shift right (arithmetic)
    Scalar& operator&= (const Scalar& rhs);

    //----------------------------------------------------------------------
    // Shifts the current value to the right without maintaining the current
    // sign of the value (if it is signed).
    //----------------------------------------------------------------------
    bool
    ShiftRightLogical(const Scalar& rhs);   // Returns true on success

    //----------------------------------------------------------------------
    // Takes the absolute value of the current value if it is signed, else
    // the value remains unchanged.
    // Returns false if the contained value has a void type.
    //----------------------------------------------------------------------
    bool
    AbsoluteValue();                        // Returns true on success
    //----------------------------------------------------------------------
    // Negates the current value (even for unsigned values).
    // Returns false if the contained value has a void type.
    //----------------------------------------------------------------------
    bool
    UnaryNegate();                          // Returns true on success
    //----------------------------------------------------------------------
    // Inverts all bits in the current value as long as it isn't void or
    // a float/double/long double type.
    // Returns false if the contained value has a void/float/double/long
    // double type, else the value is inverted and true is returned.
    //----------------------------------------------------------------------
    bool
    OnesComplement();                       // Returns true on success

    //----------------------------------------------------------------------
    // Access the type of the current value.
    //----------------------------------------------------------------------
    Scalar::Type
    GetType() const { return m_type; }

    //----------------------------------------------------------------------
    // Returns a casted value of the current contained data without
    // modifying the current value. FAIL_VALUE will be returned if the type
    // of the value is void or invalid.
    //----------------------------------------------------------------------
    int
    SInt(int fail_value = 0) const;

    // Return the raw unsigned integer without any casting or conversion
    unsigned int
    RawUInt () const;

    // Return the raw unsigned long without any casting or conversion
    unsigned long
    RawULong () const;

    // Return the raw unsigned long long without any casting or conversion
    unsigned long long
    RawULongLong () const;

    unsigned int
    UInt(unsigned int fail_value = 0) const;

    long
    SLong(long fail_value = 0) const;

    unsigned long
    ULong(unsigned long fail_value = 0) const;

    long long
    SLongLong(long long fail_value = 0) const;

    unsigned long long
    ULongLong(unsigned long long fail_value = 0) const;

    float
    Float(float fail_value = 0.0f) const;

    double
    Double(double fail_value = 0.0) const;

    long double
    LongDouble(long double fail_value = 0.0) const;

    uint64_t
    GetRawBits64 (uint64_t fail_value) const;

    Error
    SetValueFromCString (const char *s, lldb::Encoding encoding, uint32_t byte_size);

    static bool
    UIntValueIsValidForSize (uint64_t uval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const uint64_t max = ((uint64_t)1 << (uint64_t)(total_byte_size * 8)) - 1;
        return uval64 <= max;
    }

    static bool
    SIntValueIsValidForSize (int64_t sval64, size_t total_byte_size)
    {
        if (total_byte_size > 8)
            return false;

        if (total_byte_size == 8)
            return true;

        const int64_t max = ((int64_t)1 << (uint64_t)(total_byte_size * 8 - 1)) - 1;
        const int64_t min = ~(max);
        return min <= sval64 && sval64 <= max;
    }

protected:
    typedef union ValueData
    {
        int                 sint;
        unsigned int        uint;
        long                slong;
        unsigned long       ulong;
        long long           slonglong;
        unsigned long long  ulonglong;
        float               flt;
        double              dbl;
        long double         ldbl;
    };

    //------------------------------------------------------------------
    // Classes that inherit from Scalar can see and modify these
    //------------------------------------------------------------------
    Scalar::Type m_type;
    ValueData m_data;

private:
    friend const Scalar operator+   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator-   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator/   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator*   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator&   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator|   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator%   (const Scalar& lhs, const Scalar& rhs);
    friend const Scalar operator^   (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator== (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator!= (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator<  (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator<= (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator>  (const Scalar& lhs, const Scalar& rhs);
    friend          bool operator>= (const Scalar& lhs, const Scalar& rhs);

};

//----------------------------------------------------------------------
// Split out the operators into a format where the compiler will be able
// to implicitly convert numbers into Scalar objects.
//
// This allows code like:
//      Scalar two(2);
//      Scalar four = two * 2;
//      Scalar eight = 2 * four;    // This would cause an error if the
//                                  // operator* was implemented as a
//                                  // member function.
// SEE:
//  Item 19 of "Effective C++ Second Edition" by Scott Meyers
//  Differentiate among members functions, non-member functions, and
//  friend functions
//----------------------------------------------------------------------
const Scalar operator+ (const Scalar& lhs, const Scalar& rhs);
const Scalar operator- (const Scalar& lhs, const Scalar& rhs);
const Scalar operator/ (const Scalar& lhs, const Scalar& rhs);
const Scalar operator* (const Scalar& lhs, const Scalar& rhs);
const Scalar operator& (const Scalar& lhs, const Scalar& rhs);
const Scalar operator| (const Scalar& lhs, const Scalar& rhs);
const Scalar operator% (const Scalar& lhs, const Scalar& rhs);
const Scalar operator^ (const Scalar& lhs, const Scalar& rhs);
bool operator== (const Scalar& lhs, const Scalar& rhs);
bool operator!= (const Scalar& lhs, const Scalar& rhs);
bool operator<  (const Scalar& lhs, const Scalar& rhs);
bool operator<= (const Scalar& lhs, const Scalar& rhs);
bool operator>  (const Scalar& lhs, const Scalar& rhs);
bool operator>= (const Scalar& lhs, const Scalar& rhs);

} // namespace lldb_private

#endif  // liblldb_Scalar_h_
