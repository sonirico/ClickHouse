#pragma once

#include <cmath>

#include <DB/Columns/IColumn.h>


namespace DB
{

/** Stuff for comparing numbers.
  * Integer values are compared as usual.
  * Floating-point numbers are compared this way that NaNs always end up at the end
  *  (if you don't do this, the sort would not work at all).
  */
template <typename T>
struct CompareHelper
{
	static bool less(T a, T b) { return a < b; }
	static bool greater(T a, T b) { return a > b; }

	/** Compares two numbers. Returns a number less than zero, equal to zero, or greater than zero if a < b, a == b, a > b, respectively.
	  * If one of the values is NaN, then
	  * - if nan_direction_hint == -1 - NaN are considered less than all numbers;
	  * - if nan_direction_hint == 1 - NaN are considered to be larger than all numbers;
	  * Essentially: nan_direction_hint == -1 says that the comparison is for sorting in descending order.
	  */
	static int compare(T a, T b, int nan_direction_hint)
	{
		return a > b ? 1 : (a < b ? -1 : 0);
	}
};

template <typename T>
struct FloatCompareHelper
{
	static bool less(T a, T b)
	{
		if (unlikely(std::isnan(b)))
			return !std::isnan(a);
		return a < b;
	}

	static bool greater(T a, T b)
	{
		if (unlikely(std::isnan(b)))
			return !std::isnan(a);
		return a > b;
	}

	static int compare(T a, T b, int nan_direction_hint)
	{
		bool isnan_a = std::isnan(a);
		bool isnan_b = std::isnan(b);
		if (unlikely(isnan_a || isnan_b))
		{
			if (isnan_a && isnan_b)
				return 0;

			return isnan_a
				? nan_direction_hint
				: -nan_direction_hint;
		}

		return (T(0) < (a - b)) - ((a - b) < T(0));
	}
};

template <> struct CompareHelper<Float32> : public FloatCompareHelper<Float32> {};
template <> struct CompareHelper<Float64> : public FloatCompareHelper<Float64> {};



/** A template for columns that use a simple array to store.
  */
template <typename T>
class ColumnVector final : public IColumn
{
private:
	using Self = ColumnVector<T>;

	struct less;
	struct greater;

public:
	using value_type = T;
	using Container_t = PaddedPODArray<value_type>;

	ColumnVector() {}
	ColumnVector(const size_t n) : data{n} {}
	ColumnVector(const size_t n, const value_type x) : data{n, x} {}

	bool isNumeric() const override { return IsNumber<T>::value; }
	bool isFixed() const override { return IsNumber<T>::value; }

	size_t sizeOfField() const override { return sizeof(T); }

	size_t size() const override
	{
		return data.size();
	}

	StringRef getDataAt(size_t n) const override
	{
		return StringRef(reinterpret_cast<const char *>(&data[n]), sizeof(data[n]));
	}

	void insertFrom(const IColumn & src, size_t n) override
	{
		data.push_back(static_cast<const Self &>(src).getData()[n]);
	}

	void insertData(const char * pos, size_t length) override
	{
		data.push_back(*reinterpret_cast<const T *>(pos));
	}

	void insertDefault() override
	{
		data.push_back(T());
	}

	void popBack(size_t n) override
	{
		data.resize_assume_reserved(data.size() - n);
	}

	StringRef serializeValueIntoArena(size_t n, Arena & arena, char const *& begin) const override;

	const char * deserializeAndInsertFromArena(const char * pos) override;

	void updateHashWithValue(size_t n, SipHash & hash) const override;

	size_t byteSize() const override
	{
		return data.size() * sizeof(data[0]);
	}

	size_t allocatedSize() const override
	{
		return data.allocated_size() * sizeof(data[0]);
	}

	void insert(const T value)
	{
		data.push_back(value);
	}

	/// This metod implemented in header because it could be possibly devirtualized.
	int compareAt(size_t n, size_t m, const IColumn & rhs_, int nan_direction_hint) const override
	{
		return CompareHelper<T>::compare(data[n], static_cast<const Self &>(rhs_).data[m], nan_direction_hint);
	}

	void getPermutation(bool reverse, size_t limit, Permutation & res) const override;

	void reserve(size_t n) override
	{
		data.reserve(n);
	}

	std::string getName() const override;

	ColumnPtr cloneResized(size_t size) const override;

	Field operator[](size_t n) const override
	{
		return typename NearestFieldType<T>::Type(data[n]);
	}

	void get(size_t n, Field & res) const override
	{
		res = typename NearestFieldType<T>::Type(data[n]);
	}

	const T & getElement(size_t n) const
	{
		return data[n];
	}

	T & getElement(size_t n)
	{
		return data[n];
	}

	UInt64 get64(size_t n) const override;

	void insert(const Field & x) override
	{
		data.push_back(DB::get<typename NearestFieldType<T>::Type>(x));
	}

	void insertRangeFrom(const IColumn & src, size_t start, size_t length) override;

	ColumnPtr filter(const IColumn::Filter & filt, ssize_t result_size_hint) const override;

	ColumnPtr permute(const IColumn::Permutation & perm, size_t limit) const override;

	ColumnPtr replicate(const IColumn::Offsets_t & offsets) const override;

	void getExtremes(Field & min, Field & max) const override;

	Columns scatter(ColumnIndex num_columns, const Selector & selector) const override
	{
		return this->scatterImpl<Self>(num_columns, selector);
	}


	/** More efficient methods of manipulation - to manipulate with data directly. */
	Container_t & getData()
	{
		return data;
	}

	const Container_t & getData() const
	{
		return data;
	}

protected:
	Container_t data;
};


}
