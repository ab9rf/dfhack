/*
https://github.com/peterix/dfhack
Copyright (c) 2009-2012 Petr Mrázek (peterix@gmail.com)

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any
damages arising from the use of this software.

Permission is granted to anyone to use this software for any
purpose, including commercial applications, and to alter it and
redistribute it freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must
not claim that you wrote the original software. If you use this
software in a product, an acknowledgment in the product documentation
would be appreciated but is not required.

2. Altered source versions must be plainly marked as such, and
must not be misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/

#pragma once

#include <map>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "BitArray.h"

// Stop some MS stupidity
#ifdef interface
    #undef interface
#endif

typedef struct lua_State lua_State;

/*
 * Definitions of DFHack namespace structs used by generated headers.
 */

 // forward def'n
namespace df {
    template<class T> void* allocator_fn(void* out, const void* in);
}

namespace DFHack
{
    class Core;
    class virtual_class {};

    enum identity_type {
        IDTYPE_GLOBAL,
        IDTYPE_FUNCTION,
        IDTYPE_PRIMITIVE,
        IDTYPE_POINTER,
        IDTYPE_CONTAINER,
        IDTYPE_PTR_CONTAINER,
        IDTYPE_BIT_CONTAINER,
        IDTYPE_BITFIELD,
        IDTYPE_ENUM,
        IDTYPE_STRUCT,
        IDTYPE_CLASS,
        IDTYPE_BUFFER,
        IDTYPE_STL_PTR_VECTOR,
        IDTYPE_OPAQUE,
        IDTYPE_UNION
    };

    // pointer flags (bitfield), stored in the count field of struct_field_info
    // if mode is POINTER.
    enum pointer_identity_flags {
        PTRFLAG_IS_ARRAY = 1,
        PTRFLAG_HAS_BAD_POINTERS = 2,
    };

    typedef void *(*TAllocateFn)(void*,const void*);

    class DFHACK_EXPORT type_identity {
        const size_t size;
        const std::type_info& id;

    protected:
        type_identity(size_t size, const std::type_info& id) : size(size), id(id) {};

        void *do_allocate_pod() const;
        void do_copy_pod(void *tgt, const void *src) const;
        bool do_destroy_pod(void *obj) const;

        virtual bool can_allocate() const { return true; }
        virtual void *do_allocate() const { return do_allocate_pod(); }
        virtual bool do_copy(void *tgt, const void *src) const { do_copy_pod(tgt, src); return true; }
        virtual bool do_destroy(void *obj) const { return do_destroy_pod(obj); }

    public:
        virtual ~type_identity() {}

        virtual size_t byte_size() const { return size; }

        virtual identity_type type() const = 0;

        virtual std::string getFullName() const = 0;

        // For internal use in the lua wrapper
        virtual void lua_read(lua_State *state, int fname_idx, void *ptr) const = 0;
        virtual void lua_write(lua_State *state, int fname_idx, void *ptr, int val_index) const = 0;
        virtual void build_metatable(lua_State *state) const;

        // lua_read doesn't just return a reference to the object
        virtual bool isPrimitive() const { return true; }
        // needs constructor/destructor
        virtual bool isConstructed() const { return false; }
        // inherits from container_identity
        virtual bool isContainer() const { return false; }

        void *allocate() const;
        bool copy(void *tgt, const void *src) const;
        bool destroy(void *obj) const;

        bool operator==(const type_identity& rhs) const { return id == rhs.id; }
    };

    class DFHACK_EXPORT constructed_identity_base : public type_identity {
        TAllocateFn allocator;

    protected:
        constructed_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc)
            : type_identity(size, id), allocator(alloc) {};

        virtual bool can_allocate() const { return (allocator != NULL); }
        virtual void *do_allocate() const { return allocator(NULL,NULL); }
        virtual bool do_copy(void *tgt, const void *src) const { return allocator(tgt,src) == tgt; }
        virtual bool do_destroy(void *obj) const { return allocator(NULL,obj) == obj; }
    public:
        virtual bool isPrimitive() const { return false; }
        virtual bool isConstructed() const { return true; }

