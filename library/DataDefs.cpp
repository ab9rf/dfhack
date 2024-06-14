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

#include "Internal.h"

#include <string>
#include <vector>
#include <map>

#include "MemAccess.h"
#include "Core.h"
#include "VersionInfo.h"
// must be last due to MS stupidity
#include "DataDefs.h"
#include "DataIdentity.h"
#include "VTableInterpose.h"
#include "Error.h"

#include "MiscUtils.h"

using namespace DFHack;

// constinit guarantees this variable is initialized _before_ dynamic initialization begins
// and so this will have a defined value even during dynamic initialization
// set to false during static initialization, set to true once identity system is
// fully initialized
constinit static bool identities_initialized = false;

void *type_identity::do_allocate_pod() const {
    size_t sz = byte_size();
    void *p = malloc(sz);
    memset(p, 0, sz);
    return p;
}

void type_identity::do_copy_pod(void *tgt, const void *src) const {
    memmove(tgt, src, byte_size());
};

bool type_identity::do_destroy_pod(void *obj) const {
    free(obj);
    return true;
}

void *type_identity::allocate() const {
    if (can_allocate())
        return do_allocate();
    else
        return NULL;
}

bool type_identity::copy(void *tgt, const void *src) const {
    if (can_allocate() && tgt && src)
        return do_copy(tgt, src);
    else
        return false;
}

bool type_identity::destroy(void *obj) const {
    if (can_allocate() && obj)
        return do_destroy(obj);
    else
        return false;
}

struct identity_hashcomp
{
    using T = const DFHack::type_identity*;

    std::size_t operator()(const T t) const noexcept
    {
        return t->get_typeid().hash_code();
    }
    bool operator() (const T& lhs, const T& rhs) const
    {
        return (*lhs) == (*rhs);
    }
};

static std::unordered_map<const type_identity*, std::unique_ptr<const type_identity>, identity_hashcomp, identity_hashcomp> canonical_map{};

const type_identity* type_identity::canonicalize() const
{
    if (this->canon)
        return this->canon;

    auto item = canonical_map.find(this);
    if (item == canonical_map.end())
    {
        auto copy = this->clone();
        canon = copy.get();
        canon->canon = canon;
        canonical_map[canon] = std::move(copy);
    }
    else
    {
        canon = item->second.get();
    }

    assert(*this == *canon);
    return canon;
}

void *enum_identity::do_allocate() const {
    size_t sz = byte_size();
    void *p = malloc(sz);
    memcpy(p, &first_item_value, std::min(sz, sizeof(int64_t)));
    return p;
}

/* The order of global object constructor calls is
 * undefined between compilation units. Therefore,
 * this list has to be plain data, so that it gets
 * initialized by the loader in the initial mmap.
 */
compound_identity *compound_identity::list = NULL;
std::vector<const compound_identity*> compound_identity::top_scope;

compound_identity::compound_identity(const std::type_index id, size_t size, TAllocateFn alloc,
    const compound_identity* scope_parent, const std::string& dfhack_name)
    : constructed_identity(id, size, alloc), dfhack_name(dfhack_name), scope_parent(const_cast<compound_identity*>(scope_parent))
{
    next = list; list = this;
    if (identities_initialized) {
        // FIXME: crashes when initializing a struct_identity coming from a plugin, for unclear reasons
        // Init(&Core::getInstance());
    }
}

void compound_identity::doInit(Core *)
{
    auto canon = dynamic_cast<const compound_identity*>(canonicalize());
    assert(canon != nullptr);
    assert(*canon == *this);

    auto type = this;
    auto cmp_fn = [type](auto t) { return *t == *type; };

    if (scope_parent)
    {
        if (std::ranges::count_if(scope_parent->scope_children, cmp_fn) > 0)
            std::cerr << "duplicate push to scope_children : " << type->get_typeid().name() << std::endl;
        else
            scope_parent->scope_children.push_back(type);
    }
    else
    {
        if (std::ranges::count_if(top_scope, cmp_fn) > 0)
            std::cerr << "duplicate push to top_scope : " << type->get_typeid().name() << std::endl;
        else
            top_scope.push_back(type);
    }
}

