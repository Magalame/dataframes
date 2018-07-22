#include <chrono>
#include <iostream>
#include <vector>
#include <algorithm>
#include <sstream>

#include "Core/Common.h"
#include "Core/Error.h"

#include "arrow/array.h"
#include "arrow/record_batch.h"
#include "arrow/status.h"
#include "arrow/table.h"
#include "arrow/table_builder.h"
#include "arrow/type.h"
#include "arrow/util/checked_cast.h"
#include <arrow/builder.h>
#include <arrow/io/file.h>
#include <arrow/ipc/writer.h>
#include "LifetimeManager.h"

#ifdef _DEBUG
#pragma comment(lib, "arrowd.lib")
#else
#pragma comment(lib, "arrow.lib")
#endif

using namespace std;

template<typename ...Ts> struct TypeList {};

using Types = TypeList<int32_t, int64_t, float, double>; 

template<typename T>
struct TypeDescription {};

struct UInt8  {};
struct UInt16 {};
struct UInt32 {};
struct UInt64 {};
struct Int8   {};
struct Int16  {};
struct Int32  {};
struct Int64  {};
struct Float  {};
struct Double {};
struct String {};

template<typename T>
struct NumericTypeDescription
{
	using BuilderType = arrow::NumericBuilder<T>;
	//using ValueType = typename BuilderType::value_type;
	using CType = typename BuilderType::value_type;
	using Array = arrow::NumericArray<T>;
};

template<> struct TypeDescription<UInt8>  : NumericTypeDescription<arrow::UInt8Type>  {};
template<> struct TypeDescription<UInt16> : NumericTypeDescription<arrow::UInt16Type> {};
template<> struct TypeDescription<UInt32> : NumericTypeDescription<arrow::UInt32Type> {};
template<> struct TypeDescription<UInt64> : NumericTypeDescription<arrow::UInt64Type> {};
template<> struct TypeDescription<Int8>   : NumericTypeDescription<arrow::Int8Type>   {};
template<> struct TypeDescription<Int16>  : NumericTypeDescription<arrow::Int16Type>  {};
template<> struct TypeDescription<Int32>  : NumericTypeDescription<arrow::Int32Type>  {};
template<> struct TypeDescription<Int64>  : NumericTypeDescription<arrow::Int64Type>  {};
template<> struct TypeDescription<Float>  : NumericTypeDescription<arrow::FloatType>  {};
template<> struct TypeDescription<Double> : NumericTypeDescription<arrow::DoubleType> {};

template<> struct TypeDescription<String>
{
	using BuilderType = arrow::StringBuilder;
	//using ValueType = typename BuilderType::value_type;
	using CType = const char *;
	using Array = arrow::StringArray;
};

template<typename To, typename From>
auto *throwingCast(From *from)
{
	if(auto ret = dynamic_cast<To>(from))
		return ret;

	std::ostringstream out;
	out << "Failed to cast " << from << " to " << typeid(To).name();
	throw std::runtime_error(out.str());
}

template<typename TypeTag>
auto asSpecificArray(arrow::Array *array)
{
	return throwingCast<typename TypeDescription<TypeTag>::Array*>(array);
}

void checkStatus(const arrow::Status &status)
{
	if(!status.ok())
		throw std::runtime_error(status.ToString());
}


