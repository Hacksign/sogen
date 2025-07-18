#pragma once

#include <list>
#include <span>
#include <vector>
#include <string>
#include <string_view>
#include <stdexcept>
#include <cstring>
#include <optional>
#include <functional>
#include <typeindex>
#include <atomic>

namespace utils
{
    class buffer_serializer;
    class buffer_deserializer;

    template <typename T>
    concept Serializable = requires(T a, const T ac, buffer_serializer& serializer, buffer_deserializer& deserializer) {
        { ac.serialize(serializer) } -> std::same_as<void>;
        { a.deserialize(deserializer) } -> std::same_as<void>;
    };

    template <typename T>
    struct is_optional : std::false_type
    {
    };

    template <typename T>
    struct is_optional<std::optional<T>> : std::true_type
    {
    };

    namespace detail
    {
        template <typename, typename = void>
        struct has_serialize_function : std::false_type
        {
        };

        template <typename T>
        struct has_serialize_function<T,
                                      std::void_t<decltype(serialize(std::declval<buffer_serializer&>(),
                                                                     std::declval<const std::remove_cvref_t<T>&>()))>>
            : std::true_type
        {
        };

        template <typename, typename = void>
        struct has_deserialize_function : std::false_type
        {
        };

        template <typename T>
        struct has_deserialize_function<T, std::void_t<decltype(deserialize(std::declval<buffer_deserializer&>(),
                                                                            std::declval<std::remove_cvref_t<T>&>()))>>
            : std::true_type
        {
        };

        template <typename T>
        struct has_deserializer_constructor : std::bool_constant<std::is_constructible_v<T, buffer_deserializer&>>
        {
        };
    }

    class buffer_serializer
    {
      public:
        buffer_serializer() = default;

        void write(const void* buffer, const size_t length)
        {
            const auto old_size_remainder = static_cast<uint8_t>(length);
            constexpr auto check_size = sizeof(old_size_remainder);

            if (this->break_offset_ && this->buffer_.size() <= *this->break_offset_ &&
                this->buffer_.size() + length + check_size > *this->break_offset_)
            {
                throw std::runtime_error("Break offset reached!");
            }

            const auto* security_buffer = reinterpret_cast<const std::byte*>(&old_size_remainder);
            this->buffer_.insert(this->buffer_.end(), security_buffer, security_buffer + check_size);

            const auto* byte_buffer = static_cast<const std::byte*>(buffer);
            this->buffer_.insert(this->buffer_.end(), byte_buffer, byte_buffer + length);
        }

        void write(const buffer_serializer& object)
        {
            const auto& buffer = object.get_buffer();
            this->write(buffer.data(), buffer.size());
        }

        template <typename T>
            requires(!is_optional<T>::value)
        void write(const T& object)
        {
            constexpr auto is_trivially_copyable = std::is_trivially_copyable_v<T>;

            if constexpr (Serializable<T>)
            {
                object.serialize(*this);
            }
            else if constexpr (detail::has_serialize_function<T>::value)
            {
                serialize(*this, object);
            }
            else if constexpr (is_trivially_copyable)
            {
                union
                {
                    const T* type_{};
                    const void* void_;
                } pointers;

                pointers.type_ = &object;

                this->write(pointers.void_, sizeof(object));
            }
            else
            {
                static_assert(is_trivially_copyable, "Key must be trivially copyable or implement serializable!");
                std::abort();
            }
        }

        template <typename T>
        void write_atomic(const std::atomic<T>& val)
        {
            this->write(val.load());
        }

        template <typename T>
        void write_optional(const std::optional<T>& val)
        {
            this->write(val.has_value());

            if (val.has_value())
            {
                this->write(*val);
            }
        }

        template <typename T>
        void write_span(const std::span<T> vec)
        {
            this->write(static_cast<uint64_t>(vec.size()));

            for (const auto& v : vec)
            {
                this->write(v);
            }
        }

        template <typename T>
        void write_vector(const std::vector<T>& vec)
        {
            this->write_span(std::span(vec));
        }

        template <typename T>
        void write_list(const std::list<T>& vec)
        {
            this->write(static_cast<uint64_t>(vec.size()));

            for (const auto& v : vec)
            {
                this->write(v);
            }
        }

        template <typename T>
        void write_string(const std::basic_string_view<T> str)
        {
            this->write_span<const T>(str);
        }

