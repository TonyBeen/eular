/*************************************************************************
    > File Name: test_lru_cache.cc
    > Author: eular
    > Brief:
    > Created Time: Tue 24 Feb 2026 02:33:46 PM CST
 ************************************************************************/

#include <stdlib.h>
#include <stdio.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "catch/catch.hpp"
#include <utils/lru_cache.hpp>

namespace {

using SimpleKey = int;
using StringValue = const char*;

// 为了替换 AOSP 的 JenkinsHash 压测：用稳定的 64-bit FNV-1a 做“打散”
static uint64_t fnv1a64(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t h = 14695981039346656037ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= 1099511628211ull;
    }
    return h;
}

static uint32_t hash_int(int x) {
    uint64_t h = fnv1a64(&x, sizeof(x));
    return static_cast<uint32_t>(h ^ (h >> 32));
}

struct ComplexKey {
    int k;

    explicit ComplexKey(int k) : k(k) { instanceCount += 1; }
    ComplexKey(const ComplexKey& other) : k(other.k) { instanceCount += 1; }
    ~ComplexKey() { instanceCount -= 1; }

    bool operator==(const ComplexKey& other) const { return k == other.k; }
    bool operator!=(const ComplexKey& other) const { return k != other.k; }

    static ptrdiff_t instanceCount;
};

ptrdiff_t ComplexKey::instanceCount = 0;

struct ComplexValue {
    int v;

    explicit ComplexValue(int v) : v(v) { instanceCount += 1; }
    ComplexValue(const ComplexValue& other) : v(other.v) { instanceCount += 1; }
    ~ComplexValue() { instanceCount -= 1; }

    static ptrdiff_t instanceCount;
};

ptrdiff_t ComplexValue::instanceCount = 0;

// std::hash 特化：因为 LruCache 现在用 std::hash<TKey>
} // namespace

namespace std {
template <>
struct hash<ComplexKey> {
    size_t operator()(const ComplexKey& value) const noexcept {
        return std::hash<int>{}(value.k);
    }
};
} // namespace std

namespace {

struct KeyWithPointer {
    int* ptr{nullptr};
    bool operator==(const KeyWithPointer& other) const { return *ptr == *other.ptr; }
};

} // namespace

namespace std {
template <>
struct hash<KeyWithPointer> {
    size_t operator()(KeyWithPointer const& value) const noexcept {
        return std::hash<int>{}(*value.ptr);
    }
};
} // namespace std

namespace {

struct KeyFailsOnCopy : public ComplexKey {
public:
    KeyFailsOnCopy(const KeyFailsOnCopy& key) : ComplexKey(key) {
        FAIL("KeyFailsOnCopy should not be copied (get must not copy key)");
    }
    KeyFailsOnCopy(int key) : ComplexKey(key) {}
};

} // namespace

namespace std {
template <>
struct hash<KeyFailsOnCopy> {
    size_t operator()(KeyFailsOnCopy const& value) const noexcept {
        // 只读 base 的 k，不触发拷贝
        return std::hash<int>{}(value.k);
    }
};
} // namespace std

namespace {

class EntryRemovedCallback : public eular::OnEntryRemoved<SimpleKey, StringValue> {
public:
    EntryRemovedCallback() : callbackCount(0), lastKey(-1), lastValue(nullptr) {}
    void operator()(SimpleKey& k, StringValue& v) override {
        callbackCount += 1;
        lastKey = k;
        lastValue = v;
    }
    ptrdiff_t callbackCount;
    SimpleKey lastKey;
    StringValue lastValue;
};

class InvalidateKeyCallback : public eular::OnEntryRemoved<KeyWithPointer, StringValue> {
public:
    void operator()(KeyWithPointer& k, StringValue&) override {
        delete k.ptr;
        k.ptr = nullptr;
    }
};

struct LruCacheFixture {
    LruCacheFixture() {
        ComplexKey::instanceCount = 0;
        ComplexValue::instanceCount = 0;
    }

    ~LruCacheFixture() {
        CHECK(ComplexKey::instanceCount == 0);
        CHECK(ComplexValue::instanceCount == 0);
    }

    static void assertInstanceCount(ptrdiff_t keys, ptrdiff_t values) {
        if (keys != ComplexKey::instanceCount || values != ComplexValue::instanceCount) {
            FAIL("Expected " << keys << " keys and " << values
                             << " values but there were actually "
                             << ComplexKey::instanceCount << " keys and "
                             << ComplexValue::instanceCount << " values");
        }
    }
};

} // namespace

TEST_CASE_METHOD(LruCacheFixture, "Empty", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);

    CHECK(cache.get(0) == nullptr);
    CHECK(cache.size() == 0u);
}

