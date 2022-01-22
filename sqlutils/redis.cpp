/*************************************************************************
    > File Name: redis.cpp
    > Author: hsz
    > Brief:
    > Created Time: Mon 10 Jan 2022 03:12:48 PM CST
 ************************************************************************/

#define _DEBUG
#include "redis.h"
#include <utils/debug.h>
#include <utils/Errors.h>
#include <log/log.h>

#define LOG_TAG "redis"

#define REDIS_STATUS_OK 0                   // 正常
#define REDIS_STATUS_CONNECT_ERROR      -1  // 连接失败
#define REDIS_STATUS_AUTH_ERROR         -2  // 鉴权失败
#define REDIS_STATUS_QUERY_ERROR        -3  // 查询失败
#define REDIS_STATUS_NOT_CONNECTED      -4  // 未连接
#define REDIS_STATUS_UNAUTHENTICATED    -5  // 未鉴权
#define REDIS_STATUS_NOT_EXISTED        -6  // 键不存在

namespace eular {
RedisInterface::RedisInterface() :
    mRedisCtx(nullptr)
{

}

RedisInterface::RedisInterface(const String8 &ip, uint16_t port, const char *pwd) :
    mRedisCtx(nullptr)
{
    connect(ip, port, pwd);
}

RedisInterface::~RedisInterface()
{
    disconnect();
}

/**
 * @brief 连接redis数据库0
 * 
 * @param ip redis服务地址
 * @param port redis服务端口
 * @param pwd redis密码
 * @return int 成功返回0，连接失败/鉴权失败返回负值
 */
int RedisInterface::connect(const String8 &ip, uint16_t port, const char *pwd)
{
    if (mRedisCtx != nullptr) {
        return REDIS_STATUS_OK;
    }

    mRedisHost = ip;
    mRedisPort = port;
    mRedisPwd = pwd;

    mRedisCtx = redisConnect(mRedisHost.c_str(), mRedisPort);
    if (mRedisCtx == nullptr) {
        LOGE("%s() redisConnect (%s, %u) error.", __func__, mRedisHost.c_str(), mRedisPort);
        return REDIS_STATUS_CONNECT_ERROR;
    }

    if (mRedisCtx->err) {
        LOGE("%s() redisConnectWithTimeout error. %s", __func__, mRedisCtx->errstr);
        redisFree(mRedisCtx);
        mRedisCtx = nullptr;
        return REDIS_STATUS_CONNECT_ERROR;
    }

    if (authenticate(pwd)) {
        return REDIS_STATUS_OK;
    }

    return REDIS_STATUS_AUTH_ERROR;
}

int RedisInterface::connecttimeout(const String8 &ip, uint16_t port,
    const char *pwd, const struct timeval &timeout)
{
    if (mRedisCtx != nullptr) {
        return REDIS_STATUS_OK;
    }

    mRedisHost = ip;
    mRedisPort = port;
    mRedisPwd = pwd;

    mRedisCtx = redisConnectWithTimeout(ip.c_str(), port, timeout);
    if (mRedisCtx == nullptr) {
        LOGE("%s() redisConnect (%s, %u) error.", __func__, mRedisHost.c_str(), mRedisPort);
        return REDIS_STATUS_CONNECT_ERROR;
    }

    if (mRedisCtx->err) {
        LOGE("%s() redisConnectWithTimeout error. %s", __func__, mRedisCtx->errstr);
        redisFree(mRedisCtx);
        mRedisCtx = nullptr;
        return REDIS_STATUS_CONNECT_ERROR;
    }

    if (authenticate(pwd)) {
        return REDIS_STATUS_OK;
    }

    return REDIS_STATUS_AUTH_ERROR;
}

bool RedisInterface::disconnect()
{
    if (mRedisCtx) {
        redisFree(mRedisCtx);
        mRedisCtx = nullptr;
    }

    return true;
}

bool RedisInterface::authenticate(const char *pwd)
{
    if (pwd == nullptr) {
        return false;
    }

    redisReply *reply = nullptr;
    if (mRedisCtx) {
        static char buf[32];
        memset(buf, 0, sizeof(buf));
        snprintf(buf, sizeof(buf), "auth %s", pwd);
        reply = (redisReply *)redisCommand(mRedisCtx, buf);
        if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
            goto end;
        }

        freeReplyObject(reply);
        return true;
    }

end:
    int len = strlen(pwd);
    char tmpBuf[32] = {0};
    len = len > 31 ? 31 : len;
    memcpy(tmpBuf, pwd, len);
    if (len <= 4) {
        memset(tmpBuf, '*', len);
    } else {
        int begin = len / 2 - 2;
        memset(tmpBuf + begin, '*', 4);
    }

