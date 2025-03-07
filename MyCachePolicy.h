#pragma once
namespace MyCache{

template <typename Key,typename Value>
class MyCachePolicy {

	// 缓存策略接口有：put,get

public:
	virtual void put(Key key, Value value) = 0;

	virtual Value get(Key key) = 0;

	virtual bool get(Key key, Value& value) = 0;

	virtual ~MyCachePolicy() {};

};


}