        virtual void lua_read(lua_State *state, int fname_idx, void *ptr) const;
        virtual void lua_write(lua_State *state, int fname_idx, void *ptr, int val_index) const;
    };

    template<typename T>
    class constructed_identity : public constructed_identity_base {
    protected:
        constructed_identity() : constructed_identity_base(sizeof(T), typeid(T), &df::allocator_fn<T>) {};
    };

    class DFHACK_EXPORT compound_identity_base : public constructed_identity_base {
        static compound_identity_base *list;
        compound_identity_base *next;

        const char *dfhack_name;
        const compound_identity_base *scope_parent;
        mutable std::vector<const compound_identity_base*> scope_children;
        static std::vector<const compound_identity_base*> top_scope;

    protected:
        compound_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc,
                          const compound_identity_base *scope_parent, const char *dfhack_name);

        virtual void doInit(Core *core) const;

    public:
        const char *getName() const { return dfhack_name; }

        virtual std::string getFullName() const;

        const compound_identity_base *getScopeParent() const { return scope_parent; }
        const std::vector<const compound_identity_base*> &getScopeChildren() const { return scope_children; }
        static std::vector<const compound_identity_base*> &getTopScope() { return top_scope; }

        static void Init(Core *core);
    };

    template<typename T>
    class compound_identity : public compound_identity_base {
    protected:
        compound_identity(compound_identity_base* scope_parent, const char* dfhack_name) : compound_identity_base(sizeof(T), typeid(T), &df::allocator_fn, scope_parent, dfhack_name) {};
    };

    // Bitfields
    struct bitfield_item_info {
        // the name of the field, or null if the field is unnamed
        const char *name;
        // size is positive for defined fields, zero for bits past the end
        // of the field, and negative for padding on multi-bit fields
        //
        // ex. if bits[2].size is -2, then bits[0].size is at least 3
        int size;
    };

    class DFHACK_EXPORT bitfield_identity_base : public compound_identity_base {
        const bitfield_item_info *bits;
        int num_bits;

    protected:
        virtual bool can_allocate() { return true; }
        virtual void *do_allocate() { return do_allocate_pod(); }
        virtual bool do_copy(void *tgt, const void *src) { do_copy_pod(tgt, src); return true; }
        virtual bool do_destroy(void *obj) { return do_destroy_pod(obj); }

        bitfield_identity_base(size_t size, const std::type_info& id,
                          const compound_identity_base *scope_parent, const char *dfhack_name,
                          int num_bits, const bitfield_item_info *bits);

    public:
        virtual identity_type type() const { return IDTYPE_BITFIELD; }

        virtual bool isConstructed() const { return false; }

        int getNumBits() const { return num_bits; }
        const bitfield_item_info *getBits() const { return bits; }

        virtual void build_metatable(lua_State *state);
    };

    template<typename T>
    class bitfield_identity : public bitfield_identity_base {
    public:
        bitfield_identity(const compound_identity_base* scope_parent, const char* dfhack_name,
            int num_bits, const bitfield_item_info* bits) :
            bitfield_identity_base(sizeof(T), typeid(T), scope_parent, dfhack_name, num_bits, bits) {};

    };

    class struct_identity_base;

    class DFHACK_EXPORT enum_identity_base : public compound_identity_base {
    public:
        struct ComplexData {
            std::map<int64_t, size_t> value_index_map;
            std::vector<int64_t> index_value_map;
            ComplexData(std::initializer_list<int64_t> values);
            size_t size() const {
                return index_value_map.size();
            }
        };

    private:
        const char *const *keys;
        const ComplexData *complex;
        int64_t first_item_value;
        int64_t last_item_value;
        int count;

        const type_identity *base_type;

        const void *attrs;
        const struct_identity_base *attr_type;

    protected:
        virtual bool can_allocate() const { return true; }
        virtual void *do_allocate() const;
        virtual bool do_copy(void *tgt, const void *src) const { do_copy_pod(tgt, src); return true; }
        virtual bool do_destroy(void *obj) const { return do_destroy_pod(obj); }

        enum_identity_base(size_t size, const std::type_info& id,
                      const compound_identity_base *scope_parent, const char *dfhack_name,
                      const type_identity *base_type,
                      int64_t first_item_value, int64_t last_item_value,
                      const char *const *keys,
                      const ComplexData *complex,
                      const void *attrs, const struct_identity_base *attr_type);
    public:
        enum_identity_base(const enum_identity_base *enum_type, const std::type_info& id, const type_identity *override_base_type);

        virtual identity_type type() const { return IDTYPE_ENUM; }

        int64_t getFirstItem() const { return first_item_value; }
        int64_t getLastItem() const { return last_item_value; }
        int getCount() const { return count; }
        const char *const *getKeys() const { return keys; }
        const ComplexData *getComplex() const { return complex; }

        const type_identity *getBaseType() const { return base_type; }
        const void *getAttrs() const { return attrs; }
        const struct_identity_base *getAttrType() const { return attr_type; }

        virtual bool isPrimitive() const { return true; }
        virtual bool isConstructed() const { return false; }

        virtual void lua_read(lua_State *state, int fname_idx, void *ptr) const;
        virtual void lua_write(lua_State *state, int fname_idx, void *ptr, int val_index) const;
    };

    template<typename T>
    class enum_identity : public enum_identity_base {
    public:
        enum_identity(const compound_identity_base* scope_parent, const char* dfhack_name,
            const type_identity* base_type,
            int64_t first_item_value, int64_t last_item_value,
            const char* const* keys,
            const ComplexData* complex,
            const void* attrs, const struct_identity_base* attr_type) :
            enum_identity_base(sizeof(T), typeid(T), scope_parent, dfhack_name, base_type, first_item_value, last_item_value, keys, complex, attrs, attr_type ) {};
    };

    struct struct_field_info_extra {
        const enum_identity_base *index_enum;
        const type_identity *ref_target;
        const char *union_tag_field;
        const char *union_tag_attr;
        const char *original_name;
    };

    struct struct_field_info {
        enum Mode {
            END,
            PRIMITIVE,
            STATIC_STRING,
            POINTER,
            STATIC_ARRAY,
            SUBSTRUCT,
            CONTAINER,
            STL_VECTOR_PTR,
            OBJ_METHOD,
            CLASS_METHOD
        };
        Mode mode;
        const char *name;
        size_t offset;
        const type_identity *type;
        size_t count;
        const struct_field_info_extra *extra;
    };

    class DFHACK_EXPORT struct_identity_base : public compound_identity_base {
        const struct_identity_base *parent;
        mutable std::vector<const struct_identity_base*> children;
        bool has_children;

        const struct_field_info *fields;

    protected:
        virtual void doInit(Core* core) const;

        struct_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc,
            const compound_identity_base *scope_parent, const char *dfhack_name,
            const struct_identity_base *parent, const struct_field_info *fields);
    public:

        virtual identity_type type() const { return IDTYPE_STRUCT; }

        const struct_identity_base *getParent() const { return parent; }
        const std::vector<const struct_identity_base*> &getChildren() const { return children; }
        bool hasChildren() const { return has_children; }

        const struct_field_info *getFields() const { return fields; }

        bool is_subclass(const struct_identity_base *subtype) const;

        virtual void build_metatable(lua_State *state) const;
    };

    template <typename T>
    class struct_identity : public struct_identity_base {
    public:
        struct_identity(const compound_identity_base* scope_parent, const char* dfhack_name,
            const struct_identity_base* parent, const struct_field_info* fields)
            : struct_identity_base(sizeof(T), typeid(T), &df::allocator_fn<T>, scope_parent, dfhack_name, parent, fields) {};
    };

    class DFHACK_EXPORT global_identity : public struct_identity_base {
        // this class exists solely to provide a distinct typeid for this metaobject
    private:
        class global_holder {
        };
    public:
        global_identity(const struct_field_info* fields)
            : struct_identity_base(0, typeid(global_holder), NULL, NULL, "global", NULL, fields) {}

        virtual identity_type type() const { return IDTYPE_GLOBAL; }

        virtual void build_metatable(lua_State *state) const;
    };

    class DFHACK_EXPORT union_identity_base : public struct_identity_base {
    protected:
        union_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc,
                const compound_identity_base *scope_parent, const char *dfhack_name,
                const struct_identity_base *parent, const struct_field_info *fields);
    public:
        virtual identity_type type() const { return IDTYPE_UNION; }

        virtual void build_metatable(lua_State *state) const;
    };

    template<typename T>
    class union_identity : public union_identity_base {
    public:
        union_identity(const compound_identity_base* scope_parent, const char* dfhack_name,
            const struct_identity_base* parent, const struct_field_info* fields) :
            union_identity_base(sizeof(T), typeid(T), &df::allocator_fn<T>, scope_parent, dfhack_name, parent, fields) {};
    };

    class DFHACK_EXPORT other_vectors_identity_base : public struct_identity_base {
        const enum_identity_base *index_enum;

    protected:
        other_vectors_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc,
            const compound_identity_base *scope_parent, const char *dfhack_name,
            const struct_identity_base *parent, const struct_field_info *fields,
            const enum_identity_base *index_enum) :
            struct_identity_base(size, id, alloc, scope_parent, dfhack_name, parent, fields),
            index_enum(index_enum)
        {}

    public:
        const enum_identity_base *getIndexEnum() const { return index_enum; }

        virtual void build_metatable(lua_State *state);
    };

    template <typename T>
    class other_vectors_identity : public other_vectors_identity_base
    {
    public:
        other_vectors_identity(const compound_identity_base* scope_parent, const char* dfhack_name,
            const struct_identity_base* parent, const struct_field_info* fields,
            const enum_identity_base* index_enum) :
            other_vectors_identity_base(sizeof(T), typeid(T), &df::allocator_fn<T>, scope_parent, dfhack_name, parent, fields, index_enum) {};
    };


