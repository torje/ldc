#include "llvm/DerivedTypes.h"

#include "aggregate.h"
#include "declaration.h"
#include "dsymbol.h"
#include "mtype.h"

#include "gen/irstate.h"
#include "gen/logger.h"
#include "gen/tollvm.h"
#include "gen/utils.h"
#include "gen/llvmhelpers.h"
#include "ir/irtypeclass.h"

//////////////////////////////////////////////////////////////////////////////

extern size_t add_zeros(std::vector<llvm::Type*>& defaultTypes, size_t diff);
extern bool var_offset_sort_cb(const VarDeclaration* v1, const VarDeclaration* v2);

//////////////////////////////////////////////////////////////////////////////

IrTypeClass::IrTypeClass(ClassDeclaration* cd)
:   IrTypeAggr(cd),
    cd(cd),
    tc((TypeClass*)cd->type)
{
    std::string vtbl_name(cd->toPrettyChars());
    vtbl_name.append(".__vtbl");
    vtbl_type = LLStructType::create(gIR->context(), vtbl_name);
    vtbl_size = cd->vtbl.dim;
    num_interface_vtbls = 0;
}

//////////////////////////////////////////////////////////////////////////////

void IrTypeClass::addBaseClassData(
    std::vector<llvm::Type *> & defaultTypes,
    ClassDeclaration * base,
    size_t & offset,
    size_t & field_index)
{
    if (base->baseClass)
    {
        addBaseClassData(defaultTypes, base->baseClass, offset, field_index);
    }

    // FIXME: merge code with structs in IrTypeAggr

    // mirror the sd->fields array but only fill in contributors
    size_t n = base->fields.dim;
    LLSmallVector<VarDeclaration*, 16> data(n, NULL);
    default_fields.reserve(n);

    // first fill in the fields with explicit initializers
    VarDeclarationIter field_it(base->fields);
    for (; field_it.more(); field_it.next())
    {
        // init is !null for explicit inits
        if (field_it->init != NULL)
        {
            IF_LOG Logger::println("adding explicit initializer for struct field %s",
                field_it->toChars());

            data[field_it.index] = *field_it;

            size_t f_begin = field_it->offset;
            size_t f_end = f_begin + field_it->type->size();

            // make sure there is no overlap
            for (size_t i = 0; i < field_it.index; i++)
            {
                if (data[i] != NULL)
                {
                    VarDeclaration* vd = data[i];
                    size_t v_begin = vd->offset;
                    size_t v_end = v_begin + vd->type->size();

                    if (v_begin >= f_end || v_end <= f_begin)
                        continue;

                    base->error(vd->loc, "has overlapping initialization for %s and %s",
                        field_it->toChars(), vd->toChars());
                }
            }
        }
    }

    if (global.errors)
    {
        fatal();
    }

    // fill in default initializers
    field_it = VarDeclarationIter(base->fields);
    for (;field_it.more(); field_it.next())
    {
        if (data[field_it.index])
            continue;

        size_t f_begin = field_it->offset;
        size_t f_end = f_begin + field_it->type->size();

        // make sure it doesn't overlap anything explicit
        bool overlaps = false;
        for (size_t i = 0; i < n; i++)
        {
            if (data[i])
            {
                size_t v_begin = data[i]->offset;
                size_t v_end = v_begin + data[i]->type->size();

                if (v_begin >= f_end || v_end <= f_begin)
                    continue;

                overlaps = true;
                break;
            }
        }

        // if no overlap was found, add the default initializer
        if (!overlaps)
        {
            IF_LOG Logger::println("adding default initializer for struct field %s",
                field_it->toChars());
            data[field_it.index] = *field_it;
        }
    }

    // ok. now we can build a list of llvm types. and make sure zeros are inserted if necessary.

    // first we sort the list by offset
    std::sort(data.begin(), data.end(), var_offset_sort_cb);

    // add types to list
    for (size_t i = 0; i < n; i++)
    {
        VarDeclaration* vd = data[i];

        if (vd == NULL)
            continue;

        assert(vd->offset >= offset && "it's a bug... most likely DMD bug 2481");

        // add to default field list
        if (cd == base)
            default_fields.push_back(vd);

        // get next aligned offset for this type
        size_t alignedoffset = realignOffset(offset, vd->type);

        // insert explicit padding?
        if (alignedoffset < vd->offset)
        {
            field_index += add_zeros(defaultTypes, vd->offset - alignedoffset);
        }

        // add default type
        defaultTypes.push_back(DtoType(vd->type));

        // advance offset to right past this field
        offset = vd->offset + vd->type->size();

        // create ir field
        vd->aggrIndex = (unsigned)field_index;
        ++field_index;
    }

    // any interface implementations?
    if (base->vtblInterfaces && base->vtblInterfaces->dim > 0)
    {
        bool new_instances = (base == cd);

        ArrayIter<BaseClass> it2(*base->vtblInterfaces);

        VarDeclarationIter interfaces_idx(ClassDeclaration::classinfo->fields, 3);
	Type* first = interfaces_idx->type->nextOf()->pointerTo();

        // align offset
        offset = (offset + PTRSIZE - 1) & ~(PTRSIZE - 1);

        for (; !it2.done(); it2.next())
        {
            BaseClass* b = it2.get();
            IF_LOG Logger::println("Adding interface vtbl for %s", b->base->toPrettyChars());

            FuncDeclarations arr;
            b->fillVtbl(cd, &arr, new_instances);

            llvm::Type* ivtbl_type = llvm::StructType::get(gIR->context(), buildVtblType(first, &arr));
            defaultTypes.push_back(llvm::PointerType::get(ivtbl_type, 0));

            offset += PTRSIZE;

            // add to the interface map
            addInterfaceToMap(b->base, field_index);
            field_index++;

            // inc count
            num_interface_vtbls++;
        }
    }

#if 0
    // tail padding?
    if (offset < base->structsize)
    {
        field_index += add_zeros(defaultTypes, base->structsize - offset);
        offset = base->structsize;
    }
#endif
}