    if (reply) {
        freeReplyObject(reply);
        reply = nullptr;
    }
    LOGE("%s() auth %s error.\n", __func__, tmpBuf);
    return false;
}


RedisReply *RedisInterface::SqlCommand(const String8 &sql)
{
    if (mRedisCtx == nullptr) {
        return nullptr;
    }

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            LOGE("%s() sql [%s] error. %s", __func__, sql.c_str(), reply->str);
            freeReplyObject(reply);
        }
        return nullptr;
    }

    RedisReply *ret = new RedisReply(reply);
    freeReplyObject(reply);
    return ret;
}

/**
 * @brief 选择数据库，0 - 15
 * 
 * @param dbNum 
 * @return int 
 */
int RedisInterface::selectDB(uint16_t dbNum)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    if (dbNum > 15) {
        return INVALID_PARAM;
    }

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, "select %d", dbNum);
    if (reply == nullptr || reply->type != REDIS_REPLY_STATUS) {
        goto error;
    }

    if (strcasecmp("OK", reply->str) == 0) {
        freeReplyObject(reply);
        return REDIS_STATUS_OK;
    }

error:
    if (reply) {
        LOGE("%s() select database error. %s", __func__, reply->str);
        freeReplyObject(reply);
    }
    return REDIS_STATUS_QUERY_ERROR;
}

/**
 * @brief 显示匹配到的键，如keys *表示所有的键
 * 
 * @param pattern 模糊的键名
 * @return std::vector<String8> 返回所有匹配到的键
 */
std::vector<String8> RedisInterface::showKeys(const String8 &pattern)
{
    std::vector<String8> ret;

    return ret;
}

/**
 * @brief 设置键值
 * 
 * @param key 键
 * @param val 键的值
 * @return 成功返回0，失败返回负值
 */
int RedisInterface::setKeyValue(const String8 &key, const String8 &val)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, "set %s %s", key.c_str(), val.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_STATUS) {
        goto error;
    }

    if (strcasecmp(reply->str, "OK") == 0) {
        freeReplyObject(reply);
        return REDIS_STATUS_OK;
    }

error:
    if (reply) {
        LOGE("%s() set %s failed. %s", __func__, key.c_str(), reply->str);
        freeReplyObject(reply);
    }
    return REDIS_STATUS_QUERY_ERROR;
}

String8 RedisInterface::getKeyValue(const String8 &key)
{
    if (mRedisCtx == nullptr) {
        return "";
    }

    String8 ret;
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, "get %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        if (reply) {
            LOGE("%s() get error. %s", __func__, reply->str);
            freeReplyObject(reply);
        }
        return ret;
    }

    ret = reply->str;
    freeReplyObject(reply);
    return ret;
}

int RedisInterface::getKeyValue(const std::vector<String8> &keyVec, std::vector<String8> &valVec)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    if (keyVec.size() == 0) {
        return INVALID_PARAM;
    }

    String8 sql = "mget ";
    for (const auto &it : keyVec) {
        sql.appendFormat("%s ", it.c_str());
    }
    LOG("%s() sql \"%s\"\n", __func__, sql.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) {
            LOGE("%s() mget error. %s", __func__, reply->str);
            freeReplyObject(reply);
        }
        return REDIS_STATUS_QUERY_ERROR;
    }

    redisReply *curr = nullptr;
    for (int i = 0; i < reply->elements; ++i) {
        curr = reply->element[i];
        if (curr != nullptr) {
            valVec.push_back(curr->str);
        }
    }
}

/**
 * @brief 判断键key是否存在
 * 
 * @param key 键名
 * @return true 
 * @return false 
 */
bool RedisInterface::isKeyExist(const String8 &key)
{
    if (mRedisCtx == nullptr) {
        return false;
    }

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, "exists %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        goto error;
    }

    if (reply->integer) {
        freeReplyObject(reply);
        return true;
    }

error:
    if (reply) {
        freeReplyObject(reply);
    }
    return false;
}

bool RedisInterface::delKey(const String8 &key)
{
    if (mRedisCtx == nullptr) {
        return false;
    }

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, "del %s", key.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        goto error;
    }

    if (reply->integer) {
        freeReplyObject(reply);
        return true;
    }

error:
    if (reply) {
        freeReplyObject(reply);
    }
    return false;
}

/**
 * @brief 设置键的失效时间
 * 
 * @param key 键名
 * @param milliseconds 取决于isTimeStamp (为0会删除此键)
 * @param isTimeStamp isTimeStamp ? 是一个时间戳 : 多久后失效
 * @return true 成功
 * @return false 失败
 */