#ifdef _MSC_VER
    typedef void *virtual_ptr;
#else
    typedef virtual_class *virtual_ptr;
#endif

    class DFHACK_EXPORT VMethodInterposeLinkBase;
    class MemoryPatcher;

    class DFHACK_EXPORT virtual_identity_base : public struct_identity_base {
        static std::map<void*, virtual_identity_base*> known;

        const char *original_name;

        void *vtable_ptr;

        bool is_plugin;

        friend class VMethodInterposeLinkBase;
        std::map<int,VMethodInterposeLinkBase*> interpose_list;

    protected:
        virtual void doInit(Core *core);

        static void *get_vtable(virtual_ptr instance_ptr) { return *(void**)instance_ptr; }

        bool can_allocate() const { return struct_identity_base::can_allocate() && (vtable_ptr != NULL); }

        void *get_vmethod_ptr(int index) const;
        bool set_vmethod_ptr(MemoryPatcher &patcher, int index, void *ptr) const;

        virtual_identity_base(size_t size, const std::type_info& id, TAllocateFn alloc,
                         const char *dfhack_name, const char *original_name,
            const virtual_identity_base *parent, const struct_field_info *fields,
                         bool is_plugin = false);
    public:
        ~virtual_identity_base();

        virtual identity_type type() { return IDTYPE_CLASS; }

        const char *getOriginalName() { return original_name ? original_name : getName(); }

    public:
        static virtual_identity_base *get(virtual_ptr instance_ptr);

        static virtual_identity_base *find(void *vtable);
        static virtual_identity_base *find(const std::string &name);

        bool is_instance(virtual_ptr instance_ptr) const {
            if (!instance_ptr) return false;
            if (vtable_ptr) {
                void *vtable = get_vtable(instance_ptr);
                if (vtable == vtable_ptr) return true;
                if (!hasChildren()) return false;
            }
            return is_subclass(get(instance_ptr));
        }

        bool is_direct_instance(virtual_ptr instance_ptr) const {
            if (!instance_ptr) return false;
            return vtable_ptr ? (vtable_ptr == get_vtable(instance_ptr))
                              : (this == get(instance_ptr));
        }

        template<class P> static P get_vmethod_ptr(P selector);

    public:
        bool can_instantiate() const { return can_allocate(); }
        virtual_ptr instantiate() const { return can_instantiate() ? (virtual_ptr)do_allocate() : NULL; }
        static virtual_ptr clone(virtual_ptr obj);

    public:
        // Strictly for use in virtual class constructors
        void adjust_vtable(virtual_ptr obj, const virtual_identity_base* main) const;
    };

    template<class T>
    class virtual_identity : public virtual_identity_base {
    public:
        virtual_identity(TAllocateFn alloc, const char* dfhack_name, const char* original_name,
            const virtual_identity_base* parent, const struct_field_info* fields,
            bool is_plugin = false) : virtual_identity_base(sizeof(T), typeid(T),
                alloc, dfhack_name, original_name, parent, fields, is_plugin) {};
    };

    template<class T>
    inline T *virtual_cast(virtual_ptr ptr) {
        return T::_identity.is_instance(ptr) ? static_cast<T*>(ptr) : NULL;
    }

