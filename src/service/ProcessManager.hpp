// src/service/ProcessManager.hpp
#pragma once

template<typename K, typename V>
ThreadSafeLRUCache<K, V>::ThreadSafeLRUCache(size_t cap) : capacity(cap) {
    head = std::make_shared<Node>();
    tail = std::make_shared<Node>();
    head->next = tail;
    tail->prev = head;
}

template<typename K, typename V>
void ThreadSafeLRUCache<K, V>::moveToHead(std::shared_ptr<Node> node) {
    removeNode(node);
    addToHead(node);
}

template<typename K, typename V>
void ThreadSafeLRUCache<K, V>::removeNode(std::shared_ptr<Node> node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

template<typename K, typename V>
void ThreadSafeLRUCache<K, V>::addToHead(std::shared_ptr<Node> node) {
    node->prev = head;
    node->next = head->next;
    head->next->prev = node;
    head->next = node;
}

template<typename K, typename V>
std::shared_ptr<typename ThreadSafeLRUCache<K, V>::Node> ThreadSafeLRUCache<K, V>::removeTail() {
    auto node = tail->prev;
    removeNode(node);
    return node;
}

template<typename K, typename V>
std::optional<V> ThreadSafeLRUCache<K, V>::get(const K& key) const {
    std::shared_lock lock(mutex);

    auto it = cache.find(key);
    if (it == cache.end()) {
        return std::nullopt;
    }

    return it->second->value;
}

template<typename K, typename V>
void ThreadSafeLRUCache<K, V>::put(const K& key, const V& value) {
    std::unique_lock lock(mutex);

    auto it = cache.find(key);
    if (it != cache.end()) {
        it->second->value = value;
        moveToHead(it->second);
        return;
    }

    auto newNode = std::make_shared<Node>();
    newNode->key = key;
    newNode->value = value;

    cache[key] = newNode;
    addToHead(newNode);

    if (cache.size() > capacity) {
        auto removed = removeTail();
        cache.erase(removed->key);
    }
}

template<typename K, typename V>
void ThreadSafeLRUCache<K, V>::clear() {
    std::unique_lock lock(mutex);
    cache.clear();
    head->next = tail;
    tail->prev = head;
}

template<typename K, typename V>
size_t ThreadSafeLRUCache<K, V>::size() const {
    std::shared_lock lock(mutex);
    return cache.size();
}

template<typename K, typename V>
template<typename Func>
void ThreadSafeLRUCache<K, V>::forEach(Func func) const {
    std::shared_lock lock(mutex);
    for (const auto& [key, node] : cache) {
        func(key, node->value);
    }
}