//////////////////////////////////////////////////////////////////////////////

llvm::Type* IrTypeClass::buildType()
{
    IF_LOG Logger::println("Building class type %s @ %s", cd->toPrettyChars(), cd->loc.toChars());
    LOG_SCOPE;
    IF_LOG Logger::println("Instance size: %u", cd->structsize);

    // find the fields that contribute to the default initializer.
    // these will define the default type.

    std::vector<llvm::Type*> defaultTypes;
    defaultTypes.reserve(32);

    // add vtbl
    defaultTypes.push_back(llvm::PointerType::get(vtbl_type, 0));

    // interfaces are just a vtable
    if (cd->isInterfaceDeclaration())
    {
        num_interface_vtbls = cd->vtblInterfaces ? cd->vtblInterfaces->dim : 0;
    }
    // classes have monitor and fields
    else
    {
        // add monitor
        defaultTypes.push_back(llvm::PointerType::get(llvm::Type::getInt8Ty(gIR->context()), 0));

        // we start right after the vtbl and monitor
        size_t offset = PTRSIZE * 2;
        size_t field_index = 2;

        // add data members recursively
        addBaseClassData(defaultTypes, cd, offset, field_index);

#if 1
        // tail padding?
        if (offset < cd->structsize)
        {
            field_index += add_zeros(defaultTypes, cd->structsize - offset);
            offset = cd->structsize;
        }
#endif
    }

    // errors are fatal during codegen
    if (global.errors)
        fatal();

    // set struct body
    isaStruct(type)->setBody(defaultTypes, false);

    // VTBL

    // set vtbl type body
    vtbl_type->setBody(buildVtblType(ClassDeclaration::classinfo->type, &cd->vtbl));

    IF_LOG Logger::cout() << "class type: " << *type << std::endl;

    return get();
}

//////////////////////////////////////////////////////////////////////////////

std::vector<llvm::Type*> IrTypeClass::buildVtblType(Type* first, Array* vtbl_array)
{
    IF_LOG Logger::println("Building vtbl type for class %s", cd->toPrettyChars());
    LOG_SCOPE;

    std::vector<llvm::Type*> types;
    types.reserve(vtbl_array->dim);

    // first comes the classinfo
    types.push_back(DtoType(first));

    // then come the functions
    ArrayIter<Dsymbol> it(*vtbl_array);
    it.index = 1;

    for (; !it.done(); it.next())
    {
        Dsymbol* dsym = it.get();
        if (dsym == NULL)
        {
            // FIXME
            // why is this null?
            // happens for mini/s.d
            types.push_back(getVoidPtrType());
            continue;
        }

        FuncDeclaration* fd = dsym->isFuncDeclaration();
        assert(fd && "invalid vtbl entry");

        IF_LOG Logger::println("Adding type of %s", fd->toPrettyChars());

        types.push_back(DtoType(fd->type->pointerTo()));
    }

    return types;
}

//////////////////////////////////////////////////////////////////////////////

llvm::Type * IrTypeClass::get()
{
    return llvm::PointerType::get(type, 0);
}

//////////////////////////////////////////////////////////////////////////////

size_t IrTypeClass::getInterfaceIndex(ClassDeclaration * inter)
{
    ClassIndexMap::iterator it = interfaceMap.find(inter);
    if (it == interfaceMap.end())
        return ~0;
    return it->second;
}

//////////////////////////////////////////////////////////////////////////////

void IrTypeClass::addInterfaceToMap(ClassDeclaration * inter, size_t index)
{
    // don't duplicate work or overwrite indices
    if (interfaceMap.find(inter) != interfaceMap.end())
        return;

    // add this interface
    interfaceMap.insert(std::make_pair(inter, index));

    // add the direct base interfaces recursively - they
    // are accessed through the same index
    if (inter->interfaces_dim > 0)
    {
        BaseClass* b = inter->interfaces[0];
        addInterfaceToMap(b->base, index);
    }
}

//////////////////////////////////////////////////////////////////////////////