#define VIRTUAL_CAST_VAR(var,type,input) type *var = virtual_cast<type>(input)

    template<class T>
    inline T *strict_virtual_cast(virtual_ptr ptr) {
        return T::_identity.is_direct_instance(ptr) ? static_cast<T*>(ptr) : NULL;
    }

#define STRICT_VIRTUAL_CAST_VAR(var,type,input) type *var = strict_virtual_cast<type>(input)

    void InitDataDefGlobals(Core *core);

    template<class T>
    T *ifnull(T *a, T *b) { return a ? a : b; }

    template<class T>
    struct enum_list_attr {
        size_t size;
        const T *items;
    };
}

template<class T>
int linear_index(const DFHack::enum_list_attr<T> &lst, T val) {
    for (size_t i = 0; i < lst.size; i++)
        if (lst.items[i] == val)
            return i;
    return -1;
}

inline int linear_index(const DFHack::enum_list_attr<const char*> &lst, const std::string &val) {
    for (size_t i = 0; i < lst.size; i++)
        if (lst.items[i] == val)
            return (int)i;
    return -1;
}

/*
 * Definitions of df namespace structs used by generated headers.
 */

namespace df
{
    using DFHack::type_identity;
    using DFHack::compound_identity_base;
    using DFHack::virtual_ptr;
    using DFHack::virtual_identity_base;
    using DFHack::virtual_identity;
    using DFHack::virtual_class;
    using DFHack::global_identity;
    using DFHack::struct_identity_base;
    using DFHack::struct_identity;
    using DFHack::union_identity_base;
    using DFHack::union_identity;
    using DFHack::other_vectors_identity_base;
    using DFHack::other_vectors_identity;
    using DFHack::struct_field_info;
    using DFHack::struct_field_info_extra;
    using DFHack::bitfield_item_info;
    using DFHack::bitfield_identity_base;
    using DFHack::bitfield_identity;
    using DFHack::enum_identity_base;
    using DFHack::enum_identity;
    using DFHack::enum_list_attr;
    using DFHack::BitArray;
    using DFHack::DfArray;
    using DFHack::DfLinkedList;
    using DFHack::DfOtherVectors;

