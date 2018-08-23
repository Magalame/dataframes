#include "Analysis.h"
#include <unordered_map>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>


template<arrow::Type::type id>
std::shared_ptr<arrow::Table> countValueTyped(const arrow::Column &column)
{
    using T = typename TypeDescription<id>::ObservedType;
    using Builder = typename TypeDescription<id>::BuilderType;
    std::unordered_map<T, int64_t> valueCounts;

    iterateOver<id>(column,
        [&] (auto &&elem) { valueCounts[elem]++; },
        [] () {});

    Builder valueBuilder;
    arrow::Int64Builder countBuilder;

    valueBuilder.Reserve(valueCounts.size());
    countBuilder.Reserve(valueCounts.size());

    for(auto && [value, count] : valueCounts)
    {
        append(valueBuilder, value);
        append(countBuilder, count);
    }

    if(column.null_count())
    {
        valueBuilder.AppendNull();
        countBuilder.Append(column.null_count());
    }

    auto valueArray = finish(valueBuilder);
    auto countArray = finish(countBuilder);

    auto valueColumn = std::make_shared<arrow::Column>(arrow::field("value", column.type(), column.null_count()), valueArray);
    auto countColumn = std::make_shared<arrow::Column>(arrow::field("count", countArray->type(), false), countArray);

    return tableFromArrays({valueArray, countArray}, {"value", "count"});
}

template<typename T>
struct Minimum
{
    T accumulator = std::numeric_limits<T>::max();
    std::string name = "min";
    void operator() (T elem) {  accumulator = std::min<T>(accumulator, elem); }
    auto get() { return accumulator; }
};

template<typename T>
struct Maximum
{
    T accumulator = std::numeric_limits<T>::min();
    std::string name = "max";
    void operator() (T elem) {  accumulator = std::max<T>(accumulator, elem); }
    auto get() { return accumulator; }
};

// TODO naive implementation, look for something numerically better
template<typename T>
struct Mean
{
    int64_t count = 0;
    double accumulator = 0;
    std::string name = "mean";
    void operator() (T elem) {  accumulator += elem; count++; }
    auto get() { return accumulator / count; }
};

template<typename T>
struct Median
{
    boost::accumulators::accumulator_set<T, boost::accumulators::features<boost::accumulators::tag::median>> accumulator;

    std::string name = "median";
    void operator() (T elem) {  accumulator(elem); }
    auto get() { return boost::accumulators::median(accumulator); }
};

template<typename T>
struct Variance
{
    boost::accumulators::accumulator_set<T, boost::accumulators::features<boost::accumulators::tag::variance>> accumulator;

    std::string name = "variance";
    void operator() (T elem) {  accumulator(elem); }
    auto get() { return boost::accumulators::variance(accumulator); }
};

template<typename T>
struct StdDev : Variance<T>
{
    std::string name = "std dev";
    auto get() { return std::sqrt(Variance<T>::get()); }
};

template<typename T>
struct Sum : Variance<T>
{
    T accumulator{};
    std::string name = "sum";
    void operator() (T elem) {  accumulator += elem; }
    auto get() { return accumulator; }
};


template<arrow::Type::type id, typename Processor>
auto calculateStatScalar(const arrow::Column &column, Processor &p)
{
    iterateOver<id>(column,
        [&] (auto elem) { p(elem); },
        [] {});

    return p.get();
}

template<template <typename> typename Processor>
std::shared_ptr<arrow::Column> calculateStat(const arrow::Column &column)
{
    return visitType(*column.type(), [&](auto id) -> std::shared_ptr<arrow::Column>
    {
        if constexpr(id.value != arrow::Type::STRING)
        {
            using T = typename TypeDescription<id.value>::ValueType;
            Processor<T> p;
            using ResultT = decltype(p.get());

            if(column.length() - column.null_count() <= 0)
                return toColumn(std::vector<std::optional<ResultT>>{std::nullopt}, p.name);

            const auto result = calculateStatScalar<id.value>(column, p);
            return toColumn(std::vector<ResultT>{result}, { p.name });
        }
        else
            throw std::runtime_error("Operation not supported for type " + column.type()->ToString());
    });
}

template<typename T>
std::common_type_t<T, double> vectorNthElement(std::vector<T> &data, int32_t n)
{
    assert(n >= 0 && n < data.size());
    std::nth_element(data.begin(), data.begin() + n, data.end());
    return data[n];
}

template<typename T>
std::common_type_t<T, double> vectorQuantile(std::vector<T> &data, double q = 0.5)
{
    assert(!data.empty());

    if(q >= 1.0)
        return *std::max_element(data.begin(), data.end());
    if(q <= 0)
        return *std::min_element(data.begin(), data.end());

    q = std::clamp(q, 0.0, 1.0);
    const double n = data.size() * q - 0.5;
    const int n1 = std::floor(n);
    const int n2 = std::ceil(n);
    const auto t = n - n1;
    std::nth_element(data.begin(), data.begin() + n1, data.end());
    std::nth_element(data.begin() + n1, data.begin() + n2, data.end());
    return lerp<double>(data[n1], data[n2], t);
}