bool RedisInterface::setKeyLifeCycle(const String8 &key, uint64_t milliseconds, bool isTimeStamp)
{
    if (mRedisCtx == nullptr) {
        return false;
    }

    String8 sql;
    if (isTimeStamp) { // 时间戳
        sql.appendFormat("expireat %s %lu", key.c_str(), milliseconds);
    } else { // 生存时间
        sql.appendFormat("pexpire %s %lu", key.c_str(), milliseconds);
    }
    LOG("%s() %s\n", __func__, sql.c_str());

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        goto error;
    }
    if (reply->integer) {
        freeReplyObject(reply);
        return true;
    }

error:
    if (reply) {
        LOGE("%s() failed to set expire time. %s", __func__, reply->str);
        freeReplyObject(reply);
    }
    return false;
}

bool RedisInterface::delKeyLifeCycle(const String8 &key)
{
    if (mRedisCtx == nullptr) {
        return false;
    }

    const String8 &sql = String8::format("persist %s", key.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        goto error;
    }

    freeReplyObject(reply);
    return true;

error:
    if (reply) {
        LOGE("%s() failed to delete expire time. %s", __func__, reply->str);
        freeReplyObject(reply);
    }
    return false;
}

/**
 * @brief 获取key的生存时间(毫秒)
 * 
 * @param key 键名
 * @return -1表示未设置生存时间，-2表示键不存在，其他负值未知; 成功返回大于0的值
 */
int64_t RedisInterface::getKeyTTLMS(const String8 &key)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    int64_t ret = 0;
    String8 sql = String8::format("pttl %s", key.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        goto error;
    }

    ret = reply->integer;
    freeReplyObject(reply);
    return ret;

error:
    if (reply) {
        LOGE("%s() failed to get expire time. %s", __func__, reply->str);
        freeReplyObject(reply);
    }
    return UNKNOWN_ERROR;
}

/**
 * @brief 创建/替换哈希表，如果不存在就创建，如果已存在就替换
 * 
 * @param key 哈希键名
 * @param filedValue 哈希字段及值
 * @return 成功返回0，失败返回负值
 */
int RedisInterface::hashCreateOrReplace(const String8 &key,
        const std::vector<std::pair<String8, String8>> &filedValue)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    String8 filedAndVal;
    for (const auto &it : filedValue) {
        filedAndVal.appendFormat("%s %s ", it.first.c_str(), it.second.c_str());
        LOG("%s() [%s]\n", __func__, filedAndVal.c_str());
    }

    const String8 &sql = String8::format("hmset %s %s", key.c_str(), filedAndVal.c_str());
    LOG("[%s] [%s]\n", key.c_str(), filedAndVal.c_str());
    LOG("%s() sql [%s]\n", __func__, sql.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_STATUS) {
        goto error;
    }

    if (strcasecmp("OK", reply->str) == 0) {
        return REDIS_STATUS_OK;
    }

error:
    if (reply) {
        LOGE("%s() query error. [%s,%s]", __func__, reply->str, mRedisCtx->errstr);
        freeReplyObject(reply);
    }
    return REDIS_STATUS_QUERY_ERROR;
}

/**
 * @brief 将哈希表 key 中的字段 field 的值设为 value，如果键值已存在，则修改其值, 不存在则添加一个新的字段
 * 
 * @param key 键名
 * @param filedValue 域-值
 * @return 成功返回0，失败返回负值
 */
int RedisInterface::hashTableSetFiledValue(const String8 &key, const String8 &filed, const String8 &value)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    const String8 &sql = String8::format("hset %s %s %s", key.c_str(), filed.c_str(), value.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) {
            LOGE("%s() query sql \"%s\" error. [%s,%s]", __func__, sql.c_str(), reply->str, mRedisCtx->errstr);
            freeReplyObject(reply);
        }

        return REDIS_STATUS_QUERY_ERROR;
    }

    return REDIS_STATUS_OK;
}

/**
 * @brief 获取存储在哈希表中指定字段的值
 * 
 * @param key 哈希表的键名
 * @param filed 哈希表中的字段名
 * @param ret 哈希表中的值输出位置
 * @return 成功返回0，失败返回负值
 */
int RedisInterface::hashGetKeyFiled(const String8 &key, const String8 &filed, String8 &ret)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    const String8 &sql = String8::format("hget %s %s", key.c_str(), filed.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_STRING) {
        if (reply) {
            LOGE("%s() query sql \"%s\" error. [%s,%s]", __func__, sql.c_str(), reply->str, mRedisCtx->errstr);
            freeReplyObject(reply);
        }

        return REDIS_STATUS_QUERY_ERROR;
    }

    ret = reply->str;
    freeReplyObject(reply);
    return REDIS_STATUS_OK;
}