    template<class T>
    void* allocator_try_assign(void* out, const void* in) {
        if constexpr (std::is_copy_assignable_v<T>) {
            *(T*)out = *(const T*)in;
            return out;
        }
        else
        {
            // cannot copy assign, do nothing
            return NULL;
        }
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdelete-non-virtual-dtor"
    template<class T>
    void *allocator_fn(void *out, const void *in) {
        if (out) { return allocator_try_assign<T>(out, in); }
        else if (in) { delete (T*)in; return (T*)in; }
        else return new T();
    }
#pragma GCC diagnostic pop

    template<class T>
    void *allocator_nodel_fn(void *out, const void *in) {
        if (out) { *(T*)out = *(const T*)in; return out; }
        else if (in) { return NULL; }
        else return new T();
    }

    template<class T>
    void *allocator_noassign_fn(void *out, const void *in) {
        if (out) { return NULL; }
        else if (in) { delete (T*)in; return (T*)in; }
        else return new T();
    }

    template<class T>
    struct identity_traits {};

    template<typename T>
    struct enum_traits {};

    template<typename T>
    struct bitfield_traits {};

    template<class T>
    concept HasCompoundIdentity = std::is_compound_v<T> && requires { { &T::_identity } -> std::convertible_to<const compound_identity_base*>; };

    template<HasCompoundIdentity T>
    struct identity_traits<T> {
        static const compound_identity_base* get() { return &T::_identity; }
    };

    template<class T>
    inline T* allocate() { return (T*)identity_traits<T>::get()->allocate(); }

    template<class EnumType, class IntType = int32_t>
    struct enum_field {
        IntType value;

        enum_field() {}
        enum_field(EnumType ev) : value(IntType(ev)) {}
        template<class T>
        enum_field(enum_field<EnumType,T> ev) : value(IntType(ev.value)) {}

        operator EnumType () const { return EnumType(value); }
        enum_field<EnumType,IntType> &operator=(EnumType ev) {
            value = IntType(ev); return *this;
        }
    };

    template<class ET, class IT>
    struct enum_traits<enum_field<ET, IT> > : public enum_traits<ET> {};

    template<class EnumType, class IntType1, class IntType2>
    inline bool operator== (enum_field<EnumType,IntType1> a, enum_field<EnumType,IntType2> b)
    {
        return EnumType(a) == EnumType(b);
    }

    template<class EnumType, class IntType1, class IntType2>
    inline bool operator!= (enum_field<EnumType,IntType1> a, enum_field<EnumType,IntType2> b)
    {
        return EnumType(a) != EnumType(b);
    }

    namespace enums {}
}

/*
 * Templates for access to enum and bitfield traits.
 */

DFHACK_EXPORT std::string join_strings(const std::string &separator, const std::vector<std::string> &items);

namespace DFHack {
    /*
     * Enum trait tools.
     */
    template <typename T>
    concept Enumeration = std::is_enum_v<T>;

