#pragma once

#include <memory>

namespace Clog
{

template<typename T>
class optional
{
	public:
		optional() {}
		optional(const T& value)
		{
			data = std::make_shared<T>(value);
		}

		bool is_initialized() const {
			return data.get() != nullptr;
		}

		const T &operator*() const
		{
			return *data;
		}

		const T &operator=(const T &value)
		{
			data = std::make_shared<T>(value);
			return value;
		}

	private:
		std::shared_ptr<T> data;
};

}