const std::string compound_identity::getFullName() const
{
    if (scope_parent)
        return scope_parent->getFullName() + "." + getName();
    else
        return getName();
}


constinit static std::mutex *known_mutex = nullptr;

void compound_identity::Init(Core *core)
{
    if (!known_mutex)
        known_mutex = new std::mutex();

    // This cannot be done in the constructors, because
    // they are called in an undefined order.
    while (list != nullptr)
    {
        auto p = list;
        list = list->next;
        p->doInit(core);
    }

    assert(list == nullptr);
    identities_initialized = true;
}

bitfield_identity::bitfield_identity(const std::type_info& id, size_t size,
                                     const compound_identity *scope_parent, const char *dfhack_name,
                                     bitfield_item_info_int bits)
    : compound_identity(id, size, NULL, scope_parent, dfhack_name), bits(bits)
{
}

enum_identity::enum_identity(const std::type_index id, size_t size,
    const compound_identity *scope_parent, const char *dfhack_name,
    const type_identity *base_type,
                             int64_t first_item_value, int64_t last_item_value,
                             const char *const *keys_,
                             const ComplexData *complex_,
                             const void *attrs, const struct_identity *attr_type)
    : compound_identity(id, size, NULL, scope_parent, dfhack_name),
    keys({}),
      first_item_value(first_item_value), last_item_value(last_item_value),
    base_type(base_type), attrs(attrs), attr_type(attr_type)
{
    if (complex_ != nullptr) {
        complex = *complex_;
        count = complex->size();
        last_item_value = complex->index_value_map.back();
    }
    else {
        count = int(last_item_value-first_item_value+1);
    }
    std::transform(keys_, keys_ + count, std::back_inserter(keys),
        [](const char* const s) { return s != nullptr ? std::string{ s } : std::string{}; });
}

enum_identity::enum_identity(const std::type_index id, size_t size,
    const compound_identity* scope_parent, const std::string& dfhack_name,
    const type_identity* base_type,
    int64_t first_item_value, int64_t last_item_value,
    std::vector<std::string> keys,
    std::optional<ComplexData> complex,
    const void* attrs, const struct_identity* attr_type, int count)
    : compound_identity(id, size, NULL, scope_parent, dfhack_name),
    keys(keys), first_item_value(first_item_value), last_item_value(last_item_value),
    base_type(base_type), attrs(attrs), attr_type(attr_type), complex(complex), count(count)
{}

    enum_identity::enum_identity(const enum_identity *base_enum, const type_identity *override_base_type)
    : enum_identity(base_enum->get_typeid(), override_base_type->byte_size(), base_enum->getScopeParent(),
                    base_enum->getName(), override_base_type, base_enum->first_item_value,
                    base_enum->last_item_value, base_enum->keys, base_enum->complex,
                    base_enum->attrs, base_enum->attr_type, base_enum->count)
{
}

enum_identity::ComplexData::ComplexData(std::initializer_list<int64_t> values)
{
    size_t i = 0;
    for (int64_t value : values) {
        value_index_map[value] = i;
        index_value_map.push_back(value);
        i++;
    }
}

struct_identity::struct_identity(const std::type_info& id, size_t size, TAllocateFn alloc,
    const compound_identity *scope_parent, const char *dfhack_name,
    const struct_identity *parent, const struct_field_info *fields_)
    : compound_identity(id, size, alloc, scope_parent, dfhack_name),
    parent(const_cast<struct_identity*>(parent)), has_children(false), fields({})
{
    for (; fields_ && fields_->mode != struct_field_info::Mode::END; fields_++) {
        fields.push_back(struct_field_info_int{ *fields_ });
    }
}

