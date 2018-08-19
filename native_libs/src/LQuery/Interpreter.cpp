#include "Interpreter.h"

#include <string_view>
#include <arrow/buffer.h>
#include <arrow/table.h>
#include <regex>

#include "Core/ArrowUtilities.h"
#include "AST.h"
#include "Core/Common.h"

using namespace std::literals;

//namespace
//{

    // type-aware wrapper for buffer that is either owned or belongs to an external Column
    template<typename T>
    struct ArrayOperand 
    {
        std::shared_ptr<arrow::Buffer> buffer;

        auto mutable_data() { return reinterpret_cast<T *>(buffer->mutable_data()); }
        auto data() const { return reinterpret_cast<const T*>(buffer->data()); }
        
        explicit ArrayOperand(const arrow::Array *array)
            : buffer(array->data()->buffers.at(1))
        {}
        explicit ArrayOperand(size_t length)
        {
            buffer = allocateBuffer<T>(length);
        }

        //T &operator[](size_t index) { return mutable_data()[index]; }
        
        T load(size_t index) const
        {
            return data()[index];
        }
        void store(size_t index, T value)
        {
            mutable_data()[index] = value;
        }
    };

    template<>
    struct ArrayOperand<std::string>
    {
        const arrow::StringArray *array;
//         std::shared_ptr<arrow::Buffer> bufferOffsets;
//         std::shared_ptr<arrow::Buffer> bufferData;
     
        explicit ArrayOperand(const arrow::Array *array)
            : array(static_cast<const arrow::StringArray *>(array))
//             , bufferOffsets(array->value_offsets())
//             , bufferData(array->value_data())
        {
//             const auto &arrayS = static_cast<const arrow::StringArray&>(array);
//             bufferOffsets = arrayS.value_offsets();
//             bufferData = arrayS.value_data();
        }
        explicit ArrayOperand(size_t length)
        {
            throw std::runtime_error("not implemented: building string column in interpreter");
//             bufferOffsets = allocateBuffer<int32_t>(length);
//             bufferData = allocateBuffer<uint8_t>(0);
        }

        std::string_view load(size_t index) const
        {
            int32_t length;
            auto ptr = array->GetValue(index, &length);
            return std::string_view(reinterpret_cast<const char*>(ptr), length);
        }

        void store(size_t index, const std::string &value)
        {
            throw std::runtime_error("not implemented: storing string in column view");
        }

    };
    template<>
    struct ArrayOperand<bool> : ArrayOperand<unsigned char>
    {
        ArrayOperand(size_t length)
            : ArrayOperand<unsigned char>(arrow::BitUtil::BytesForBits(length))
        {}

        bool load(size_t index) const
        {
            return arrow::BitUtil::GetBit(data(), index);
        }

        void store(size_t index, bool value)
        {
            if(value)
                arrow::BitUtil::SetBit(mutable_data(), index);
            else
                arrow::BitUtil::ClearBit(mutable_data(), index);
        }
    };

    template<typename T>
    auto getValue(const ArrayOperand<T> &src, int64_t index)
    {
        return src.load(index);
    }
    template<typename T>
    auto getValue(const T &src, int64_t index)
    {
        // so our functions get only string_view
        if constexpr(std::is_same_v<T, std::string>)
            return std::string_view(src);
        else
            return src;
    }

#define COMPLAIN_ABOUT_OPERAND_TYPES \
        throw std::runtime_error(__FUNCTION__ + ": not supported operand types: "s + typeid(lhs).name() + " and "s + typeid(rhs).name()); \

#define BINARY_REL_OPERATOR(op)                                                                          \
    template<typename Lhs, typename Rhs>                                                                 \
    static bool exec(const Lhs &lhs, const Rhs &rhs)                                                     \
    { /* below we protect against mixed types like int/string (eg. for ==)  */                           \
        if constexpr(std::is_same_v<Lhs, Rhs> || std::is_arithmetic_v<Lhs> && std::is_arithmetic_v<Rhs>) \
            return lhs op rhs;                                                                           \
        else                                                                                             \
        {                                                                                                \
            COMPLAIN_ABOUT_OPERAND_TYPES;                                                                \
            return {}; /* just for type inference  */                                                    \
        }                                                                                                \
    }

