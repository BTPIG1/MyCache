#pragma once // 防止头文件被重复包含

#include <cstring>
#include <list> // 双向链表
#include <memory> // 提供智能指针
#include <mutex> // 互斥量
#include <unordered_map> // 无序map
#include "MyCachePolicy.h"

namespace MyCache {

	template<typename Key, typename Value> class MyLruCache;

	template <typename Key, typename Value>
	class MyLruNode {

	private:
		Key key_;
		Value value_;
		size_t visCount_;
		std::shared_ptr<MyLruNode<Key, Value>> prev_;
		std::shared_ptr<MyLruNode<Key, Value>> next_;


	public:
		MyLruNode(Key key, Value value) : key_(key), value_(value), visCount_(0), prev_(nullptr), next_(nullptr) {}

		Key getKey() {
			return key_;
		}

		Value getValue() {
			return value_;
		}

		void setValue(Value value) {
			value_ = value;
		}

		size_t getVisCount() {
			return visCount_;
		}

		void incrementVisCount() {
			this->visCount_++;
		}

		friend class MyLruCache<Key, Value>;
	};


	// LRU缓存策略: 容量、一个链表、一个哈希表、一个头节点、一个尾节点、一个互斥量
	template<typename Key, typename Value> 
	class MyLruCache: public MyCachePolicy<Key,Value> {

	public:
		using NodeType = MyLruNode<Key, Value>;
		using NodePtr = std::shared_ptr<NodeType>;
		using NodeMap = std::unordered_map<Key, NodePtr>;

	public: // 提供的外部方法：构造方法、put、get
		MyLruCache(int capacity): capacity_(capacity) {
			initialzeList();
		}

		~MyLruCache() override = default;

		void put(Key key, Value value) override{ // 判断key在不在链表中？在，则移到队头；否则先插入队头，size>capacity?是则弹出队尾。
			if (capacity_ < 0)return;

			std::lock_guard<std::mutex> lock(mtx); // 
			auto it = nodeMap_.find(key);
			if (it != nodeMap_.end()) {
				updateExistNode(it->second, value);
				return;
			}

			addNode(key, value);

		}

		Value get(Key key) override{
			Value value{}; // 这是在做什么？可以对template空参构造？
			get(key, value);
			return value;
		}

		bool get(Key key, Value& value) override{ // 上锁，map中找，找到先把节点移到最前，然后返回true
			std::lock_guard<std::mutex>(mutex_);
			auto it = nodeMap_.find(key);
			if (it != nodeMap_.end()) {
				moveToMostRecent(it->second);
				value = it->second->getValue();
				return true;
			}
			return false;
		}

	private:
		int capacity_;
		NodeMap nodeMap_;
		NodePtr dummyHead_;
		NodePtr dummyTail_;
		std::mutex	mutex_;

	private:
		void initialzeList() { // 初始化头尾节点
			dummyHead_ = std::make_shared<NodeType>(Key(), Value()); // 因为MyLruNode的构造方法只有一个所以必须传入默认值。
			dummyTail_ = std::make_shared<NodeType>(Key(), Value());
			dummyHead_->next_ = dummyTail_;
			dummyTail_->prev_ = dummyHead_;
		}

		void updateExistNode(NodePtr node,const Value& value) {
			node->setValue(value);
			moveToMostRecent(node);
		}

		void moveToMostRecent(NodePtr node) {
			removeNode(node);
			insertNode(node);
		}

		void removeNode(NodePtr node) {
			node->next_->prev_ = node->prev_;
			node->prev_->next_ = node->next_;
		}

		void insertNode(NodePtr node) { // 从尾部插入？尾插法？
			node->next_ = dummyTail_;
			node->prev_ = dummyTail_->prev_;
			dummyTail_->prev_->next_ = node;
			dummyTail_ - prev_ = node;
		}

		void addNode(const Key& key,const Value& value) { // 先直接插入到尾节点，如果capacity<0弹出最后一个
			NodePtr node = std::make_shared<NodeType>(key, value);
			insertNode(node);
			nodeMap_[key] = node;

			if (nodeMap_.size() > capacity_) { evictLeastRecent(); }
		}

		void evictLeastRecent() { // 弹出dummyHead_->next,并在nodeMap_中erase
			nodeptr& node = dummyHead_->next_;
			removeNode(node);
			nodeMap_.erase(node->getKey());
		}
	};

	template<typename Key,typename Value>
	class MyKLruCache :public MyLruCache<Key,Value> { // 在LRU基础上实现，维护一个历史访问队列，
	private:
		int k_;
		std::unique_ptr< MyLruCache<Key, size_t>> historyList_;

	public:
		MyKLruCache(int capactity, int historyCapactiy, int k)  // 倒数第k次访问时间最久的淘汰，维护一个历史队列，这个队列在这里也是根据LRU策略淘汰的
			:MyLruCache<Key,Value>(capactity), 
			historyList_(std::make_unique<MyLruCache<Key, size_t>>(historyCapactiy)), 
			k_(k) {};

		void put(Key key, Value value) { // 先判断是否在缓存中？在则直接put；否则先对历史队列操作是否需要弹出，在入队。
			if (MyLruCache<Key, Value>::get(key) != "")MyLruCache<Key, Value>::put(key, value); 

			int historyCount = historyList_->get(key);
			historyList_->put(key, ++historyCount); 

			if (historyCount >= k_) {
				historyList_->removeNode(key);
				MyLruCache<Key, Value>::put(key, value);
			}
		}

		Value get(Key key) { // 先到历史队列中更新次数，在去访问缓存。
			int historyCount = historyList_->get(key);

			if (historyCount >= k_) { // 我自己加的，为解决多次访问而没有写的问题
				historyList_->removeNode(key);
				MyLruCache<Key, Value>::put(key, value);
			}
			else {
				historyList_->put(key, ++historyCount); // 假设此处historyCount == k如何处理？
			}


			return MyLruCache<Key, Value>::get(key);// 上面会不会存在问题？不会有问题，想一想假如k=4，刚刚好第四次访问，此时不应该加入缓存应该是下一次
		}

	};

	template<typename Key,typename Value>
	class MyHashLru {
	private:
		size_t capacity_;
		int sliceNum_;
		std::vector<std::unique_ptr<MyKLruCache<Key, Value>>> lruSliceCaches_;

	public:
		MyHashLru(size_t capacity, int slice) :capacity_(capacity), sliceNum_(slice > 0 ? sliceNum : std::thread::hardware_concurrency()) {
			size_t size = std::ceil(capacity_ / static_cast<double>(sliceNum_));
			for (int i = 0; i < sliceNum_; i++) {
				lruSliceCaches_.emplace_back(std::make_unique<MyKLruCache<Key, Value>>(size));
			}
		}
		void put(Key key, Value value)
		{
			// 获取key的hash值，并计算出对应的分片索引
			size_t sliceIndex = Hash(key) % sliceNum_;
			return lruSliceCaches_[sliceIndex]->put(key, value);
		}

		bool get(Key key, Value& value)
		{
			// 获取key的hash值，并计算出对应的分片索引
			size_t sliceIndex = Hash(key) % sliceNum_;
			return lruSliceCaches_[sliceIndex]->get(key, value);
		}

		Value get(Key key)
		{
			Value value;
			memset(&value, 0, sizeof(value));
			get(key, value);
			return value;
		}
	private:
		// 将key转换为对应hash值
		size_t Hash(Key key)
		{
			std::hash<Key> hashFunc;
			return hashFunc(key);
		}
	};




}