void struct_identity::doInit(Core *core)
{
    compound_identity::doInit(core);

    if (parent) {
        parent->children.push_back(this);
        parent->has_children = true;
    }
}

bool struct_identity::is_subclass(const struct_identity *actual) const
{
    if (!has_children && actual != this)
        return false;

    for (; actual; actual = actual->getParent())
        if (actual == this) return true;

    return false;
}

const std::string pointer_identity::getFullName() const
{
    return (target ? target->getFullName() : std::string("void")) + "*";
}

const std::string container_identity::getFullName(const type_identity *item) const
{
    return '<' + (item ? item->getFullName() : std::string("void")) + '>';
}

const std::string ptr_container_identity::getFullName(const type_identity *item) const
{
    return '<' + (item ? item->getFullName() : std::string("void")) + std::string("*>");
}

const std::string bit_container_identity::getFullName(const type_identity *) const
{
    return "<bool>";
}

const std::string df::buffer_container_identity::getFullName(const type_identity *item) const
{
    return (item ? item->getFullName() : std::string("void")) +
           (size > 0 ? stl_sprintf("[%d]", size) : std::string("[]"));
}

union_identity::union_identity(const std::type_info& id, size_t size, const TAllocateFn alloc,
        const compound_identity *scope_parent, const char *dfhack_name,
        const struct_identity *parent, const struct_field_info *fields)
    : struct_identity(id, size, alloc, scope_parent, dfhack_name, parent, fields)
{
}

virtual_identity::virtual_identity(const std::type_info& id, size_t size, const TAllocateFn alloc,
                                   const char *dfhack_name, const char *original_name,
                                   const virtual_identity *parent, const struct_field_info *fields,
                                   bool is_plugin)
    : struct_identity(id, size, alloc, NULL, dfhack_name, parent, fields), original_name(original_name ? std::string{ original_name } : std::string{}),
      vtable_ptr(NULL), is_plugin(is_plugin)
{
    // Plugins are initialized after Init was called, so they need to be added to the name table here
    if (is_plugin)
    {
        doInit(&Core::getInstance());
    }
}

/* Vtable name to identity lookup. */
static std::map<std::string, virtual_identity*> name_lookup;

/* Vtable pointer to identity lookup. */
std::map<void*, virtual_identity*> virtual_identity::known;

virtual_identity::~virtual_identity()
{
    // Remove interpose entries, so that they don't try accessing this object later
    for (auto it = interpose_list.begin(); it != interpose_list.end(); ++it)
        if (it->second)
            it->second->on_host_delete(this);
    interpose_list.clear();

    // Remove global lookup table entries if we're from a plugin
    if (is_plugin)
    {
        name_lookup.erase(getOriginalName());

        if (vtable_ptr)
            known.erase(vtable_ptr);
    }
}

void virtual_identity::doInit(Core *core)
{
    struct_identity::doInit(core);

    auto vtname = getOriginalName();
    name_lookup[vtname] = this;

    vtable_ptr = core->vinfo->getVTable(vtname);
    if (vtable_ptr)
        known[vtable_ptr] = this;
}

virtual_identity *virtual_identity::find(const std::string &name)
{
    auto name_it = name_lookup.find(name);

    return (name_it != name_lookup.end()) ? name_it->second : NULL;
}

virtual_identity *virtual_identity::get(virtual_ptr instance_ptr)
{
    if (!instance_ptr) return NULL;

    return find(get_vtable(instance_ptr));
}