extern "C"
{
#define PRIMITIVE_BUILDER(TYPENAME) \
	EXPORT TypeDescription<TYPENAME>::BuilderType *builder##TYPENAME##New(const char **outError) noexcept															   \
	{																																					   \
		LOG(""); \
		/* NOTE: needs release */																														   \
		return TRANSLATE_EXCEPTION(outError)																											   \
		{																																				   \
			return LifetimeManager::instance().addOwnership(std::make_shared<TypeDescription<TYPENAME>::BuilderType>());									   \
		};																																				   \
	}																																					   \
	EXPORT void builder##TYPENAME##Reserve(TypeDescription<TYPENAME>::BuilderType *builder, int64_t count, const char **outError) noexcept						   \
	{																																					   \
		LOG("@{}: {}", (void*)builder, count); \
		TRANSLATE_EXCEPTION(outError)																													   \
		{																																				   \
			checkStatus(builder->Reserve(count));																										   \
		};																																				   \
	}																																					   \
	EXPORT void builder##TYPENAME##Resize(TypeDescription<TYPENAME>::BuilderType *builder, int64_t count, const char **outError) noexcept							   \
	{																																					   \
		LOG("@{}: {}", (void*)builder, count); \
		return TRANSLATE_EXCEPTION(outError)																											   \
		{																																				   \
			checkStatus(builder->Resize(count));																										   \
		};																																				   \
	}																																					   \
	EXPORT void builder##TYPENAME##AppendValue(TypeDescription<TYPENAME>::BuilderType *builder, TypeDescription<TYPENAME>::CType value, const char **outError) noexcept \
	{																																					   \
		LOG("@{}: {} :: {}", (void*)builder, value, #TYPENAME); \
		return TRANSLATE_EXCEPTION(outError)																											   \
		{																																				   \
			checkStatus(builder->Append(value));																										   \
		};																																				   \
	}																																					   \
	EXPORT void builder##TYPENAME##AppendNull(TypeDescription<TYPENAME>::BuilderType *builder, const char **outError) noexcept									   \
	{																																					   \
		LOG("@{}", (void*)builder); \
		return TRANSLATE_EXCEPTION(outError)																											   \
		{																																				   \
			checkStatus(builder->AppendNull());																											   \
		};																																				   \
	}																																					   \
	EXPORT arrow::Array *builder##TYPENAME##Finish(TypeDescription<TYPENAME>::BuilderType *builder, const char **outError) noexcept								   \
	{																																					   \
		LOG("@{}", (void*)builder); \
		return TRANSLATE_EXCEPTION(outError)																											   \
		{																																				   \
			std::shared_ptr<arrow::Array> resultArray = nullptr;																						   \
			checkStatus(builder->Finish(&resultArray));																									   \
			/* Note: owner must release ownership by arrayRelease */																					   \
			return LifetimeManager::instance().addOwnership(resultArray);																				   \
		};																																				   \
	}

	PRIMITIVE_BUILDER(UInt8);
	PRIMITIVE_BUILDER(UInt16);
	PRIMITIVE_BUILDER(UInt32);
	PRIMITIVE_BUILDER(UInt64);
	PRIMITIVE_BUILDER(Int8);
	PRIMITIVE_BUILDER(Int16);
	PRIMITIVE_BUILDER(Int32);
	PRIMITIVE_BUILDER(Int64);
	PRIMITIVE_BUILDER(Float);
	PRIMITIVE_BUILDER(Double);
	//PRIMITIVE_BUILDER(String);
}

// BUFFER
extern "C"
{
	EXPORT int64_t bufferSize(arrow::Buffer *buffer, const char **outError) noexcept
	{
		return TRANSLATE_EXCEPTION(outError)
		{
			return buffer->size();
		};
	}

	EXPORT const void *bufferData(arrow::Buffer *buffer, const char **outError) noexcept
	{
		return TRANSLATE_EXCEPTION(outError)
		{
			return buffer->data();
		};
	}

	// NOTE: needs release 
	// (deep copy)
	EXPORT arrow::Buffer *bufferCopy(arrow::Buffer *buffer, int64_t start, int64_t byteCount, const char **outError) noexcept
	{
		LOG("@{} beginning from {} will copy {} bytes", (void*)buffer, start, byteCount);
		return TRANSLATE_EXCEPTION(outError)
		{
			std::shared_ptr<arrow::Buffer> ret = nullptr;
			checkStatus(buffer->Copy(start, byteCount, &ret));
			return LifetimeManager::instance().addOwnership(ret);
		};
	}

	EXPORT int64_t bufferCapacity(arrow::Buffer *buffer, const char **outError) noexcept
	{
		LOG("@{}", (void*)buffer);
		return TRANSLATE_EXCEPTION(outError)
		{
			return buffer->capacity();
		};
	}
}

// ARRAY DATA
extern "C"
{
	// NOTE: does not account for offset!!!
	EXPORT int64_t arrayDataBufferCount(arrow::ArrayData *arrayData) noexcept
	{
		LOG("@{}", (void*)arrayData);
		return TRANSLATE_EXCEPTION(nullptr)
		{
			return arrayData->length;
		};
	}

	// NOTE: needs release
	EXPORT arrow::Buffer *arrayDataBufferAt(arrow::ArrayData *arrayData, size_t bufferIndex, const char **outError) noexcept
	{
		LOG("@{}", (void*)arrayData);
		return TRANSLATE_EXCEPTION(outError)
		{
			return LifetimeManager::instance().addOwnership(arrayData->buffers.at(bufferIndex));
		};
	}
}

// ARRAY
extern "C"
{
	EXPORT int64_t arrayBufferCount(arrow::Array *array) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(nullptr)
		{
			return array->data()->length;
		};
	}

#define NUMERIC_ARRAY_METHODS(TYPENAME)                                                                                                  \
	EXPORT  TypeDescription<TYPENAME>::CType array##TYPENAME##ValueAt(arrow::Array *array, int64_t index, const char **outError) noexcept\
	{																																	 \
		LOG("[{}]", index);                                                                                                              \
		/* NOTE: needs release */																										 \
		return TRANSLATE_EXCEPTION(outError)																							 \
		{																																 \
			if(index < 0 || index >= array->length())                                                                                    \
			{                                                                                                                            \
				std::ostringstream out;                                                                                                  \
				out << "wrong index " << index << "; array length is " << array->length();                                               \
				throw std::out_of_range{out.str()};                                                                                      \
			}                                                                                                                            \
			return asSpecificArray<TYPENAME>(array)->Value(index);                                                                       \
		};																																 \
	}                                                                                                                                    \
	EXPORT const TypeDescription<TYPENAME>::CType *array##TYPENAME##RawValues(arrow::Array *array, const char **outError) noexcept       \
	{																																	 \
		LOG("@{}", (void*)array);                                                                                                        \
		/* NOTE: needs release */																										 \
		return TRANSLATE_EXCEPTION(outError)																							 \
		{																																 \
			return asSpecificArray<TYPENAME>(array)->raw_values();                                                                       \
		};																																 \
	}
	
	NUMERIC_ARRAY_METHODS(UInt8);
	NUMERIC_ARRAY_METHODS(UInt16);
	NUMERIC_ARRAY_METHODS(UInt32);
	NUMERIC_ARRAY_METHODS(UInt64);
	NUMERIC_ARRAY_METHODS(Int8);
	NUMERIC_ARRAY_METHODS(Int16);
	NUMERIC_ARRAY_METHODS(Int32);
	NUMERIC_ARRAY_METHODS(Int64);
	NUMERIC_ARRAY_METHODS(Float);
	NUMERIC_ARRAY_METHODS(Double);

	// NOTE: needs release
	EXPORT arrow::Buffer *primitiveArrayValueBuffer(arrow::Array *array, const char **outError) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(outError)
		{
			if(auto primitiveArray = dynamic_cast<arrow::PrimitiveArray*>(array))
				return LifetimeManager::instance().addOwnership(primitiveArray->values());

			std::ostringstream out;
			out << "Failed to cast " << array << " to specific array!";
			throw std::runtime_error(out.str());
		};
	}

	// NOTE: needs release
	EXPORT arrow::Buffer *arrayNullBitmapBuffer(arrow::Array *array, size_t bufferIndex, const char **outError) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(outError)
		{
			return LifetimeManager::instance().addOwnership(array->null_bitmap());
		};
	}

	// NOTE: needs release
	EXPORT arrow::ArrayData *arrayData(arrow::Array *array, const char **outError) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(outError)
		{
			return LifetimeManager::instance().addOwnership(array->data());
		};
	}

	// NOTE: needs release
	EXPORT arrow::Array *arraySlice(arrow::Array *array, int64_t offset, int64_t length, const char **outError) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(outError)
		{
			return LifetimeManager::instance().addOwnership(array->Slice(offset, length));
		};
	}

	EXPORT bool arrayIsNullAt(arrow::Array *array, int64_t index, const char **outError) noexcept
	{
		LOG("@{}", (void*)array);
		return TRANSLATE_EXCEPTION(outError)
		{
			return array->IsNull(index);
		};
	}

	EXPORT int64_t arrayLength(arrow::Array *array) noexcept
	{
		LOG("@{}", (void*)array);
		return array->length();
	}

	EXPORT int64_t nullCount(arrow::Array *array) noexcept
	{
		LOG("@{}", (void*)array);
		return array->null_count();
	}
}