#define BINARY_ARIT_OPERATOR(op)                                                                         \
    template<typename Lhs, typename Rhs>                                                                 \
    static auto exec(const Lhs &lhs, const Rhs &rhs)                                                     \
    {                                                                                                    \
        if constexpr(std::is_same_v<Lhs, Rhs> || std::is_arithmetic_v<Lhs> && std::is_arithmetic_v<Rhs>) \
            return lhs op rhs;                                                                           \
        else                                                                                             \
        {                                                                                                \
    COMPLAIN_ABOUT_OPERAND_TYPES;                                                                        \
        return lhs; /* just for type inference  */                                                       \
        }                                                                                                \
    }
#define FAIL_ON_STRING(ret)                                                                              \
    static ret exec(const std::string &lhs, const std::string &rhs)                                      \
    {                                                                                                    \
            COMPLAIN_ABOUT_OPERAND_TYPES;                                                                \
    }                                                                                                    \
    static ret exec(const std::string_view &lhs, const std::string_view &rhs)                            \
    {                                                                                                    \
            COMPLAIN_ABOUT_OPERAND_TYPES;                                                                \
    }                                                                                                    \
    template<typename Rhs>                                                                               \
    static ret exec(const std::string_view &lhs, const Rhs &rhs)                                         \
    {                                                                                                    \
            COMPLAIN_ABOUT_OPERAND_TYPES;                                                                \
    }                                                                                                    \
    template<typename Lhs>                                                                               \
    static ret exec(const Lhs &lhs, const std::string_view &rhs)                                         \
    {                                                                                                    \
            COMPLAIN_ABOUT_OPERAND_TYPES;                                                                \
    } 

    struct GreaterThan { BINARY_REL_OPERATOR(>); FAIL_ON_STRING(bool); };
    struct LessThan    { BINARY_REL_OPERATOR(<); FAIL_ON_STRING(bool); };
    struct EqualTo     { BINARY_REL_OPERATOR(==);};
    struct StartsWith
    {
        static bool exec(const std::string_view &lhs, const std::string_view &rhs)
        {
            return lhs.length() >= rhs.length()
                && std::memcmp(lhs.data(), rhs.data(), rhs.length()) == 0;
        }

        template<typename Lhs, typename Rhs>
        static bool exec(const Lhs &lhs, const Rhs &rhs)
        {
            COMPLAIN_ABOUT_OPERAND_TYPES;
        }
    };
    struct Matches
    {
        static bool exec(const std::string_view &lhs, const std::string_view &rhs)
        {
            std::regex regex{std::string(rhs)};
            return std::regex_match(lhs.begin(), lhs.end(), regex);
        }

        template<typename Lhs, typename Rhs>
        static bool exec(const Lhs &lhs, const Rhs &rhs)
        {
            COMPLAIN_ABOUT_OPERAND_TYPES;
        }
    };
    struct Plus        { BINARY_ARIT_OPERATOR(+); FAIL_ON_STRING(int64_t); };
    struct Minus       { BINARY_ARIT_OPERATOR(-); FAIL_ON_STRING(int64_t); };
    struct Times       { BINARY_ARIT_OPERATOR(*); FAIL_ON_STRING(int64_t); };
    struct Divide      { BINARY_ARIT_OPERATOR(/); FAIL_ON_STRING(int64_t); };
    struct Modulo      
    {
        static constexpr int64_t exec(const int64_t &lhs, const int64_t &rhs)
        {
            return lhs % rhs;
        }
        static double exec(const double &lhs, const double &rhs)
        {
            return std::fmod(lhs, rhs);
        }
        template<typename Lhs, typename Rhs>
        static int64_t exec(const Lhs &lhs, const Rhs &rhs)
        {
            COMPLAIN_ABOUT_OPERAND_TYPES;
        }
    };
    struct Negate
    {
        template<typename Lhs>
        static constexpr Lhs exec(const Lhs &lhs)
        {
            if constexpr(std::is_arithmetic_v<Lhs>)
                return -lhs;
            else
                throw std::runtime_error("negate does not support operand of type: "s + typeid(lhs).name());
        }

        static int64_t exec(const std::string_view &lhs)
        {
            throw std::runtime_error("negate does not support operand of type: "s + typeid(lhs).name());
        }
    };
    struct And
    {
        static bool exec(const bool &lhs, const bool &rhs)
        {
            return lhs && rhs;
        }

        template<typename Lhs, typename Rhs>
        static bool exec(const Lhs &lhs, const Rhs &rhs)
        {
            COMPLAIN_ABOUT_OPERAND_TYPES;
        }
    };
    struct Or
    {
        static bool exec(const bool &lhs, const bool &rhs)
        {
            return lhs || rhs;
        }

        template<typename Lhs, typename Rhs>
        static bool exec(const Lhs &lhs, const Rhs &rhs)
        {
            COMPLAIN_ABOUT_OPERAND_TYPES;
        }
    };
    struct Not
    {
        static bool exec(const bool &lhs)
        {
            return !lhs;
        }

        template<typename Lhs>
        static bool exec(const Lhs &lhs)
        {
            throw std::runtime_error("Not: wrong operand type "s + typeid(Lhs).name());
        }
    };


    template<typename Operation, typename ... Operands>
    auto exec(int64_t count, const Operands & ...operands)
    {
        using OperationResult = decltype(Operation::exec(getValue(operands, 0)...));

        // TODO: optimization opportunity: boolean constant support (remove the last part of if below and fix the build)
        constexpr bool arithmeticOperands = (std::is_arithmetic_v<Operands> && ...);
        if constexpr(arithmeticOperands && !std::is_same_v<bool, OperationResult>)
        {
            return Operation::exec(operands...);
        }
        else
        {
            ArrayOperand<OperationResult> ret{ (size_t)count };
            for(int64_t i = 0; i < count; i++)
            {
                auto result = Operation::exec(getValue(operands, i)...);
                ret.store(i, result);
            }
            return ret;
        }
    }