std::shared_ptr<arrow::Column> calculateQuantile(const arrow::Column &column, double q, std::string name)
{
    // return calculateStat<Median>(column);
    auto v = toJustVector(column);
    return std::visit([&] (auto &vector) -> std::shared_ptr<arrow::Column>
    {
        using VectorType = std::decay_t<decltype(vector)>;
        using T = typename VectorType::value_type;
        if constexpr(std::is_arithmetic_v<T>)
        {
            auto result = vectorQuantile(vector, q);
            return scalarToColumn(result, name);
        }
        else
            throw std::runtime_error(name + " is allowed only for arithmetics type");
    }, v);
}

std::shared_ptr<arrow::Table> countValues(const arrow::Column &column)
{
    return visitType(*column.type(), [&] (auto id)
    {
        return countValueTyped<id.value>(column);
    });
}

std::shared_ptr<arrow::Column> calculateMin(const arrow::Column &column)
{
    return calculateStat<Minimum>(column);
}

std::shared_ptr<arrow::Column> calculateMax(const arrow::Column &column)
{
    return calculateStat<Maximum>(column);
}

std::shared_ptr<arrow::Column> calculateMean(const arrow::Column &column)
{
    return calculateStat<Mean>(column);
}

std::shared_ptr<arrow::Column> calculateMedian(const arrow::Column &column)
{
    return calculateQuantile(column, 0.5, "median");
}

std::shared_ptr<arrow::Column> calculateQuantile(const arrow::Column &column, double q)
{
    return calculateQuantile(column, q, "quantile " + std::to_string(q));
}

std::shared_ptr<arrow::Array> fromMemory(double *data, int32_t dataCount)
{
    arrow::DoubleBuilder builder;
    builder.AppendValues(data, dataCount);
    return finish(builder);
}

std::shared_ptr<arrow::Column> columnFromArray(std::shared_ptr<arrow::Array> array, std::string name)
{
    auto field = arrow::field(name, array->type(), array->null_count());
    return std::make_shared<arrow::Column>(field, array);
}

std::shared_ptr<arrow::Column> calculateVariance(const arrow::Column &column)
{
    return calculateStat<Variance>(column);
}

std::shared_ptr<arrow::Column> calculateStandardDeviation(const arrow::Column &column)
{
    return calculateStat<StdDev>(column);
}

std::shared_ptr<arrow::Column> calculateSum(const arrow::Column &column)
{
    return calculateStat<Sum>(column);
}

double calculateCorrelation(const arrow::Column &xCol, const arrow::Column &yCol)
{
    double sumX = 0;
    double sumY = 0;
    double sumXX = 0;
    double sumYY = 0;
    double sumXY = 0;
    int64_t n = 0;

    visitType(*xCol.type(), [&] (auto id1)
    {
        visitType(*yCol.type(), [&] (auto id2)
        {
            if constexpr(id1.value != arrow::Type::STRING  &&  id2.value != arrow::Type::STRING)
            {
                return iterateOverJustPairs<id1.value, id2.value>(xCol, yCol,
                    [&] (double xVal, double yVal)
                {
                    sumX += xVal;
                    sumY += yVal;
                    sumXX += xVal * xVal;
                    sumYY += yVal * yVal;
                    sumXY += xVal * yVal;
                    n++;
                });
            }
            else
                throw std::runtime_error("Correlation not supported on string types");
        });
    });

    const auto num = n * sumXY - sumX * sumY;
    const auto den = std::sqrt(n * sumXX - (sumX*sumX)) * std::sqrt(n * sumYY - (sumY*sumY));

    return num/den;
}

std::shared_ptr<arrow::Column> calculateCorrelation(const arrow::Table &table, const arrow::Column &column)
{
    if(table.num_rows() != column.length())
        throw std::runtime_error("cannot calculate correlation: mismatched column/table row counts");

    const auto N = table.num_columns();
    std::vector<double> correlationValues;
    correlationValues.resize(N);

    for(int i = 0; i < N; i++)
    {
        const auto columnI = table.column(i);
        const auto isSelfCompare = &column == columnI.get();
        correlationValues[i] = isSelfCompare
            ? 1.0
            : calculateCorrelation(column, *columnI);
    }

    return toColumn(correlationValues, column.name() + "_CORR");
}

std::shared_ptr<arrow::Table> calculateCorrelationMatrix(const arrow::Table &table)
{
    const auto N = table.num_columns();
    std::vector<std::vector<double>> correlationMatrix;
    correlationMatrix.resize(N);
    for(auto &correlationColumn : correlationMatrix)
        correlationColumn.resize(N);

    for(int i = 0; i < N; i++)
    {
        const auto ci = table.column(i);
        correlationMatrix[i][i] = 1.0;
        for(int j = i + 1; j < N; j++)
        {
            const auto cj = table.column(j);
            const auto correlation = calculateCorrelation(*ci, *cj);
            correlationMatrix[i][j] = correlation;
            correlationMatrix[j][i] = correlation;
        }
    }

    std::vector<std::shared_ptr<arrow::Column>> ret;
    for(int i = 0; i < N; i++)
    {
        auto c = toColumn(correlationMatrix.at(i), table.column(i)->name());
        ret.push_back(c);
    }

    return tableFromColumns(ret);
}