    /**
     * Return the next item in the enum, wrapping to the first one at the end if 'wrap' is true (otherwise an invalid item).
     */
    template<Enumeration T>
    inline T next_enum_item(T v, bool wrap = true)
    {
        using traits = df::enum_traits<T>;

        if constexpr (traits::is_complex) {
            const auto& complex = traits::complex;
            const auto it = complex.value_index_map.find(v);
            if (it != complex.value_index_map.end())
            {
                if (!wrap && it->second + 1 == complex.size())
                {
                    return T(traits::last_item_value + 1);
                }
                size_t next_index = (it->second + 1) % complex.size();
                return T(complex.index_value_map[next_index]);
            }
            else
                return T(traits::last_item_value + 1);
        }
        else
        {
            typedef typename traits::base_type base_type;
            base_type iv = base_type(v);
            if (iv < traits::last_item_value)
            {
                return T(iv + 1);
            }
            else
            {
                if (wrap)
                    return traits::first_item;
                else
                    return T(traits::last_item_value + 1);
            }
        }
    }

    /**
     * Check if the value is valid for its enum type.
     */
    template<Enumeration T>
    inline bool is_valid_enum_item(T v)
    {
        if constexpr (df::enum_traits<T>::is_complex) {
            const auto& complex = df::enum_traits<T>::complex;
            return complex.value_index_map.find(v) != complex.value_index_map.end();
        }
        else
        {
            return df::enum_traits<T>::is_valid(v);
        }
    }