        template <typename T>
        void write_string(const std::basic_string<T>& str)
        {
            this->write_string(std::basic_string_view<T>(str));
        }

        template <typename Map>
        void write_map(const Map& map)
        {
            this->write<uint64_t>(map.size());

            for (const auto& entry : map)
            {
                this->write(entry.first);
                this->write(entry.second);
            }
        }

        const std::vector<std::byte>& get_buffer() const
        {
            return this->buffer_;
        }

        std::vector<std::byte> move_buffer()
        {
            return std::move(this->buffer_);
        }

        void set_break_offset(const size_t break_offset)
        {
            this->break_offset_ = break_offset;
        }

        std::optional<size_t> get_diff(const buffer_serializer& other) const
        {
            const auto& b1 = this->get_buffer();
            const auto& b2 = other.get_buffer();

            const auto s1 = b1.size();
            const auto s2 = b2.size();

            for (size_t i = 0; i < s1 && i < s2; ++i)
            {
                if (b1.at(i) != b2.at(i))
                {
                    return i;
                }
            }

            if (s1 != s2)
            {
                return std::min(s1, s2);
            }

            return std::nullopt;
        }

        void print_diff(const buffer_serializer& other) const
        {
            const auto diff = this->get_diff(other);
            if (diff)
            {
                printf("Diff at %zd\n", *diff);
            }
        }

      private:
        std::vector<std::byte> buffer_{};
        std::optional<size_t> break_offset_{};
    };

    class buffer_deserializer
    {
      public:
        template <typename T>
        buffer_deserializer(const std::span<T> buffer)
            : buffer_(reinterpret_cast<const std::byte*>(buffer.data()), buffer.size() * sizeof(T))
        {
            static_assert(std::is_trivially_copyable_v<T>, "Type must be trivially copyable");
        }

        template <typename T>
        buffer_deserializer(const std::vector<T>& buffer)
            : buffer_deserializer(std::span(buffer))
        {
        }

        buffer_deserializer(const buffer_serializer& serializer)
            : buffer_deserializer(serializer.get_buffer())
        {
        }

        std::span<const std::byte> read_data(const size_t length)
        {
            const auto length_rest = static_cast<uint8_t>(length);
            constexpr auto check_size = sizeof(length_rest);

            if (this->offset_ + (length + check_size) > this->buffer_.size())
            {
                throw std::runtime_error("Out of bounds read from byte buffer");
            }

            if (static_cast<uint8_t>(this->buffer_[this->offset_]) != length_rest)
            {
                throw std::runtime_error("Reading from serialized buffer mismatches written data!");
            }

            this->offset_ += check_size;

            const std::span result(this->buffer_.data() + this->offset_, length);
            this->offset_ += length;

            return result;
        }

        void read(void* data, const size_t length)
        {
            const auto span = this->read_data(length);
            memcpy(data, span.data(), length);
        }

        template <typename T>
        void read(T& object)
        {
            constexpr auto is_trivially_copyable = std::is_trivially_copyable_v<T>;

            if constexpr (Serializable<T>)
            {
                object.deserialize(*this);
            }
            else if constexpr (detail::has_deserialize_function<T>::value)
            {
                deserialize(*this, object);
            }
            else if constexpr (is_trivially_copyable)
            {
                union
                {
                    T* type_{};
                    void* void_;
                } pointers;

                pointers.type_ = &object;

                this->read(pointers.void_, sizeof(object));
            }
            else
            {
                static_assert(!is_trivially_copyable, "Key must be trivially copyable or implement serializable!");
                std::abort();
            }
        }

        template <typename T>
        T read()
        {
            auto object = this->construct_object<T>();
            this->read(object);
            return object;
        }

        template <typename T>
        void read_atomic(std::atomic<T>& val)
        {
            val = this->read<T>();
        }

        template <typename T>
        void read_optional(std::optional<T>& val)
        {
            if (this->read<bool>())
            {
                val.emplace(this->read<T>());
            }
            else
            {
                val = std::nullopt;
            }
        }

        template <typename T, typename F>
            requires(std::is_invocable_r_v<T, F>)
        void read_optional(std::optional<T>& val, const F& factory)
        {
            if (this->read<bool>())
            {
                val.emplace(factory());
                this->read<T>(*val);
            }
            else
            {
                val = {};
            }
        }

