/*************************************************************************
    > File Name: bitmap.h
    > Author: hsz
    > Brief:
    > Created Time: 2022-05-23 09:42:30 Monday
 ************************************************************************/

#ifndef __EULAR_BITMAP_H__
#define __EULAR_BITMAP_H__

#include <stdint.h>
#include <stdio.h>

#include <utils/sysdef.h>

#ifdef __CHAR_BIT__
#define BITS_PEER_BYTE      __CHAR_BIT__
#else
#define BITS_PEER_BYTE      (8)
#endif

namespace eular {
class UTILS_API BitMap final
{
public:
    BitMap();
    BitMap(uint32_t bitSize);
    BitMap(const BitMap &other);
    ~BitMap();

    BitMap &operator=(const BitMap &other);

    /**
     * @brief 设置idx位置的值为v
     * 
     * @param idx 索引
     * @param v 值
     * @return 成功返回true, 失败返回false
     */
    bool set(uint32_t idx, bool v);

    /**
     * @brief 获取idx位置的值
     * 
     * @param idx 索引
     * @return 返回索引位置的值
     */
    bool at(uint32_t idx) const;

    /**
     * @brief 清空所有的值, 重置为0
     */
    void clear();

    /**
     * @brief 统计BitMap中值为1的个数
     * 
     * @return 返回值为1的个数
     */
    uint32_t count() const;

    /**
     * @brief 获取当前bit位数, 通过resize设置
     * 
     * @return uint32_t 
     */
    uint32_t size() const;

    /**
     * @brief 获取BitMap容量
     * 
     * @return 返回实际容量
     */
    uint32_t capacity() const;

    /**
     * @brief 对BitMap进行扩容
     * 
     * @param bitSize 要扩容的大小, 大于容量时才会进行扩容
     * @return 成功返回true, 失败返回false
     */
    bool resize(uint32_t bitSize);

private:
    uint8_t *alloc(uint32_t bitSize);
    void release();
    void nullThrow() const;

private:
    uint8_t*    mBitMap;
    uint32_t    mSize;
    uint32_t    mCapacity;
};

} // namespace eular

#endif // __EULAR_BITMAP_H__