struct Interpreter
{
    Interpreter(const arrow::Table &table, const ColumnMapping &mapping)
        : table(table)
    {
        for(int i = 0; i < mapping.size(); i++)
            columns.push_back(table.column(mapping.at(i)));
    }

    const arrow::Table &table;
    std::vector<std::shared_ptr<arrow::Column>> columns;

    using Field = std::variant<int64_t, double, std::string, ArrayOperand<int64_t>, ArrayOperand<double>, ArrayOperand<std::string>>;

    Field fieldFromColumn(const arrow::Column &column)
    {
        const auto data = column.data();
        if(data->num_chunks() != 1)
            throw std::runtime_error("not implemented: processing of chunked arrays");

        const auto chunkPtr = data->chunk(0).get();
        return visitArray(chunkPtr, [] (auto *array) -> Field
        {
            using ArrowType = typename std::remove_pointer_t<decltype(array)>::TypeClass;
            using T = typename TypeDescription<ArrowType::type_id>::ValueType;
            return ArrayOperand<T>(array);
        });
    }
    std::vector<Field> evaluateOperands(const std::vector<ast::Value> &operands)
    {
        return transformToVector(operands, 
            [this] (auto &&operand) { return evaluateValue(operand); });
    }
    std::vector<ArrayOperand<bool>> evaluatePredicates(const std::vector<ast::Predicate> &operands)
    {
        return transformToVector(operands, 
            [this] (auto &&operand) { return evaluate(operand); });
    }

    template<typename T>
    static auto getOperand(const T &t, unsigned index)
    {
        if(index < t.size())
            return t[index];

        throw std::runtime_error("failed to get operand by index " + std::to_string(index) + ": has only " + std::to_string(t.size()));
    }