    /**
     * Return the enum item key string pointer, or NULL if none.
     */
    template<Enumeration T>
    inline const char * enum_item_raw_key(T val) {
        using traits = df::enum_traits<T>;
        if constexpr (traits::is_complex)
        {
            const auto& value_index_map = traits::complex.value_index_map;
            auto it = value_index_map.find(val);
            if (it != value_index_map.end())
                return traits::key_table[it->second];
            else
                return NULL;
        }
        else
        {
            return traits::is_valid(val) ? traits::key_table[(short)val - traits::first_item_value] : NULL;
        }
    }

    /**
     * Return the enum item key string pointer, or "?" if none.
     */
    template<Enumeration T>
    inline const char *enum_item_key_str(T val) {
        return ifnull(enum_item_raw_key(val), "?");
    }

    template<class BaseType>
    std::string format_key(const char *keyname, BaseType val) {
        if (keyname) return std::string(keyname);
        std::stringstream ss; ss << "?" << val << "?"; return ss.str();
    }

    /**
     * Return the enum item key string, or ?123? (using the numeric value) if unknown.
     */
    template<Enumeration T>
    inline std::string enum_item_key(T val) {
        using base_type = typename df::enum_traits<T>::base_type;
        return format_key<base_type>(enum_item_raw_key(val), base_type(val));
    }

    DFHACK_EXPORT int findEnumItem(const std::string &name, int size, const char *const *items);

    /**
     * Find an enum item by key string. Returns success code.
     */
    template<Enumeration T>
    inline bool find_enum_item(T *var, const std::string &name) {
        using traits = df::enum_traits<T>;
        int size = traits::last_item_value-traits::first_item_value+1;
        int idx = findEnumItem(name, size, traits::key_table);
        if (idx < 0) return false;
        *var = T(traits::first_item_value+idx);
        return true;
    }

    /*
     * Bitfield tools.
     */

    DFHACK_EXPORT bool findBitfieldField(unsigned *idx, const std::string &name,
                                         unsigned size, const bitfield_item_info *items);
    DFHACK_EXPORT void setBitfieldField(void *p, unsigned idx, unsigned size, int value);
    DFHACK_EXPORT int getBitfieldField(const void *p, unsigned idx, unsigned size);

    /**
     * Find a bitfield item by key string. Returns success code.
     */
    template<class T>
    inline bool find_bitfield_field(unsigned *idx, const std::string &name, const T* = NULL) {
        typedef df::bitfield_traits<T> traits;
        return findBitfieldField(&idx, name, traits::bit_count, traits::bits);
    }

    /**
     * Find a bitfield item by key and set its value. Returns success code.
     */
    template<class T>
    inline bool set_bitfield_field(T *bitfield, const std::string &name, int value)
    {
        typedef df::bitfield_traits<T> traits;
        unsigned idx;
        if (!findBitfieldField(&idx, name, traits::bit_count, traits::bits)) return false;
        setBitfieldField(&bitfield->whole, idx, traits::bits[idx].size, value);
        return true;
    }

    /**
     * Find a bitfield item by key and retrieve its value. Returns success code.
     */
    template<class T>
    inline bool get_bitfield_field(int *value, const T &bitfield, const std::string &name)
    {
        typedef df::bitfield_traits<T> traits;
        unsigned idx;
        if (!findBitfieldField(&idx, name, traits::bit_count, traits::bits)) return false;
        *value = getBitfieldField(&bitfield.whole, idx, traits::bits[idx].size);
        return true;
    }

    DFHACK_EXPORT void bitfieldToString(std::vector<std::string> *pvec, const void *p,
                                        unsigned size, const bitfield_item_info *items);

    /**
     * Represent bitfield bits as strings in a vector.
     */
    template<class T>
    inline void bitfield_to_string(std::vector<std::string> *pvec, const T &val) {
        typedef df::bitfield_traits<T> traits;
        bitfieldToString(pvec, &val.whole, traits::bit_count, traits::bits);
    }

    /**
     * Represent bitfield bits as a string, using sep as join separator.
     */
    template<class T>
    inline std::string bitfield_to_string(const T &val, const std::string &sep = " ") {
        std::vector<std::string> tmp;
        bitfield_to_string<T>(&tmp, val);
        return join_strings(sep, tmp);
    }