virtual_identity *virtual_identity::find(void *vtable)
{
    if (!vtable)
        return NULL;

    // Actually, a reader/writer lock would be sufficient,
    // since the table is only written once per class.
    std::lock_guard<std::mutex> lock(*known_mutex);

    std::map<void*, virtual_identity*>::iterator it = known.find(vtable);

    if (it != known.end())
        return it->second;

    // If using a reader/writer lock, re-grab as write here, and recheck
    Core &core = Core::getInstance();
    std::string name = core.p->doReadClassName(vtable);

    auto name_it = name_lookup.find(name);
    if (name_it != name_lookup.end()) {
        virtual_identity *p = name_it->second;

        if (p->vtable_ptr && p->vtable_ptr != vtable) {
            std::cerr << "Conflicting vtable ptr for class '" << p->getName()
                      << "': found 0x" << std::hex << uintptr_t(vtable)
                      << ", previous 0x" << uintptr_t(p->vtable_ptr) << std::dec << std::endl;
            abort();
        } else if (!p->vtable_ptr) {
            uintptr_t pv = uintptr_t(vtable);
            pv -= Core::getInstance().vinfo->getRebaseDelta();
            std::cerr << "<vtable-address name='" << p->getOriginalName() << "' value='0x"
                      << std::hex << pv << std::dec << "'/>" << std::endl;
        }

        known[vtable] = p;
        p->vtable_ptr = vtable;
        return p;
    }

    std::cerr << "Class not in symbols.xml: '" << name << "': vtable = 0x"
              << std::hex << uintptr_t(vtable) << std::dec << std::endl;

    known[vtable] = NULL;
    return NULL;
}

void virtual_identity::adjust_vtable(virtual_ptr obj, const virtual_identity *main) const
{
    if (vtable_ptr) {
        *(void**)obj = vtable_ptr;
        return;
    }

    if (main && main != this && is_subclass(main))
        return;

    std::cerr << "Attempt to create class '" << getName() << "' without known vtable." << std::endl;
    throw DFHack::Error::VTableMissing(getName().c_str());
}

virtual_ptr virtual_identity::clone(virtual_ptr obj)
{
    virtual_identity *id = get(obj);
    if (!id) return NULL;
    virtual_ptr copy = id->instantiate();
    if (!copy) return NULL;
    id->do_copy(copy, obj);
    return copy;
}

bool DFHack::findBitfieldField(unsigned *idx, const std::string &name,
                               unsigned size, const bitfield_item_info *items)
{
    for (unsigned i = 0; i < size; i++) {
        if (items[i].name && items[i].name == name)
        {
            *idx = i;
            return true;
        }
    }

    return false;
}

void DFHack::setBitfieldField(void *p, unsigned idx, unsigned size, int value)
{
    uint8_t *data = ((uint8_t*)p) + (idx/8);
    unsigned shift = idx%8;
    uint32_t mask = ((1<<size)-1) << shift;
    uint32_t vmask = ((value << shift) & mask);

#define ACCESS(type) *(type*)data = type((*(type*)data & ~mask) | vmask)

    if (!(mask & ~0xFFU)) ACCESS(uint8_t);
    else if (!(mask & ~0xFFFFU)) ACCESS(uint16_t);
    else ACCESS(uint32_t);

#undef ACCESS
}

int DFHack::getBitfieldField(const void *p, unsigned idx, unsigned size)
{
    const uint8_t *data = ((const uint8_t*)p) + (idx/8);
    unsigned shift = idx%8;
    uint32_t mask = ((1<<size)-1) << shift;

#define ACCESS(type) return int((*(type*)data & mask) >> shift)

    if (!(mask & ~0xFFU)) ACCESS(uint8_t);
    else if (!(mask & ~0xFFFFU)) ACCESS(uint16_t);
    else ACCESS(uint32_t);

#undef ACCESS
}

void DFHack::bitfieldToString(std::vector<std::string> *pvec, const void *p,
                              unsigned size, const bitfield_item_info *items)
{
    for (unsigned i = 0; i < size; i++) {
        int value = getBitfieldField(p, i, std::max(1,items[i].size));

        if (value) {
            std::string name = format_key(items[i].name, i);

            if (items[i].size > 1)
                name += stl_sprintf("=%u", value);

            pvec->push_back(name);
        }

        if (items[i].size > 1)
            i += items[i].size-1;
    }
}