    Field evaluateValue(const ast::Value &value)
    {
        return std::visit(overloaded{
            [&] (const ast::ColumnReference &col)    -> Field { return fieldFromColumn(*columns[col.columnRefId]); },
            [&] (const ast::ValueOperation &op)      -> Field 
            {
#define VALUE_UNARY_OP(opname)                                               \
            case ast::ValueOperator::opname:                                 \
                return std::visit(                                        \
                    [&] (auto &&lhs) -> Field                                \
                        { return exec<opname>(table.num_rows(), lhs);},      \
                    getOperand(operands, 0));
#define VALUE_BINARY_OP(opname)                                              \
            case ast::ValueOperator::opname:                                 \
                return std::visit(                                        \
                    [&] (auto &&lhs, auto &&rhs) -> Field                    \
                        { return exec<opname>(table.num_rows(), lhs, rhs);}, \
                    getOperand(operands, 0), getOperand(operands, 1));

                const auto operands = evaluateOperands(op.operands);
                switch(op.what)
                {
                    VALUE_BINARY_OP(Plus);
                    VALUE_BINARY_OP(Minus);
                    VALUE_BINARY_OP(Times);
                    VALUE_BINARY_OP(Divide);
                    VALUE_BINARY_OP(Modulo);
                    VALUE_UNARY_OP(Negate);
                default:
                    throw std::runtime_error("not implemented: value operator " + std::to_string((int)op.what));
                }
            },
            [&] (const ast::Literal<int64_t> &l)     -> Field { return l.literal; },
            [&] (const ast::Literal<double> &l)      -> Field { return l.literal; },
            [&] (const ast::Literal<std::string> &l) -> Field { return l.literal; },
            //[&] (const ast::Literal<std::string> &l) -> Field { return l.literal; },
            [&] (auto &&t) -> Field { throw std::runtime_error("not implemented: value node of type "s + typeid(decltype(t)).name()); }
            }, (const ast::ValueBase &) value);
    }