    /*
     * BitArray tools
     */

    /**
     * Find a flag array item by key string. Returns success code.
     */
    template<class T>
    inline bool find_flagarray_field(unsigned *idx, const std::string &name, const BitArray<T>*) {
        T tmp;
        if (!find_enum_item(&tmp, name) || tmp < 0) return false;
        *idx = unsigned(tmp);
        return true;
    }

    /**
     * Find a flag array item by key and set its value. Returns success code.
     */
    template<class T>
    inline bool set_flagarray_field(BitArray<T> *bitfield, const std::string &name, int value)
    {
        T tmp;
        if (!find_enum_item(&tmp, name) || tmp < 0) return false;
        bitfield->set(tmp, value!=0);
        return true;
    }

    /**
     * Find a flag array item by key and retrieve its value. Returns success code.
     */
    template<class T>
    inline bool get_flagarray_field(int *value, const BitArray<T> &bitfield, const std::string &name)
    {
        T tmp;
        if (!find_enum_item(&tmp, name) || tmp < 0) return false;
        *value = (bitfield->is_set(tmp) ? 1 : 0);
        return true;
    }

    DFHACK_EXPORT void flagarrayToString(std::vector<std::string> *pvec, const void *p,
                                         int bytes, int base, int size, const char *const *items);

    /**
     * Represent flag array bits as strings in a vector.
     */
    template<class T>
    inline void flagarray_to_string(std::vector<std::string> *pvec, const BitArray<T> &val) {
        typedef df::enum_traits<T> traits;
        int size = traits::last_item_value-traits::first_item_value+1;
        flagarrayToString(pvec, val.bits, val.size,
                          (int)traits::first_item_value, size, traits::key_table);
    }

    /**
     * Represent flag array bits as a string, using sep as join separator.
     */
    template<class T>
    inline std::string flagarray_to_string(const BitArray<T> &val, const std::string &sep = " ") {
        std::vector<std::string> tmp;
        flagarray_to_string<T>(&tmp, val);
        return join_strings(sep, tmp);
    }

    /**
     * Finds the tag field for a given union field.
     *
     * The returned tag field is a primitive enum field or nullptr.
     *
     * If the union field is a container type, the returned tag field is
     * a container of primitive enum types.
     *
     * As a special case, a container-type union can have a tag field that is
     * a bit vector if it has exactly two members.
     */
    DFHACK_EXPORT const struct_field_info *find_union_tag(const struct_identity_base *structure, const struct_field_info *union_field);
}

#define ENUM_ATTR(enum,attr,val) (df::enum_traits<df::enum>::attrs(val).attr)
#define ENUM_ATTR_STR(enum,attr,val) DFHack::ifnull(ENUM_ATTR(enum,attr,val),"?")
#define ENUM_KEY_STR(enum,val) (DFHack::enum_item_key<df::enum>(val))
#define ENUM_FIRST_ITEM(enum) (df::enum_traits<df::enum>::first_item)
#define ENUM_LAST_ITEM(enum) (df::enum_traits<df::enum>::last_item)

#define ENUM_NEXT_ITEM(enum,val) \
    (DFHack::next_enum_item<df::enum>(val))
#define FOR_ENUM_ITEMS(enum,iter) \
    for(df::enum iter = ENUM_FIRST_ITEM(enum); DFHack::is_valid_enum_item(iter); iter = DFHack::next_enum_item(iter, false))

/*
 * Include mandatory generated headers.
 */

// Global object pointers
#include "df/global_objects.h"

#define DF_GLOBAL_VALUE(name,defval) (df::global::name ? *df::global::name : defval)
#define DF_GLOBAL_FIELD(name,fname,defval) (df::global::name ? df::global::name->fname : defval)

// A couple of headers that have to be included at once
#include "df/coord2d.h"
#include "df/coord.h"

template <>
struct std::hash<df::coord> {
    std::size_t operator()(const df::coord& c) const {
        return c();
    }
};