/**
 * @brief 获取在哈希表中指定 key 的所有字段和值
 * 
 * @param key 哈希表对应的键名
 * @param ret 输出位置
 * @return int 成功返回查询到的对数，失败返回负值
 */
int RedisInterface::hashGetKeyAll(const String8 &key, std::vector<std::pair<String8, String8>> &ret)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    const String8 &sql = String8::format("hgetall %s", key.c_str());
    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_ARRAY) {
        if (reply) {
            LOGE("%s() query sql \"%s\" error. [%s,%s]", __func__, sql.c_str(), reply->str, mRedisCtx->errstr);
            freeReplyObject(reply);
        }

        return REDIS_STATUS_QUERY_ERROR;
    }
    int number = reply->elements / 2;
    LOG_ASSERT(reply->elements % 2 == 0, "redis fatal error: number of elements is not even");
    ret.clear();

    redisReply *filed, *value;
    for (int i = 0; i < reply->elements; i += 2) {
        filed = reply->element[i];
        value = reply->element[i + 1];
        if (value && filed) {
            ret.push_back(std::make_pair<String8, String8>(filed->str, value->str));
        }
    }

    freeReplyObject(reply);
    return number;
}

/**
 * @brief 删除一个或多个key下的字段
 * 
 * @param key 键名
 * @param filed 字段集合
 * @param fileds 字段的个数
 * @return int 
 */
int RedisInterface::hashDelKeyOrFiled(const String8 &key, const char **filed, uint32_t fileds)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    if (!filed) {
        return INVALID_PARAM;
    }

    String8 sql = String8::format("hdel %s ", key.c_str());
    for (uint32_t i = 0; i < fileds; ++i) {
        sql.appendFormat("%s ", filed[i]);
    }
    LOG("%s() query sql: [%s]\n", __func__, sql.c_str());

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) {
            LOGE("%s() query sql \"%s\" error. [%s,%s]", __func__, sql.c_str(), reply->str, mRedisCtx->errstr);
            freeReplyObject(reply);
        }

        return REDIS_STATUS_QUERY_ERROR;
    }

    if (reply->integer >=0 && reply->integer <= fileds) {   // hash删除域的返回值的范围
        freeReplyObject(reply);
        return REDIS_STATUS_OK;
    }

    LOG_ASSERT(false, "sql: [%s], reply: %lld", sql.c_str(), reply->integer);
    freeReplyObject(reply);
    return REDIS_STATUS_QUERY_ERROR;
}

int RedisInterface::hashDelKeyOrFiled(const String8 &key, const std::vector<String8> &filedVec)
{
    if (mRedisCtx == nullptr) {
        return REDIS_STATUS_NOT_CONNECTED;
    }

    String8 sql = String8::format("hdel %s ", key.c_str());
    for (auto it : filedVec) {
        sql.appendFormat("%s ", it.c_str());
    }

    LOG("%s() query sql: [%s]\n", __func__, sql.c_str());

    redisReply *reply = (redisReply *)redisCommand(mRedisCtx, sql.c_str());
    if (reply == nullptr || reply->type != REDIS_REPLY_INTEGER) {
        if (reply) {
            LOGE("%s() query sql \"%s\" error. [%s,%s]", __func__, sql.c_str(), reply->str, mRedisCtx->errstr);
            freeReplyObject(reply);
        }

        return REDIS_STATUS_QUERY_ERROR;
    }

    if (reply->integer >=0 && reply->integer <= filedVec.size()) {   // hash删除域的返回值的范围
        freeReplyObject(reply);
        return REDIS_STATUS_OK;
    }

    LOG_ASSERT(false, "sql: [%s], reply: %lld", sql.c_str(), reply->integer);
    freeReplyObject(reply);
    return REDIS_STATUS_QUERY_ERROR;
}

const char *RedisInterface::strerror(int no) const
{
    const char *ret = nullptr;
    static const char *msgArray[] = {
        "OK",
        "Connect Error",
        "Authentication Failed",
        "Query Error",
        "Not Connected",
        "Unauthenticated"
    };

    static const uint16_t arraySize = sizeof(msgArray) / sizeof(char *);
    
    int index = -no;
    if (index >= 0 && index < arraySize) {
        ret = msgArray[index];
    } else {
        ret = "Unknow Error";
    }

    return ret;
}

RedisReply::RedisReply(redisReply *reply) :
    isVaild(false)
{
    if (reply == nullptr) {
        return;
    }
    mReply.reset(reply, freeReplyObject);
    isVaild = true;
}

RedisReply::~RedisReply()
{

}

}