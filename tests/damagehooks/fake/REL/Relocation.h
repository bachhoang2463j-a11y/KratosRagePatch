#pragma once

// Test stand-in for REL/Relocation.h, limited to what the damage hook uses: VariantID and
// the Relocation wrapper with address() / get() / operator() / write_vfunc.
//
// The real Relocation::write_vfunc writes into the vtable and returns the entry it found,
// which is exactly the chaining contract the hook relies on.

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace REL {

class VariantID
{
public:
    constexpr VariantID() noexcept = default;

    explicit constexpr VariantID(std::uint64_t a_seID, std::uint64_t, std::uint64_t) noexcept :
        _address{ static_cast<std::uintptr_t>(a_seID) }
    {}

    [[nodiscard]] constexpr std::uintptr_t address() const noexcept { return _address; }

private:
    std::uintptr_t _address{};
};

template <class T>
class Relocation
{
public:
    using value_type = std::conditional_t<
        std::is_member_pointer_v<T> || std::is_function_v<std::remove_pointer_t<T>>,
        std::decay_t<T>,
        T>;

    constexpr Relocation() noexcept = default;

    explicit constexpr Relocation(std::uintptr_t a_address) noexcept :
        _impl{ a_address }
    {}

    explicit constexpr Relocation(VariantID a_id) noexcept :
        _impl{ a_id.address() }
    {}

    constexpr Relocation& operator=(std::uintptr_t a_address) noexcept
    {
        _impl = a_address;
        return *this;
    }

    template <class... Args>
    std::invoke_result_t<const value_type&, Args...> operator()(Args&&... a_args) const
    {
        return get()(std::forward<Args>(a_args)...);
    }

    [[nodiscard]] constexpr std::uintptr_t address() const noexcept { return _impl; }

    [[nodiscard]] value_type get() const { return reinterpret_cast<value_type>(_impl); }

    template <class F>
    std::uintptr_t write_vfunc(std::size_t a_idx, F a_newFunc)
    {
        return write_vfunc(a_idx, reinterpret_cast<std::uintptr_t>(a_newFunc));
    }

    std::uintptr_t write_vfunc(std::size_t a_idx, std::uintptr_t a_newFunc)
    {
        const auto address = _impl + sizeof(void*) * a_idx;
        const auto previous = *reinterpret_cast<std::uintptr_t*>(address);
        *reinterpret_cast<std::uintptr_t*>(address) = a_newFunc;
        return previous;
    }

private:
    std::uintptr_t _impl{};
};

}
