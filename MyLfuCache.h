#pragma once

#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "MyCachePolicy.h"

namespace MyCache {


	template<typename Key, typename Value> class MyLfuCache;

	template<typename Key, typename Value>
	class Freqlist {
	private: // 类似于LRU但少了map，所以需要定义节点、头尾节点、频率
		typedef struct Node {
			int freq;
			Key key;
			Value value;
			std::shared_ptr<Node> prev;
			std::shared_ptr<Node> next;

			Node() :freq(1), prev(nullptr), next(nullptr) {};
			Node(Key key,Value value) :freq(1),key(key),value(value), prev(nullptr), next(nullptr) {};
		};

		using NodePtr = std::shared_ptr<Node>;
		int freq_;
		NodePtr head_;
		NodePtr tail_;

	public: // 提供给外部的方法：构造、加入、移除头部节点、判空
		explicit Freqlist(int n) :freq_(n) {  // explicit避免隐式转换，例如：vecotr<Freqlist> a; a.push_back(42)不会隐式调用构造方法，必须用 a.push_back(new Freqlist(42))
			head_ = std::make_shared<Node>(); // 调用Node的空参构造
			tail_ = std::make_shared<Node>();
			head_->next = tail_;
			tail_->prev = head_;
		}

		void addNode(NodePtr node) {
			if (!node || !head_ || !tail_)
				return;
			node->prev = tail_->prev;
			node->next = tail_;
			tail_->prev->next = node;
			tail_->prev = node;
		}

		void remove(NodePtr node) {
			if (!node || !head_ || !tail_)return;
			if (!node->prev || !node->next)return;

			node->prev->next = node->next;
			node->next->prev = node->prev;
			node->prev = nullptr;
			node->next = nullptr;
		}

		bool isEmpty() const{
			return head_->next == tail_;
		}

		NodePtr getFirstNode() const { return head_->next; }

		friend class MyLfuCache<Key, Value>;
	};


	template<typename Key, typename Value> 
	class MyLfuCache:public MyCachePolicy<Key,Value> {
		// 一个map保存lfu节点，一个map保存visCount对应的LRU
	public:
		using Node = typename Freqlist<Key, Value>::Node; // typename告诉编译器 依赖于模板的嵌套成员Node是一个类型，而不是变量或常量
		using NodePtr = std::shared_ptr<Node>;
		using NodeMap = std::unordered_map<Key, NodePtr>;
	private:
		int                                            capacity_; // 缓存容量
		int                                            minFreq_; // 最小访问频次(用于找到最小访问频次结点)
		int                                            maxAverageNum_; // 最大平均访问频次 !!!!!!
		int                                            curAverageNum_; // 当前平均访问频次
		int                                            curTotalNum_; // 当前访问所有缓存次数总数 
		std::mutex                                     mutex_; // 互斥锁
		NodeMap                                        nodeMap_; // key 到 缓存节点的映射
		std::unordered_map<int, Freqlist<Key, Value>*> freqToFreqList_;// 访问频次到该频次链表的映射

	public:
		MyLfuCache(int capacity, int maxAverageNum = 10) :capacity_(capacity), maxAverageNum_(maxAverageNum), minFreq_(INT8_MAX), curAverageNum_(0) {} // INT8_MAX?
		~MyLfuCache() override = default;

		void put(Key key,Value value) {
			if (capacity_ <= 0)return;
			std::lock_guard<std::mutex>lock(mutex_);
			auto it = nodeMap_.find(key);
			if (it != nodeMap_.end()) {
				it->second->value = value;
				getInternal(it->second, value); // 相当于访问一次,那为什么要把value传进去？
				return;
			}
			putInternal(key, value);
		}

		Value get(Key key) {
			Value value;
			get(key, value);
			return value;
		}

		bool get(Key key, Value& value) {
			std::lock_guard<std::mutex>lock(mutex_);
			auto it = nodeMap_.find(key);
			if (it != nodeMap_.end()) {
				value = it->second->value; // 直接改不就得了
				getInternal(it->second, value);
				return true;
			}
			return false;
		}