TEST_CASE_METHOD(LruCacheFixture, "Simple", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    REQUIRE(std::string(*cache.get(1)) == "one");
    REQUIRE(std::string(*cache.get(2)) == "two");
    REQUIRE(std::string(*cache.get(3)) == "three");
    CHECK(cache.size() == 3u);
}

TEST_CASE_METHOD(LruCacheFixture, "MaxCapacity", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(2);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK(cache.get(1) == nullptr);
    REQUIRE(std::string(*cache.get(2)) == "two");
    REQUIRE(std::string(*cache.get(3)) == "three");
    CHECK(cache.size() == 2u);
}

TEST_CASE_METHOD(LruCacheFixture, "RemoveLru", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    cache.removeOldest();
    CHECK(cache.get(1) == nullptr);
    REQUIRE(std::string(*cache.get(2)) == "two");
    REQUIRE(std::string(*cache.get(3)) == "three");
    CHECK(cache.size() == 2u);
}

TEST_CASE_METHOD(LruCacheFixture, "GetUpdatesLru", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    REQUIRE(std::string(*cache.get(1)) == "one");
    cache.removeOldest();
    REQUIRE(std::string(*cache.get(1)) == "one");
    CHECK(cache.get(2) == nullptr);
    REQUIRE(std::string(*cache.get(3)) == "three");
    CHECK(cache.size() == 2u);
}

TEST_CASE_METHOD(LruCacheFixture, "StressTest", "[LruCache]") {
    const size_t kCacheSize = 512;
    eular::LruCache<SimpleKey, StringValue> cache(static_cast<uint32_t>(kCacheSize));
    const size_t kNumKeys = 16 * 1024;
    const size_t kNumIters = 100000;

    std::vector<char*> strings(kNumKeys);
    for (size_t i = 0; i < kNumKeys; i++) {
        strings[i] = (char*)malloc(16);
        snprintf(strings[i], 16, "%zu", i);
    }

#if defined(_WIN32)
    std::srand(12345);
#else
    srandom(12345);
#endif
    int hitCount = 0;
    for (size_t i = 0; i < kNumIters; i++) {
#if defined(_WIN32)
        int index = static_cast<int>(static_cast<size_t>(std::rand()) % kNumKeys);
#else
        int index = static_cast<int>(random() % kNumKeys);
#endif
        uint32_t key = hash_int(index);
        const char* val = nullptr;
        auto ptr = cache.get(static_cast<int>(key));
        if (ptr != nullptr) {
            val = *ptr;
        }
        if (val != nullptr) {
            CHECK(val == strings[index]); // 指针相等（和原测试一致）
            hitCount++;
        } else {
            cache.put(static_cast<int>(key), strings[index]);
        }
    }

    size_t expectedHitCount = kNumIters * kCacheSize / kNumKeys;
    CHECK(hitCount > int(expectedHitCount * 0.9));
    CHECK(hitCount < int(expectedHitCount * 1.1));
    CHECK(cache.size() == kCacheSize);

    for (size_t i = 0; i < kNumKeys; i++) {
        free((void*)strings[i]);
    }
}

TEST_CASE_METHOD(LruCacheFixture, "NoLeak", "[LruCache]") {
    eular::LruCache<ComplexKey, ComplexValue> cache(100);

    cache.put(ComplexKey(0), ComplexValue(0));
    cache.put(ComplexKey(1), ComplexValue(1));
    CHECK(cache.size() == 2u);
    assertInstanceCount(2, 2);
}

TEST_CASE_METHOD(LruCacheFixture, "Clear", "[LruCache]") {
    eular::LruCache<ComplexKey, ComplexValue> cache(100);

    cache.put(ComplexKey(0), ComplexValue(0));
    cache.put(ComplexKey(1), ComplexValue(1));
    CHECK(cache.size() == 2u);
    assertInstanceCount(2, 2);
    cache.clear();
    assertInstanceCount(0, 0);
}

TEST_CASE_METHOD(LruCacheFixture, "ClearNoDoubleFree", "[LruCache]") {
    {
        eular::LruCache<ComplexKey, ComplexValue> cache(100);

        cache.put(ComplexKey(0), ComplexValue(0));
        cache.put(ComplexKey(1), ComplexValue(1));
        CHECK(cache.size() == 2u);
        assertInstanceCount(2, 2);
        cache.removeOldest();
        cache.clear();
        assertInstanceCount(0, 0);
    }
    assertInstanceCount(0, 0);
}