// RESOURCE MANAGEMENT
extern "C"
{
	EXPORT void release(void *handle) noexcept
	{
		LOG("@{}", (void*)handle);
		return TRANSLATE_EXCEPTION(nullptr)
		{
			LifetimeManager::instance().releaseOwnership(handle);
		};
	}
}

int main()
{

	{
		const char **err = nullptr;
		auto builder = builderInt64New(err);
		builderInt64AppendValue(builder, 60, err);
		builderInt64AppendValue(builder, 500, err);
		
		auto array = builderInt64Finish(builder, err);
		builderInt64Finish(builder, err);

// 		auto data = arrayData(array);
// 
// 		int size = arrayCount(array);
// 		arrayRelease(array);
	}

	arrow::StringBuilder sb;
	auto s1 = sb.AppendNull();
	sb.AppendValues({"a", "bbb", "cc"});

	std::shared_ptr<arrow::Array> stringArray = nullptr;
	sb.Finish(&stringArray);
	auto strar = std::dynamic_pointer_cast<arrow::StringArray>(stringArray);

	auto stringType = sb.type();

	arrow::Int64Builder builder;
	builder.Append(1);
	builder.Append(2);
	builder.Append(3);
	builder.AppendNull();
	builder.Append(5);
	builder.Append(6);
	builder.Append(7);
	builder.Append(8);
	builder.Append(9);

	std::shared_ptr<arrow::Array> array;
	builder.Finish(&array);
	
	auto int64Length = array->length();
	auto numar = std::dynamic_pointer_cast<arrow::Int64Array>(array);
	
	auto nullBuffer = array->null_bitmap();
	auto nullCap = nullBuffer->capacity();
	auto nullSize = nullBuffer->size();

	auto sliced = array->Slice(2,4);

	bool nullable = true;
	auto field = std::make_shared<arrow::Field>("name", arrow::int64(), nullable, nullptr);

	auto column = std::make_shared<arrow::Column>("kino", sliced);

	std::shared_ptr<arrow::io::FileOutputStream> outFile = nullptr;
	arrow::io::FileOutputStream::Open("foo.dat", &outFile);


	//arrow::Schema schema{fields, nullptr};

	std::shared_ptr<arrow::ipc::RecordBatchFileWriter> writer{};
	//rw.Open(outFile.get(), schema, &writer);
}