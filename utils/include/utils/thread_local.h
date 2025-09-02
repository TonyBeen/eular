/*************************************************************************
    > File Name: thread_local.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年12月18日 星期三 10时58分43秒
 ************************************************************************/

#ifndef __EULAR_UTILS_THREAD_H__
#define __EULAR_UTILS_THREAD_H__

#include <map>
#include <memory>
#include <string>

#include <utils/utils.h>

namespace eular {

class TLSAbstractSlot
{
public:
    TLSAbstractSlot() {}
    virtual ~TLSAbstractSlot() {}
};

template <typename T>
class TLSSlot: public TLSAbstractSlot
{
    DISALLOW_COPY_AND_ASSIGN(TLSSlot);
public:
    TLSSlot(): m_value() { }
    TLSSlot(const T &value): m_value(value) { }
    ~TLSSlot() { }

    T &value()
    {
        return m_value;
    }

    T *pointer()
    {
        return &m_value;
    }

private:
    T m_value;
};

class UTILS_API ThreadLocalStorage
{
public:
    ThreadLocalStorage() = default;
    ~ThreadLocalStorage() = default;

    template <typename T>
    std::shared_ptr<TLSSlot<T>> get(const std::string &key)
    {
        auto it = m_tlsMap.find(key);
        if (it == m_tlsMap.end()) {
            // it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>())).first;
            return nullptr;
        }

        return std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
    }

    template <typename T>
    std::shared_ptr<TLSSlot<T>> set(const std::string &key, T &&value)
    {
        std::shared_ptr<TLSSlot<T>> result;
        auto it = m_tlsMap.find(key);
        if (it == m_tlsMap.end()) {
            it = m_tlsMap.insert(std::make_pair(key, std::make_shared<TLSSlot<T>>(std::forward<T>(value)))).first;
            result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
        } else {
            result = std::dynamic_pointer_cast<TLSSlot<T>>(it->second);
            result->value() = std::forward<T>(value);
        }

        return result;
    }

    static ThreadLocalStorage *Current();

private:
    using TLSMap = std::map<std::string, std::shared_ptr<TLSAbstractSlot>>;

    TLSMap m_tlsMap;
};

} // namespace eular

#endif // __EULAR_UTILS_THREAD_H__