		// 清空缓存,回收资源 ??????????
		void purge()
		{
			nodeMap_.clear();
			freqToFreqList_.clear();
		}

	private:
		void putInternal(Key key, Value value){ // 添加缓存
			// 新建一个节点，先判断是否大于capacity,加入到对应的Freqlist
			NodePtr node = std::make_shared<Node>(key,value);
			if (nodeMap_.size() >= capacity_) { kickOut(); }
			nodeMap_[key] = node;
			addToFreqList(node);
		}
		void getInternal(NodePtr node, Value& value) { // 获取缓存
			removeFromFreqList(node);
			node->freq++;
			addToFreqList(node);
			if (node->freq == minFreq_ + 1 && freqToFreqList_[minFreq_]->isEmpty()) {
				minFreq_++;
			}
			addFreqNum();
		}

		void kickOut() { // 移除缓存中的过期数据
			NodePtr node = freqToFreqList_[minFreq_]->getFirstNode();
			nodeMap_.erase(node->key);
			removeFromFreqList(node);
			decreaseFreqNum(node->freq);
		}

		void removeFromFreqList(NodePtr node) { // 从频率列表中移除节点
			// 检查结点是否为空
			if (!node)
				return;

			auto freq = node->freq;
			freqToFreqList_[freq]->remove(node);
		}
		void addToFreqList(NodePtr node) { // 添加到频率列表
			// 检查结点是否为空
			if (!node)
				return;

			// 添加进入相应的频次链表前需要判断该频次链表是否存在
			auto freq = node->freq;
			if (freqToFreqList_.find(node->freq) == freqToFreqList_.end())
			{
				// 不存在则创建
				freqToFreqList_[node->freq] = new Freqlist<Key, Value>(node->freq);
			}

			freqToFreqList_[freq]->addNode(node);
		}

		void addFreqNum() { // 增加平均访问等频率
			curTotalNum_++;
			if (nodeMap_.empty())
				curAverageNum_ = 0;
			else
				curAverageNum_ = curTotalNum_ / nodeMap_.size();

			if (curAverageNum_ > maxAverageNum_)
			{
				handleOverMaxAverageNum();
			}
		}
		void decreaseFreqNum(int num) { // 减少平均访问等频率
			curTotalNum_ -= num;
			if (nodeMap_.empty())
				curAverageNum_ = 0;
			else
				curAverageNum_ = curTotalNum_ / nodeMap_.size();
		}
		void handleOverMaxAverageNum() { // 处理当前平均访问频率超过上限的情况 把所以不为空的节点都取出来，使其freq_ = min(1,freq_ - maxAverageNum_ / 2);
			if (nodeMap_.empty())
				return;

			// 当前平均访问频次已经超过了最大平均访问频次，所有结点的访问频次- (maxAverageNum_ / 2)
			for (auto it = nodeMap_.begin(); it != nodeMap_.end(); ++it)
			{
				// 检查结点是否为空
				if (!it->second)
					continue;

				NodePtr node = it->second;

				// 先从当前频率列表中移除
				removeFromFreqList(node);

				// 减少频率
				node->freq -= maxAverageNum_ / 2;
				if (node->freq < 1) node->freq = 1;

				// 添加到新的频率列表
				addToFreqList(node);
			}

			// 更新最小频率
			updateMinFreq();
		}
		void updateMinFreq() {
			minFreq_ = INT8_MAX;
			for (const auto& pair : freqToFreqList_) // ?? 为什么这样遍历？const是为了不让修改。 for(auto& [x,y] : freqToFreqList_)等同于for(auto& pair : freqToFreqList_)
			{
				if (pair.second && !pair.second->isEmpty())
				{
					minFreq_ = std::min(minFreq_, pair.first);
				}
			}
			if (minFreq_ == INT8_MAX)
				minFreq_ = 1;
		}
	};
}
