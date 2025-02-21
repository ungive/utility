#pragma once

namespace ungive
{
namespace utility
{
namespace detail
{

template <typename L>
class unlock_guard
{
public:
    unlock_guard(L& lockable) : m_lockable(lockable) { m_lockable.unlock(); }

    ~unlock_guard() { m_lockable.lock(); }

    unlock_guard(const unlock_guard&) = delete;
    unlock_guard& operator=(const unlock_guard&) = delete;

private:
    L& m_lockable;
};

} // namespace detail
} // namespace utility
} // namespace ungive