int DFHack::findEnumItem(const std::string &name, int size, const char *const *items)
{
    for (int i = 0; i < size; i++) {
        if (items[i] && items[i] == name)
            return i;
    }

    return -1;
}

void DFHack::flagarrayToString(std::vector<std::string> *pvec, const void *p,
                               int bytes, int base, int size, const char *const *items)
{
    for (int i = 0; i < bytes*8; i++) {
        int value = getBitfieldField(p, i, 1);

        if (value)
        {
            int ridx = int(i) - base;
            const char *name = (ridx >= 0 && ridx < size) ? items[ridx] : NULL;
            pvec->push_back(format_key(name, i));
        }
    }
}

static const std::optional<struct_field_info_int> find_union_tag_candidate(const struct_identity *structure, const struct_field_info_int& union_field)
{
    if (union_field.extra && !union_field.extra->union_tag_field.empty())
    {
        auto& defined_field_name = union_field.extra->union_tag_field;
        for (auto p = structure; p; p = p->getParent())
        {
            for (auto& field : p->getFields())
            {
                if (field.name == defined_field_name)
                {
                    return field;
                }
            }
        }

        return std::nullopt;
    }

    std::string name(union_field.name);
    if (name.length() >= 4 && name.substr(name.length() - 4) == "data")
    {
        name.erase(name.length() - 4, 4);
        name += "type";

        for (auto p = structure; p; p = p->getParent())
        {
            for (auto& field : p->getFields())
            {
                if (field.name == name)
                {
                    return field;
                }
            }
        }
    }

    return std::nullopt;
}

const std::optional<const struct_field_info_int> DFHack::find_union_tag(const struct_identity *structure, const struct_field_info_int& union_field)
{
    CHECK_NULL_POINTER(structure);
//    CHECK_NULL_POINTER(union_field);

    auto tag_candidate = find_union_tag_candidate(structure, union_field);

    if (!tag_candidate)
    {
        return std::nullopt;
    }

    if (union_field.mode == struct_field_info::SUBSTRUCT &&
            union_field.type &&
            union_field.type->type() == IDTYPE_UNION)
    {
        // union field

        if (tag_candidate->mode == struct_field_info::PRIMITIVE &&
                tag_candidate->type &&
                tag_candidate->type->type() == IDTYPE_ENUM)
        {
            return tag_candidate;
        }

        return std::nullopt;
    }

    if (union_field.mode != struct_field_info::CONTAINER ||
            !union_field.type ||
            union_field.type->type() != IDTYPE_CONTAINER)
    {
        // not a union field or a vector; bail
        return std::nullopt;
    }

    auto container_type = dynamic_cast<const container_identity *>(union_field.type);
    if (container_type->getFullName(nullptr) != "vector<void>" ||
            !container_type->getItemType() ||
            container_type->getItemType()->type() != IDTYPE_UNION)
    {
        // not a vector of unions
        return std::nullopt;
    }

    if (tag_candidate->mode != struct_field_info::CONTAINER ||
            !tag_candidate->type ||
            tag_candidate->type->type() != IDTYPE_CONTAINER)
    {
        // candidate is not a vector
        return std::nullopt;
    }

    auto tag_container_type = dynamic_cast<const container_identity *>(tag_candidate->type);
    if (tag_container_type->getFullName(nullptr) == "vector<void>" &&
            tag_container_type->getItemType() &&
            tag_container_type->getItemType()->type() == IDTYPE_ENUM)
    {
        return tag_candidate;
    }

    auto union_fields = dynamic_cast<const struct_identity*>(union_field.type)->getFields();
    if (tag_container_type->getFullName() == "vector<bool>" &&
            union_fields.size() == 2)
    {
        return tag_candidate;
    }

    return std::nullopt;
}
