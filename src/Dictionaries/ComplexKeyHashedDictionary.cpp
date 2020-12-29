#include "ComplexKeyHashedDictionary.h"
#include <ext/map.h>
#include <ext/range.h>
#include <Columns/ColumnsNumber.h>
#include <Functions/FunctionHelpers.h>
#include "DictionaryBlockInputStream.h"
#include "DictionaryFactory.h"

namespace DB
{
namespace ErrorCodes
{
    extern const int TYPE_MISMATCH;
    extern const int BAD_ARGUMENTS;
    extern const int DICTIONARY_IS_EMPTY;
}

ComplexKeyHashedDictionary::ComplexKeyHashedDictionary(
    const StorageID & dict_id_,
    const DictionaryStructure & dict_struct_,
    DictionarySourcePtr source_ptr_,
    const DictionaryLifetime dict_lifetime_,
    bool require_nonempty_,
    BlockPtr saved_block_)
    : IDictionaryBase(dict_id_)
    , dict_struct(dict_struct_)
    , source_ptr{std::move(source_ptr_)}
    , dict_lifetime(dict_lifetime_)
    , require_nonempty(require_nonempty_)
    , saved_block{std::move(saved_block_)}
{
    createAttributes();
    loadData();
    calculateBytesAllocated();
}

ColumnPtr ComplexKeyHashedDictionary::getColumn(
    const std::string & attribute_name,
    const DataTypePtr &,
    const Columns & key_columns,
    const DataTypes & key_types,
    const ColumnPtr default_untyped) const
{
    dict_struct.validateKeyTypes(key_types);

    ColumnPtr result;

    const auto & attribute = getAttribute(attribute_name);

    /// TODO: Check that attribute type is same as result type
    /// TODO: Check if const will work as expected
    
    auto size = key_columns.front()->size();

    auto type_call = [&](const auto &dictionary_attribute_type)
    {
        using Type = std::decay_t<decltype(dictionary_attribute_type)>;
        using AttributeType = typename Type::AttributeType;

        if constexpr (std::is_same_v<AttributeType, String>)
        {
            auto column_string = ColumnString::create();
            auto out = column_string.get();

            if (default_untyped != nullptr)
            {
                if (const auto default_col = checkAndGetColumn<ColumnString>(*default_untyped))
                {
                    getItemsImpl<StringRef, StringRef>(
                        attribute,
                        key_columns,
                        [&](const size_t, const StringRef value) { out->insertData(value.data, value.size); },
                        [&](const size_t row) { return default_col->getDataAt(row); });
                }
                else if (const auto default_col_const = checkAndGetColumnConst<ColumnString>(default_untyped.get()))
                {
                    const auto & def = default_col_const->template getValue<String>();

                    getItemsImpl<StringRef, StringRef>(
                        attribute,
                        key_columns,
                        [&](const size_t, const StringRef value) { out->insertData(value.data, value.size); },
                        [&](const size_t) { return def; });
                }
            }
            else
            {
                const auto & null_value = std::get<StringRef>(attribute.null_values);

                getItemsImpl<StringRef, StringRef>(
                    attribute,
                    key_columns,
                    [&](const size_t, const StringRef value) { out->insertData(value.data, value.size); },
                    [&](const size_t) { return null_value; });
            }

            result = std::move(column_string);
        }
        else
        {
            using ResultColumnType
                = std::conditional_t<IsDecimalNumber<AttributeType>, ColumnDecimal<AttributeType>, ColumnVector<AttributeType>>;
            using ResultColumnPtr = typename ResultColumnType::MutablePtr;

            ResultColumnPtr column;

            if constexpr (IsDecimalNumber<AttributeType>)
            {
                // auto scale = getDecimalScale(*attribute.type);
                column = ColumnDecimal<AttributeType>::create(size, 0);
            }
            else if constexpr (IsNumber<AttributeType>)
                column = ColumnVector<AttributeType>::create(size);
 
            auto& out = column->getData();

            if (default_untyped != nullptr)
            {
                if (const auto default_col = checkAndGetColumn<ResultColumnType>(*default_untyped))
                {
                    getItemsImpl<AttributeType, AttributeType>(
                        attribute,
                        key_columns,
                        [&](const size_t row, const auto value) { return out[row] = value; },
                        [&](const size_t row) { return default_col->getData()[row]; }
                    );
                }
                else if (const auto default_col_const = checkAndGetColumnConst<ResultColumnType>(default_untyped.get()))
                {
                    const auto & def = default_col_const->template getValue<AttributeType>();

                    getItemsImpl<AttributeType, AttributeType>(
                        attribute,
                        key_columns,
                        [&](const size_t row, const auto value) { return out[row] = value; },
                        [&](const size_t) { return def; }
                    );
                }
            }
            else
            {
                const auto null_value = std::get<AttributeType>(attribute.null_values);

                getItemsImpl<AttributeType, AttributeType>(
                    attribute,
                    key_columns,
                    [&](const size_t row, const auto value) { return out[row] = value; },
                    [&](const size_t) { return null_value; }
                );
            }

            result = std::move(column);
        }
    };

    callOnDictionaryAttributeType(attribute.type, type_call);
   
    return result;
}

ColumnUInt8::Ptr ComplexKeyHashedDictionary::has(const Columns & key_columns, const DataTypes & key_types) const
{
    dict_struct.validateKeyTypes(key_types);

    auto size = key_columns.front()->size();
    auto result = ColumnUInt8::create(size);
    auto& out = result->getData();

    const auto & attribute = attributes.front();

    auto type_call = [&](const auto & dictionary_attribute_type)
    {
        using Type = std::decay_t<decltype(dictionary_attribute_type)>;
        using AttributeType = typename Type::AttributeType;

        if constexpr (std::is_same_v<AttributeType, String>)
        {
            has<StringRef>(attribute, key_columns, out);
        }
        else
        {
            has<AttributeType>(attribute, key_columns, out);
        }
    };

    callOnDictionaryAttributeType(attribute.type, type_call);

    return result;
}

void ComplexKeyHashedDictionary::createAttributes()
{
    const auto size = dict_struct.attributes.size();
    attributes.reserve(size);

    for (const auto & attribute : dict_struct.attributes)
    {
        attribute_index_by_name.emplace(attribute.name, attributes.size());
        attributes.push_back(createAttributeWithType(attribute.underlying_type, attribute.null_value));

        if (attribute.hierarchical)
            throw Exception{full_name + ": hierarchical attributes not supported for dictionary of type " + getTypeName(),
                            ErrorCodes::TYPE_MISMATCH};
    }
}

void ComplexKeyHashedDictionary::blockToAttributes(const Block & block)
{
    /// created upfront to avoid excess allocations
    const auto keys_size = dict_struct.key->size();
    StringRefs keys(keys_size);

    const auto attributes_size = attributes.size();
    const auto rows = block.rows();
    element_count += rows;

    const auto key_column_ptrs = ext::map<Columns>(
        ext::range(0, keys_size), [&](const size_t attribute_idx) { return block.safeGetByPosition(attribute_idx).column; });

    const auto attribute_column_ptrs = ext::map<Columns>(ext::range(0, attributes_size), [&](const size_t attribute_idx)
    {
        return block.safeGetByPosition(keys_size + attribute_idx).column;
    });

    for (const auto row_idx : ext::range(0, rows))
    {
        /// calculate key once per row
        const auto key = placeKeysInPool(row_idx, key_column_ptrs, keys, keys_pool);

        auto should_rollback = false;

        for (const auto attribute_idx : ext::range(0, attributes_size))
        {
            const auto & attribute_column = *attribute_column_ptrs[attribute_idx];
            auto & attribute = attributes[attribute_idx];
            const auto inserted = setAttributeValue(attribute, key, attribute_column[row_idx]);
            if (!inserted)
                should_rollback = true;
        }

        /// @note on multiple equal keys the mapped value for the first one is stored
        if (should_rollback)
            keys_pool.rollback(key.size);
    }
}

void ComplexKeyHashedDictionary::updateData()
{
    /// created upfront to avoid excess allocations
    const auto keys_size = dict_struct.key->size();
    StringRefs keys(keys_size);

    const auto attributes_size = attributes.size();

    if (!saved_block || saved_block->rows() == 0)
    {
        auto stream = source_ptr->loadUpdatedAll();
        stream->readPrefix();

        while (const auto block = stream->read())
        {
            /// We are using this method to keep saved data if input stream consists of multiple blocks
            if (!saved_block)
                saved_block = std::make_shared<DB::Block>(block.cloneEmpty());
            for (const auto attribute_idx : ext::range(0, keys_size + attributes_size))
            {
                const IColumn & update_column = *block.getByPosition(attribute_idx).column.get();
                MutableColumnPtr saved_column = saved_block->getByPosition(attribute_idx).column->assumeMutable();
                saved_column->insertRangeFrom(update_column, 0, update_column.size());
            }
        }
        stream->readSuffix();
    }
    else
    {
        auto stream = source_ptr->loadUpdatedAll();

        stream->readPrefix();
        while (Block block = stream->read())
        {
            const auto saved_key_column_ptrs = ext::map<Columns>(
                ext::range(0, keys_size), [&](const size_t key_idx) { return saved_block->safeGetByPosition(key_idx).column; });

            const auto update_key_column_ptrs = ext::map<Columns>(
                ext::range(0, keys_size), [&](const size_t key_idx) { return block.safeGetByPosition(key_idx).column; });

            Arena temp_key_pool;
            ContainerType<std::vector<size_t>> update_key_hash;

            for (size_t i = 0; i < block.rows(); ++i)
            {
                const auto u_key = placeKeysInPool(i, update_key_column_ptrs, keys, temp_key_pool);
                update_key_hash[u_key].push_back(i);
            }

            const size_t rows = saved_block->rows();
            IColumn::Filter filter(rows);

            for (size_t i = 0; i < saved_block->rows(); ++i)
            {
                const auto s_key = placeKeysInPool(i, saved_key_column_ptrs, keys, temp_key_pool);
                auto * it = update_key_hash.find(s_key);
                if (it)
                    filter[i] = 0;
                else
                    filter[i] = 1;
            }

            auto block_columns = block.mutateColumns();
            for (const auto attribute_idx : ext::range(0, keys_size + attributes_size))
            {
                auto & column = saved_block->safeGetByPosition(attribute_idx).column;
                const auto & filtered_column = column->filter(filter, -1);

                block_columns[attribute_idx]->insertRangeFrom(*filtered_column.get(), 0, filtered_column->size());
            }

            saved_block->setColumns(std::move(block_columns));
        }
        stream->readSuffix();
    }

    if (saved_block)
        blockToAttributes(*saved_block.get());
}

void ComplexKeyHashedDictionary::loadData()
{
    if (!source_ptr->hasUpdateField())
    {
        auto stream = source_ptr->loadAll();
        stream->readPrefix();

        while (const auto block = stream->read())
            blockToAttributes(block);

        stream->readSuffix();
    }
    else
        updateData();

    if (require_nonempty && 0 == element_count)
        throw Exception{full_name + ": dictionary source is empty and 'require_nonempty' property is set.", ErrorCodes::DICTIONARY_IS_EMPTY};
}

template <typename T>
void ComplexKeyHashedDictionary::addAttributeSize(const Attribute & attribute)
{
    const auto & map_ref = std::get<ContainerType<T>>(attribute.maps);
    bytes_allocated += sizeof(ContainerType<T>) + map_ref.getBufferSizeInBytes();
    bucket_count = map_ref.getBufferSizeInCells();
}

template <>
void ComplexKeyHashedDictionary::addAttributeSize<String>(const Attribute & attribute)
{
    const auto & map_ref = std::get<ContainerType<StringRef>>(attribute.maps);
    bytes_allocated += sizeof(ContainerType<StringRef>) + map_ref.getBufferSizeInBytes();
    bucket_count = map_ref.getBufferSizeInCells();
    bytes_allocated += sizeof(Arena) + attribute.string_arena->size();
}

void ComplexKeyHashedDictionary::calculateBytesAllocated()
{
    bytes_allocated += attributes.size() * sizeof(attributes.front());

    for (const auto & attribute : attributes)
    {
        auto type_call = [&](const auto & dictionary_attribute_type)
        {
            using Type = std::decay_t<decltype(dictionary_attribute_type)>;
            using AttributeType = typename Type::AttributeType;

            addAttributeSize<AttributeType>(attribute);
        };

        callOnDictionaryAttributeType(attribute.type, type_call);
    }

    bytes_allocated += keys_pool.size();
}

template <typename T>
void ComplexKeyHashedDictionary::createAttributeImpl(Attribute & attribute, const Field & null_value)
{
    attribute.null_values = T(null_value.get<NearestFieldType<T>>());
    attribute.maps.emplace<ContainerType<T>>();
}

template <>
void ComplexKeyHashedDictionary::createAttributeImpl<String>(Attribute & attribute, const Field & null_value)
{
    attribute.string_arena = std::make_unique<Arena>();
    const String & string = null_value.get<String>();
    const char * string_in_arena = attribute.string_arena->insert(string.data(), string.size());
    attribute.null_values.emplace<StringRef>(string_in_arena, string.size());
    attribute.maps.emplace<ContainerType<StringRef>>();
}

ComplexKeyHashedDictionary::Attribute
ComplexKeyHashedDictionary::createAttributeWithType(const AttributeUnderlyingType type, const Field & null_value)
{
    Attribute attr{type, {}, {}, {}};

    auto type_call = [&](const auto &dictionary_attribute_type)
    {
        using Type = std::decay_t<decltype(dictionary_attribute_type)>;
        using AttributeType = typename Type::AttributeType;
        createAttributeImpl<AttributeType>(attr, null_value);
    };

    callOnDictionaryAttributeType(type, type_call);

    return attr;
}


template <typename AttributeType, typename OutputType, typename ValueSetter, typename DefaultGetter>
void ComplexKeyHashedDictionary::getItemsImpl(
    const Attribute & attribute, const Columns & key_columns, ValueSetter && set_value, DefaultGetter && get_default) const
{
    const auto & attr = std::get<ContainerType<AttributeType>>(attribute.maps);

    const auto keys_size = key_columns.size();
    StringRefs keys(keys_size);
    Arena temporary_keys_pool;

    const auto rows = key_columns.front()->size();
    for (const auto i : ext::range(0, rows))
    {
        /// copy key data to arena so it is contiguous and return StringRef to it
        const auto key = placeKeysInPool(i, key_columns, keys, temporary_keys_pool);

        const auto it = attr.find(key);
        set_value(i, it ? static_cast<OutputType>(it->getMapped()) : get_default(i));

        /// free memory allocated for the key
        temporary_keys_pool.rollback(key.size);
    }

    query_count.fetch_add(rows, std::memory_order_relaxed);
}


template <typename T>
bool ComplexKeyHashedDictionary::setAttributeValueImpl(Attribute & attribute, const StringRef key, const T value)
{
    auto & map = std::get<ContainerType<T>>(attribute.maps);
    const auto pair = map.insert({key, value});
    return pair.second;
}

bool ComplexKeyHashedDictionary::setAttributeValue(Attribute & attribute, const StringRef key, const Field & value)
{
    switch (attribute.type)
    {
        case AttributeUnderlyingType::utUInt8:
            return setAttributeValueImpl<UInt8>(attribute, key, value.get<UInt64>());
        case AttributeUnderlyingType::utUInt16:
            return setAttributeValueImpl<UInt16>(attribute, key, value.get<UInt64>());
        case AttributeUnderlyingType::utUInt32:
            return setAttributeValueImpl<UInt32>(attribute, key, value.get<UInt64>());
        case AttributeUnderlyingType::utUInt64:
            return setAttributeValueImpl<UInt64>(attribute, key, value.get<UInt64>());
        case AttributeUnderlyingType::utUInt128:
            return setAttributeValueImpl<UInt128>(attribute, key, value.get<UInt128>());
        case AttributeUnderlyingType::utInt8:
            return setAttributeValueImpl<Int8>(attribute, key, value.get<Int64>());
        case AttributeUnderlyingType::utInt16:
            return setAttributeValueImpl<Int16>(attribute, key, value.get<Int64>());
        case AttributeUnderlyingType::utInt32:
            return setAttributeValueImpl<Int32>(attribute, key, value.get<Int64>());
        case AttributeUnderlyingType::utInt64:
            return setAttributeValueImpl<Int64>(attribute, key, value.get<Int64>());
        case AttributeUnderlyingType::utFloat32:
            return setAttributeValueImpl<Float32>(attribute, key, value.get<Float64>());
        case AttributeUnderlyingType::utFloat64:
            return setAttributeValueImpl<Float64>(attribute, key, value.get<Float64>());

        case AttributeUnderlyingType::utDecimal32:
            return setAttributeValueImpl<Decimal32>(attribute, key, value.get<Decimal32>());
        case AttributeUnderlyingType::utDecimal64:
            return setAttributeValueImpl<Decimal64>(attribute, key, value.get<Decimal64>());
        case AttributeUnderlyingType::utDecimal128:
            return setAttributeValueImpl<Decimal128>(attribute, key, value.get<Decimal128>());

        case AttributeUnderlyingType::utString:
        {
            auto & map = std::get<ContainerType<StringRef>>(attribute.maps);
            const auto & string = value.get<String>();
            const auto * string_in_arena = attribute.string_arena->insert(string.data(), string.size());
            const auto pair = map.insert({key, StringRef{string_in_arena, string.size()}});
            return pair.second;
        }
    }

    return {};
}

const ComplexKeyHashedDictionary::Attribute & ComplexKeyHashedDictionary::getAttribute(const std::string & attribute_name) const
{
    const auto it = attribute_index_by_name.find(attribute_name);
    if (it == std::end(attribute_index_by_name))
        throw Exception{full_name + ": no such attribute '" + attribute_name + "'", ErrorCodes::BAD_ARGUMENTS};

    return attributes[it->second];
}

StringRef ComplexKeyHashedDictionary::placeKeysInPool(const size_t row, const Columns & key_columns, StringRefs & keys, Arena & pool)
{
    const auto keys_size = key_columns.size();
    size_t sum_keys_size{};

    const char * block_start = nullptr;
    for (size_t j = 0; j < keys_size; ++j)
    {
        keys[j] = key_columns[j]->serializeValueIntoArena(row, pool, block_start);
        sum_keys_size += keys[j].size;
    }

    const auto * key_start = block_start;
    for (size_t j = 0; j < keys_size; ++j)
    {
        keys[j].data = key_start;
        key_start += keys[j].size;
    }

    return {block_start, sum_keys_size};
}

template <typename T>
void ComplexKeyHashedDictionary::has(const Attribute & attribute, const Columns & key_columns, PaddedPODArray<UInt8> & out) const
{
    const auto & attr = std::get<ContainerType<T>>(attribute.maps);
    const auto keys_size = key_columns.size();
    StringRefs keys(keys_size);
    Arena temporary_keys_pool;
    const auto rows = key_columns.front()->size();

    for (const auto i : ext::range(0, rows))
    {
        /// copy key data to arena so it is contiguous and return StringRef to it
        const auto key = placeKeysInPool(i, key_columns, keys, temporary_keys_pool);

        const auto it = attr.find(key);
        out[i] = static_cast<bool>(it);

        /// free memory allocated for the key
        temporary_keys_pool.rollback(key.size);
    }

    query_count.fetch_add(rows, std::memory_order_relaxed);
}

std::vector<StringRef> ComplexKeyHashedDictionary::getKeys() const
{
    const Attribute & attribute = attributes.front();

    std::vector<StringRef> result;

    auto type_call = [&](const auto & dictionary_attribute_type)
    {
        using Type = std::decay_t<decltype(dictionary_attribute_type)>;
        using AttributeType = typename Type::AttributeType;

        if constexpr (std::is_same_v<AttributeType, String>)
        {
            result = getKeys<StringRef>(attribute);
        }
        else
        {
            result = getKeys<AttributeType>(attribute);
        }
    };

    callOnDictionaryAttributeType(attribute.type, type_call);

    return result;
}

template <typename T>
std::vector<StringRef> ComplexKeyHashedDictionary::getKeys(const Attribute & attribute) const
{
    const ContainerType<T> & attr = std::get<ContainerType<T>>(attribute.maps);
    std::vector<StringRef> keys;
    keys.reserve(attr.size());
    for (const auto & key : attr)
        keys.push_back(key.getKey());

    return keys;
}

BlockInputStreamPtr ComplexKeyHashedDictionary::getBlockInputStream(const Names & column_names, size_t max_block_size) const
{
    using BlockInputStreamType = DictionaryBlockInputStream<UInt64>;
    return std::make_shared<BlockInputStreamType>(shared_from_this(), max_block_size, getKeys(), column_names);
}

void registerDictionaryComplexKeyHashed(DictionaryFactory & factory)
{
    auto create_layout = [=](const std::string &,
                             const DictionaryStructure & dict_struct,
                             const Poco::Util::AbstractConfiguration & config,
                             const std::string & config_prefix,
                             DictionarySourcePtr source_ptr) -> DictionaryPtr
    {
        if (!dict_struct.key)
            throw Exception{"'key' is required for dictionary of layout 'complex_key_hashed'", ErrorCodes::BAD_ARGUMENTS};

        const auto dict_id = StorageID::fromDictionaryConfig(config, config_prefix);
        const DictionaryLifetime dict_lifetime{config, config_prefix + ".lifetime"};
        const bool require_nonempty = config.getBool(config_prefix + ".require_nonempty", false);
        return std::make_unique<ComplexKeyHashedDictionary>(dict_id, dict_struct, std::move(source_ptr), dict_lifetime, require_nonempty);
    };
    factory.registerLayout("complex_key_hashed", create_layout, true);
}

}
