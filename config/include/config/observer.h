/*************************************************************************
    > File Name: observer.h
    > Author: eular
    > Brief: 配置观察者模板
    > Created Time: Mon 22 Jun 2026 12:00:00 AM CST
 ************************************************************************/

#ifndef __CONFIG_OBSERVER_H__
#define __CONFIG_OBSERVER_H__

#include <stdint.h>
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace eular {

template<typename Parser, typename Node>
class ConfigObserver
{
public:
    using Callback = std::function<void(const std::string &path, const Node &node)>;

    uint64_t subscribe(const std::string &pattern, Callback callback)
    {
        std::vector<std::string> tokens;
        if (!callback || !parsePattern(pattern, tokens)) {
            return 0;
        }

        Subscription subscription;
        subscription.id = m_nextId++;
        subscription.pattern = pattern;
        subscription.tokens = tokens;
        subscription.callback = callback;
        m_subscriptions.push_back(subscription);
        m_subscriptionIndex[subscription.id] = m_subscriptions.size() - 1;
        insertSubscription(tokens, subscription.id);
        ++m_activeCount;
        return subscription.id;
    }

    bool unsubscribe(uint64_t id)
    {
        if (id == 0) {
            return false;
        }

        typename std::unordered_map<uint64_t, size_t>::iterator it = m_subscriptionIndex.find(id);
        if (it == m_subscriptionIndex.end()) {
            return false;
        }

        m_subscriptions[it->second].active = false;
        m_subscriptionIndex.erase(it);
        if (m_activeCount > 0) {
            --m_activeCount;
        }
        return true;
    }

    void clear()
    {
        m_subscriptions.clear();
        m_subscriptionIndex.clear();
        m_root.reset(new TrieNode());
        m_activeCount = 0;
    }

    size_t notify(const Parser &parser, const std::string &path) const
    {
        std::vector<std::string> pathTokens;
        if (!parsePath(path, pathTokens)) {
            return 0;
        }

        if (m_activeCount <= trieThreshold()) {
            return notifyLinear(parser, path, pathTokens);
        }

        return notifyTrie(parser, path, pathTokens);
    }

    size_t notify(const Parser &parser) const
    {
        size_t notified = 0;
        parser.foreachNode([this, &notified](const std::string &path, const std::vector<std::string> &pathTokens, const Node &node) {
            notified += notifyPreparedNode(path, pathTokens, node);
        });

        return notified;
    }

private:
    struct Subscription
    {
        uint64_t id = 0;
        std::string pattern;
        std::vector<std::string> tokens;
        Callback callback;
        bool active = true;
    };

    struct TrieNode
    {
        std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>> children;
        std::unique_ptr<TrieNode> plus;
        std::vector<uint64_t> exactSubscriptions;
        std::vector<uint64_t> hashSubscriptions;
    };

    static size_t trieThreshold()
    {
        return 16;
    }

    size_t notifyLinear(const Parser &parser, const std::string &path, const std::vector<std::string> &pathTokens) const
    {
        bool nodeLoaded = false;
        Node node;
        size_t notified = 0;
        for (typename std::vector<Subscription>::const_iterator it = m_subscriptions.begin();
             it != m_subscriptions.end(); ++it) {
            if (!it->active || !matchTokens(it->tokens, pathTokens)) {
                continue;
            }

            if (!nodeLoaded) {
                node = parser.getNode(path);
                nodeLoaded = true;
                if (!node.valid()) {
                    return 0;
                }
            }

            it->callback(path, node);
            ++notified;
        }

        return notified;
    }

    size_t notifyPreparedNode(const std::string &path, const std::vector<std::string> &pathTokens, const Node &node) const
    {
        if (!node.valid()) {
            return 0;
        }

        if (m_activeCount <= trieThreshold()) {
            return notifyPreparedNodeLinear(path, pathTokens, node);
        }

        return notifyPreparedNodeTrie(path, pathTokens, node);
    }

    size_t notifyPreparedNodeLinear(const std::string &path, const std::vector<std::string> &pathTokens, const Node &node) const
    {
        size_t notified = 0;
        for (typename std::vector<Subscription>::const_iterator it = m_subscriptions.begin();
             it != m_subscriptions.end(); ++it) {
            if (!it->active || !matchTokens(it->tokens, pathTokens)) {
                continue;
            }

            it->callback(path, node);
            ++notified;
        }

        return notified;
    }

    size_t notifyTrie(const Parser &parser, const std::string &path, const std::vector<std::string> &pathTokens) const
    {
        std::vector<uint64_t> matchedIds;
        matchSubscriptions(*m_root, pathTokens, 0, matchedIds);
        if (matchedIds.empty()) {
            return 0;
        }
        std::sort(matchedIds.begin(), matchedIds.end());
        matchedIds.erase(std::unique(matchedIds.begin(), matchedIds.end()), matchedIds.end());

        bool nodeLoaded = false;
        Node node;
        size_t notified = 0;
        for (typename std::vector<uint64_t>::const_iterator it = matchedIds.begin();
             it != matchedIds.end(); ++it) {
            typename std::unordered_map<uint64_t, size_t>::const_iterator indexIt = m_subscriptionIndex.find(*it);
            if (indexIt == m_subscriptionIndex.end()) {
                continue;
            }

            const Subscription &subscription = m_subscriptions[indexIt->second];
            if (!subscription.active) {
                continue;
            }

            if (!nodeLoaded) {
                node = parser.getNode(path);
                nodeLoaded = true;
                if (!node.valid()) {
                    return 0;
                }
            }

            subscription.callback(path, node);
            ++notified;
        }

        return notified;
    }

    size_t notifyPreparedNodeTrie(const std::string &path, const std::vector<std::string> &pathTokens, const Node &node) const
    {
        std::vector<uint64_t> matchedIds;
        matchSubscriptions(*m_root, pathTokens, 0, matchedIds);
        if (matchedIds.empty()) {
            return 0;
        }
        std::sort(matchedIds.begin(), matchedIds.end());
        matchedIds.erase(std::unique(matchedIds.begin(), matchedIds.end()), matchedIds.end());

        size_t notified = 0;
        for (typename std::vector<uint64_t>::const_iterator it = matchedIds.begin();
             it != matchedIds.end(); ++it) {
            typename std::unordered_map<uint64_t, size_t>::const_iterator indexIt = m_subscriptionIndex.find(*it);
            if (indexIt == m_subscriptionIndex.end()) {
                continue;
            }

            const Subscription &subscription = m_subscriptions[indexIt->second];
            if (!subscription.active) {
                continue;
            }

            subscription.callback(path, node);
            ++notified;
        }

        return notified;
    }

    static bool parsePattern(const std::string &pattern, std::vector<std::string> &tokens)
    {
        if (!parsePath(pattern, tokens)) {
            return false;
        }

        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "#") {
                return i + 1 == tokens.size();
            }
        }

        return true;
    }

    static bool matchTokens(const std::vector<std::string> &pattern, const std::vector<std::string> &path)
    {
        size_t pi = 0;
        size_t ti = 0;
        while (pi < pattern.size()) {
            if (pattern[pi] == "#") {
                return pi + 1 == pattern.size();
            }
            if (ti >= path.size()) {
                return false;
            }
            if (pattern[pi] != "+" && pattern[pi] != path[ti]) {
                return false;
            }
            ++pi;
            ++ti;
        }

        return ti == path.size();
    }

    static bool parsePath(const std::string &path, std::vector<std::string> &tokens)
    {
        tokens.clear();
        if (path.empty() || path[0] != '$') {
            return false;
        }

        if (path.size() == 1) {
            return true;
        }

        const std::string body = path.substr(1);
        if (body.empty() || body[0] == '.' || body[body.size() - 1] == '.') {
            return false;
        }

        size_t begin = 0;
        while (begin < body.size()) {
            size_t end = body.find('.', begin);
            if (end == std::string::npos) {
                end = body.size();
            }
            if (end == begin) {
                tokens.clear();
                return false;
            }

            tokens.push_back(body.substr(begin, end - begin));
            begin = end + 1;
        }

        return true;
    }

    void insertSubscription(const std::vector<std::string> &tokens, uint64_t id)
    {
        TrieNode *node = m_root.get();
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (tokens[i] == "#") {
                node->hashSubscriptions.push_back(id);
                return;
            }
            if (tokens[i] == "+") {
                if (!node->plus) {
                    node->plus.reset(new TrieNode());
                }
                node = node->plus.get();
                continue;
            }

            node = findOrCreateChild(*node, tokens[i]);
        }

        node->exactSubscriptions.push_back(id);
    }

    static TrieNode *findOrCreateChild(TrieNode &node, const std::string &token)
    {
        for (typename std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>>::iterator it = node.children.begin();
             it != node.children.end(); ++it) {
            if (it->first == token) {
                return it->second.get();
            }
        }

        node.children.push_back(std::make_pair(token, std::unique_ptr<TrieNode>(new TrieNode())));
        return node.children.back().second.get();
    }

    static const TrieNode *findChild(const TrieNode &node, const std::string &token)
    {
        for (typename std::vector<std::pair<std::string, std::unique_ptr<TrieNode>>>::const_iterator it = node.children.begin();
             it != node.children.end(); ++it) {
            if (it->first == token) {
                return it->second.get();
            }
        }

        return nullptr;
    }

    static void appendSubscriptions(const std::vector<uint64_t> &source, std::vector<uint64_t> &target)
    {
        target.insert(target.end(), source.begin(), source.end());
    }

    static void matchSubscriptions(const TrieNode &node,
                                   const std::vector<std::string> &path,
                                   size_t index,
                                   std::vector<uint64_t> &matches)
    {
        appendSubscriptions(node.hashSubscriptions, matches);

        if (index == path.size()) {
            appendSubscriptions(node.exactSubscriptions, matches);
            return;
        }

        const TrieNode *exactChild = findChild(node, path[index]);
        if (exactChild != nullptr) {
            matchSubscriptions(*exactChild, path, index + 1, matches);
        }

        if (node.plus) {
            matchSubscriptions(*node.plus, path, index + 1, matches);
        }
    }

private:
    std::vector<Subscription> m_subscriptions;
    std::unordered_map<uint64_t, size_t> m_subscriptionIndex;
    std::unique_ptr<TrieNode> m_root = std::unique_ptr<TrieNode>(new TrieNode());
    size_t m_activeCount = 0;
    uint64_t m_nextId = 1;
};

} // namespace eular

#endif // __CONFIG_OBSERVER_H__