        template <typename T>
        void read_vector(std::vector<T>& result)
        {
            const auto size = this->read<uint64_t>();
            result.clear();
            result.reserve(static_cast<size_t>(size));

            for (uint64_t i = 0; i < size; ++i)
            {
                result.emplace_back(this->read<T>());
            }
        }

        template <typename T>
        std::vector<T> read_vector()
        {
            std::vector<T> result{};
            this->read_vector(result);
            return result;
        }

        template <typename T>
        void read_list(std::list<T>& result)
        {
            const auto size = this->read<uint64_t>();
            result.clear();

            for (uint64_t i = 0; i < size; ++i)
            {
                result.emplace_back(this->read<T>());
            }
        }

        template <typename T>
        std::list<T> read_list()
        {
            std::list<T> result{};
            this->read_list(result);
            return result;
        }

        template <typename Map>
        void read_map(Map& map)
        {
            using key_type = typename Map::key_type;
            using value_type = typename Map::mapped_type;

            map.clear();

            const auto size = this->read<uint64_t>();

            for (uint64_t i = 0; i < size; ++i)
            {
                auto key = this->read<key_type>();
                auto value = this->read<value_type>();

                map.emplace(std::move(key), std::move(value));
            }
        }

        template <typename Map>
        Map read_map()
        {
            Map map{};
            this->read_map(map);
            return map;
        }

        template <typename T = char>
        void read_string(std::basic_string<T>& result)
        {
            const auto size = this->read<uint64_t>();

            result.clear();
            result.reserve(static_cast<size_t>(size));

            for (uint64_t i = 0; i < size; ++i)
            {
                result.push_back(this->read<T>());
            }
        }

        template <typename T = char>
        std::basic_string<T> read_string()
        {
            std::basic_string<T> result{};
            this->read_string(result);
            return result;
        }

        size_t get_remaining_size() const
        {
            return this->buffer_.size() - offset_;
        }

        std::span<const std::byte> get_remaining_data()
        {
            return this->read_data(this->get_remaining_size());
        }

        size_t get_offset() const
        {
            return this->offset_;
        }

        template <typename T, typename F>
            requires(std::is_invocable_r_v<T, F>)
        void register_factory(F factory)
        {
            this->factories_[std::type_index(typeid(T))] = [f = std::move(factory)]() -> T* { return new T(f()); };
        }

      private:
        size_t offset_{0};
        std::span<const std::byte> buffer_{};
        std::unordered_map<std::type_index, std::function<void*()>> factories_{};

        template <typename T>
        T construct_object()
        {
            if constexpr (detail::has_deserializer_constructor<T>::value)
            {
                return T(*this);
            }
            else if constexpr (std::is_default_constructible_v<T>)
            {
                return {};
            }
            else
            {
                const auto factory = this->factories_.find(std::type_index(typeid(T)));
                if (factory == this->factories_.end())
                {
                    throw std::runtime_error("Object construction failed. Missing factory for type: " +
                                             std::string(typeid(T).name()));
                }

                auto* object = static_cast<T*>(factory->second());
                auto obj = std::move(*object);
                delete object;

                return obj;
            }
        }
    };

    template <>
    inline void buffer_deserializer::read<bool>(bool& object)
    {
        object = this->read<uint8_t>() != 0;
    }

    template <>
    inline void buffer_deserializer::read<std::string>(std::string& object)
    {
        object = this->read_string<char>();
    }

    template <>
    inline void buffer_deserializer::read<std::wstring>(std::wstring& object)
    {
        object = this->read_string<wchar_t>();
    }

    template <>
    inline void buffer_deserializer::read<std::u16string>(std::u16string& object)
    {
        object = this->read_string<char16_t>();
    }

    template <>
    inline void buffer_serializer::write<bool>(const bool& object)
    {
        this->write<uint8_t>(object ? 1 : 0);
    }

    template <>
    inline void buffer_serializer::write<std::string>(const std::string& object)
    {
        this->write_string(object);
    }

    template <>
    inline void buffer_serializer::write<std::wstring>(const std::wstring& object)
    {
        this->write_string(object);
    }

    template <>
    inline void buffer_serializer::write<std::u16string>(const std::u16string& object)
    {
        this->write_string(object);
    }
}
