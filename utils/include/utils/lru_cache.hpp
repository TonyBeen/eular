#ifndef __UTILS_LRU_CACHE_H__
#define __UTILS_LRU_CACHE_H__

#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <unordered_set>
#include <utility>

namespace eular {

/**
 * GenerationCache callback used when an item is removed.
 */
template<typename EntryKey, typename EntryValue>
class OnEntryRemoved {
public:
    virtual ~OnEntryRemoved() = default;
    virtual void operator()(EntryKey& key, EntryValue& value) = 0;
};

template <typename TKey, typename TValue, typename Hash = std::hash<TKey>, typename KeyEqual = std::equal_to<TKey>>
class LruCache {
public:
    explicit LruCache(uint32_t maxCapacity);
    ~LruCache();

    enum Capacity : uint32_t {
        kUnlimitedCapacity = 0u, // 0 表示不限制
    };

    void setOnEntryRemovedListener(OnEntryRemoved<TKey, TValue>* listener);

    size_t size() const;
    const TValue *get(const TKey& key);
    bool put(const TKey& key, const TValue& value);
    bool remove(const TKey& key);
    bool removeOldest();
    void clear();
    const TValue* peekOldestValue() const;

private:
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;
    LruCache(LruCache&&) = delete;
    LruCache& operator=(LruCache&&) = delete;

    class KeyedEntry {
    public:
        virtual const TKey& getKey() const = 0;
        virtual ~KeyedEntry() = default;
    };

    class Entry final : public KeyedEntry {
    public:
        TKey key;
        TValue value;
        Entry* parent{nullptr};
        Entry* child{nullptr};

        Entry(const TKey& _key, const TValue& _value)
            : key(_key), value(_value) {}

        const TKey& getKey() const final { return key; }
    };

    class EntryForSearch : public KeyedEntry {
    public:
        explicit EntryForSearch(const TKey& key_) : key(key_) {}
        const TKey& getKey() const final { return key; }
    private:
        const TKey& key;
    };

    struct HashForEntry {
        size_t operator()(const KeyedEntry* entry) const noexcept {
            return Hash{}(entry->getKey());
        }
    };

    struct EqualityForHashedEntries {
        bool operator()(const KeyedEntry* lhs, const KeyedEntry* rhs) const noexcept {
            return KeyEqual{}(lhs->getKey(), rhs->getKey());
        }
    };

    using LruCacheSet = std::unordered_set<KeyedEntry*, HashForEntry, EqualityForHashedEntries>;

    void attachToCache(Entry& entry);
    void detachFromCache(Entry& entry);

    typename LruCacheSet::iterator findByKey(const TKey& key) {
        EntryForSearch probe(key);
        return mSet->find(&probe);
    }

    std::unique_ptr<LruCacheSet> mSet;
    OnEntryRemoved<TKey, TValue>* mListener{nullptr};
    Entry* mOldest{nullptr};
    Entry* mYoungest{nullptr};
    uint32_t mMaxCapacity{0};

public:
    class Iterator {
    public:
        explicit Iterator(const LruCache& cache)
            : mCache(cache), mIterator(mCache.mSet->begin()) {}

        bool next() {
            if (mIterator == mCache.mSet->end()) return false;
            if (mBeginReturned) {
                std::advance(mIterator, 1);
                if (mIterator == mCache.mSet->end()) return false;
            } else {
                mBeginReturned = true;
            }
            return true;
        }

        const TValue& value() const {
            return static_cast<Entry*>(*mIterator)->value;
        }

        const TKey& key() const {
            return (*mIterator)->getKey();
        }

    private:
        const LruCache& mCache;
        typename LruCacheSet::iterator mIterator;
        bool mBeginReturned{false};
    };
};

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
LruCache<TKey, TValue, Hash, KeyEqual>::LruCache(uint32_t maxCapacity)
    : mSet(new LruCacheSet())
    , mMaxCapacity(maxCapacity) {
    mSet->max_load_factor(1.0f);
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
LruCache<TKey, TValue, Hash, KeyEqual>::~LruCache() {
    clear();
}

template<typename TKey, typename TValue, typename Hash, typename KeyEqual>
void LruCache<TKey, TValue, Hash, KeyEqual>::setOnEntryRemovedListener(OnEntryRemoved<TKey, TValue>* listener) {
    mListener = listener;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
size_t LruCache<TKey, TValue, Hash, KeyEqual>::size() const {
    return mSet->size();
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
const TValue *LruCache<TKey, TValue, Hash, KeyEqual>::get(const TKey& key) {
    auto it = findByKey(key);
    if (it == mSet->end()) {
        return nullptr;
    }

    auto* entry = static_cast<Entry *>(*it);
    detachFromCache(*entry);
    attachToCache(*entry);
    return &entry->value;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
bool LruCache<TKey, TValue, Hash, KeyEqual>::put(const TKey& key, const TValue& value) {
    if (mMaxCapacity != kUnlimitedCapacity && size() >= mMaxCapacity) {
        removeOldest();
    }

    if (findByKey(key) != mSet->end()) {
        return false;
    }
    auto* newEntry = new Entry(key, value);
    mSet->insert(newEntry);
    attachToCache(*newEntry);
    return true;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
bool LruCache<TKey, TValue, Hash, KeyEqual>::remove(const TKey& key) {
    auto it = findByKey(key);
    if (it == mSet->end()) {
        return false;
    }
    auto* entry = static_cast<Entry*>(*it);
    mSet->erase(entry);
    if (mListener) {
        (*mListener)(entry->key, entry->value);
    }
    detachFromCache(*entry);
    delete entry;
    return true;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
bool LruCache<TKey, TValue, Hash, KeyEqual>::removeOldest() {
    if (mOldest) {
        return remove(mOldest->key);
    }
    return false;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
const TValue *LruCache<TKey, TValue, Hash, KeyEqual>::peekOldestValue() const {
    return mOldest ? &mOldest->value : nullptr;
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
void LruCache<TKey, TValue, Hash, KeyEqual>::clear() {
    // 先回调（按照 LRU 链表顺序），再释放 Entry
    if (mListener) {
        for (Entry* p = mOldest; p != nullptr; p = p->child) {
            (*mListener)(p->key, p->value);
        }
    }

    mYoungest = nullptr;
    mOldest = nullptr;

    for (auto* entry : *mSet) {
        delete entry;
    }
    mSet->clear();
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
void LruCache<TKey, TValue, Hash, KeyEqual>::attachToCache(Entry& entry) {
    entry.parent = nullptr;
    entry.child = nullptr;

    if (!mYoungest) {
        mYoungest = mOldest = &entry;
    } else {
        entry.parent = mYoungest;
        mYoungest->child = &entry;
        mYoungest = &entry;
    }
}

template <typename TKey, typename TValue, typename Hash, typename KeyEqual>
void LruCache<TKey, TValue, Hash, KeyEqual>::detachFromCache(Entry& entry) {
    if (entry.parent) {
        entry.parent->child = entry.child;
    } else {
        mOldest = entry.child;
    }

    if (entry.child) {
        entry.child->parent = entry.parent;
    } else {
        mYoungest = entry.parent;
    }

    entry.parent = nullptr;
    entry.child = nullptr;
}

} // namespace eular

#endif // __UTILS_LRU_CACHE_H__