TEST_CASE_METHOD(LruCacheFixture, "ClearReuseOk", "[LruCache]") {
    eular::LruCache<ComplexKey, ComplexValue> cache(100);

    cache.put(ComplexKey(0), ComplexValue(0));
    cache.put(ComplexKey(1), ComplexValue(1));
    CHECK(cache.size() == 2u);
    assertInstanceCount(2, 2);
    cache.clear();
    assertInstanceCount(0, 0);
    cache.put(ComplexKey(0), ComplexValue(0));
    cache.put(ComplexKey(1), ComplexValue(1));
    CHECK(cache.size() == 2u);
    assertInstanceCount(2, 2);
}

TEST_CASE_METHOD(LruCacheFixture, "Callback", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);
    EntryRemovedCallback callback;
    cache.setOnEntryRemovedListener(&callback);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK(cache.size() == 3u);
    cache.removeOldest();
    CHECK(callback.callbackCount == 1);
    CHECK(callback.lastKey == 1);
    REQUIRE(std::string(callback.lastValue) == "one");
}

TEST_CASE_METHOD(LruCacheFixture, "CallbackOnClear", "[LruCache]") {
    eular::LruCache<SimpleKey, StringValue> cache(100);
    EntryRemovedCallback callback;
    cache.setOnEntryRemovedListener(&callback);

    cache.put(1, "one");
    cache.put(2, "two");
    cache.put(3, "three");
    CHECK(cache.size() == 3u);
    cache.clear();
    CHECK(callback.callbackCount == 3);
}

TEST_CASE_METHOD(LruCacheFixture, "CallbackRemovesKeyWorksOK", "[LruCache]") {
    eular::LruCache<KeyWithPointer, StringValue> cache(1);
    InvalidateKeyCallback callback;
    cache.setOnEntryRemovedListener(&callback);

    KeyWithPointer key1;
    key1.ptr = new int(1);
    KeyWithPointer key2;
    key2.ptr = new int(2);

    cache.put(key1, "one");
    // put 会触发淘汰，回调会 delete key1.ptr 并置空
    cache.put(key2, "two");
    CHECK(cache.size() == 1u);
    REQUIRE(std::string(*cache.get(key2)) == "two");
    cache.clear();
}

TEST_CASE_METHOD(LruCacheFixture, "IteratorCheck", "[LruCache]") {
    eular::LruCache<int, int> cache(100);

    cache.put(1, 4);
    cache.put(2, 5);
    cache.put(3, 6);
    CHECK(cache.size() == 3u);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        int v = it.value();
        CHECK(returnedValues.find(v) == returnedValues.end());
        returnedValues.insert(v);
    }
    CHECK(returnedValues == std::unordered_set<int>({4, 5, 6}));
}

TEST_CASE_METHOD(LruCacheFixture, "EmptyCacheIterator", "[LruCache]") {
    eular::LruCache<int, int> cache(100);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({}));
}

TEST_CASE_METHOD(LruCacheFixture, "OneElementCacheIterator", "[LruCache]") {
    eular::LruCache<int, int> cache(100);
    cache.put(1, 2);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({2}));
}

TEST_CASE_METHOD(LruCacheFixture, "OneElementCacheRemove", "[LruCache]") {
    eular::LruCache<int, int> cache(100);
    cache.put(1, 2);

    cache.remove(1);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({}));
}

TEST_CASE_METHOD(LruCacheFixture, "Remove", "[LruCache]") {
    eular::LruCache<int, int> cache(100);
    cache.put(1, 4);
    cache.put(2, 5);
    cache.put(3, 6);

    cache.remove(2);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({4, 6}));
}

TEST_CASE_METHOD(LruCacheFixture, "RemoveYoungest", "[LruCache]") {
    eular::LruCache<int, int> cache(100);
    cache.put(1, 4);
    cache.put(2, 5);
    cache.put(3, 6);

    cache.remove(3);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({4, 5}));
}

TEST_CASE_METHOD(LruCacheFixture, "RemoveNonMember", "[LruCache]") {
    eular::LruCache<int, int> cache(100);
    cache.put(1, 4);
    cache.put(2, 5);
    cache.put(3, 6);

    cache.remove(7);

    eular::LruCache<int, int>::Iterator it(cache);
    std::unordered_set<int> returnedValues;
    while (it.next()) {
        returnedValues.insert(it.value());
    }
    CHECK(returnedValues == std::unordered_set<int>({4, 5, 6}));
}

TEST_CASE_METHOD(LruCacheFixture, "DontCopyKeyInGet", "[LruCache]") {
    eular::LruCache<KeyFailsOnCopy, KeyFailsOnCopy> cache(1);
    // 如果 get 内部拷贝 key，会触发 KeyFailsOnCopy 的拷贝构造 FAIL
    cache.get(KeyFailsOnCopy(0));
}