    ArrayOperand<bool> evaluate(const ast::Predicate &p)
    {
        return std::visit(overloaded{
            [&] (const ast::PredicateFromValueOperation &elem) -> ArrayOperand<bool>
        {
            const auto operands = evaluateOperands(elem.operands);
            switch(elem.what)
            {
            case ast::PredicateFromValueOperator::Greater:
                return std::visit(
                    [&] (auto &&lhs, auto &&rhs) { return exec<GreaterThan>(table.num_rows(), lhs, rhs);},
                    getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateFromValueOperator::Lesser:
                return std::visit(
                    [&] (auto &&lhs, auto &&rhs) { return exec<LessThan>(table.num_rows(), lhs, rhs);},
                    getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateFromValueOperator::Equal:
                return std::visit(
                    [&] (auto &&lhs, auto &&rhs) { return exec<EqualTo>(table.num_rows(), lhs, rhs);},
                    getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateFromValueOperator::StartsWith:
                return std::visit(
                    [&] (auto &&lhs, auto &&rhs) { return exec<StartsWith>(table.num_rows(), lhs, rhs);},
                    getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateFromValueOperator::Matches:
                return std::visit(
                    [&] (auto &&lhs, auto &&rhs) { return exec<Matches>(table.num_rows(), lhs, rhs);},
                    getOperand(operands, 0), getOperand(operands, 1));
            default:
                throw std::runtime_error("not implemented: predicate operator " + std::to_string((int)elem.what));
            }
        },
            [&] (const ast::PredicateOperation &op) -> ArrayOperand<bool> 
        {
            const auto operands = evaluatePredicates(op.operands);
            switch(op.what)
            {
            case ast::PredicateOperator::And:
                return exec<And>(table.num_rows(), getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateOperator::Or:
                return exec<Or>(table.num_rows(), getOperand(operands, 0), getOperand(operands, 1));
            case ast::PredicateOperator::Not:
                return exec<Not>(table.num_rows(), operands[0]);
            default:
                throw std::runtime_error("not implemented: predicate operator " + std::to_string((int)op.what));
            }
        }
            }, (const ast::PredicateBase &) p);
    }
};

//}

std::shared_ptr<arrow::Buffer> execute(const arrow::Table &table, const ast::Predicate &predicate, ColumnMapping mapping)
{
    Interpreter interpreter{table, mapping};
    auto ret = interpreter.evaluate(predicate);

    for(auto && [refid, columnIndex] : mapping)
    {
        const auto column = table.column(columnIndex);
        if(column->null_count() == 0)
            continue;

        int i = 0;
        iterateOverGeneric(*column, 
            [&] (auto &&) { i++;              }, 
            [&]           { ret.store(i++, false); });
    }

    return ret.buffer;
}

template<typename T>
auto arrayWith(const arrow::Table &table, const ArrayOperand<T> &arrayProto, std::shared_ptr<arrow::Buffer> nullBuffer)
{
    const auto N = table.num_rows();
    constexpr auto id = ValueTypeToId<T>();
    if constexpr(std::is_arithmetic_v<T>)
    {
        return std::make_shared<typename TypeDescription<id>::Array>(N, arrayProto.buffer, nullBuffer, -1);
    }
    else
    {
        arrow::StringBuilder builder;
        checkStatus(builder.Reserve(N));

        if(nullBuffer)
        {
            for(int i = 0; i < N; i++)
            {
                if(arrow::BitUtil::GetBit(nullBuffer->data(), i))
                {
                    const auto sv = arrayProto.load(i);
                    checkStatus(builder.Append(sv.data(), (int)sv.size()));
                }
                else
                    checkStatus(builder.AppendNull());
            }
        }
        else
        {
            for(int i = 0; i < N; i++)
            {
                const auto sv = arrayProto.load(i);
                checkStatus(builder.Append(sv.data(), (int)sv.size()));
            }
        }

        return finish(builder);
    }
}

template<typename T>
auto arrayWith(const arrow::Table &table, const T &constant, std::shared_ptr<arrow::Buffer> nullBuffer)
{
    const auto N = table.num_rows();
    constexpr auto id = ValueTypeToId<T>();
    if constexpr(std::is_arithmetic_v<T>)
    {
        auto buffer = allocateBuffer<T>(N);
        auto raw = reinterpret_cast<T*>(buffer->mutable_data());
        std::fill_n(raw, N, constant);
        return std::make_shared<typename TypeDescription<id>::Array>(N, buffer, nullBuffer, -1);
    }
    else
    {
        arrow::StringBuilder builder;
        checkStatus(builder.Reserve(N));

        const auto length = (int32_t)constant.size();
        if(!nullBuffer)
        {
            for(int i = 0; i < N; i++)
                checkStatus(builder.Append(constant.data(), length));
        }
        else
        {
            for(int i = 0; i < N; i++)
            {
                if(arrow::BitUtil::GetBit(nullBuffer->data(), i))
                    checkStatus(builder.Append(constant.data(), length));
                else
                    checkStatus(builder.AppendNull());
            }
        }

        return finish(builder);
    }
}

std::shared_ptr<arrow::Array> execute(const arrow::Table &table, const ast::Value &value, ColumnMapping mapping)
{
    Interpreter interpreter{table, mapping};
    auto field = interpreter.evaluateValue(value);


    bool usedNullableColumns = false;
    BitmaskGenerator bitmask{table.num_rows(), true};

    for(auto && [refid, columnIndex] : mapping)
    {
        const auto column = table.column(columnIndex);
        if(column->null_count() == 0)
            continue;
 
        usedNullableColumns = true;

        int64_t i = 0;
        iterateOverGeneric(*column, 
            [&] (auto &&) { i++;}, 
            [&]           { bitmask.clear(i++); });
    }

    const auto nullBufferToBeUsed = usedNullableColumns ? bitmask.buffer : nullptr;

    return std::visit(
        [&] (auto &&i) -> std::shared_ptr<arrow::Array>
        {
            return arrayWith(table, i, nullBufferToBeUsed);
        }, field);
}
