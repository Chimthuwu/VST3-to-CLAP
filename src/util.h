// vst3-to-clap - small shared helpers
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "pluginterfaces/base/ftypes.h"
#include "pluginterfaces/vst/vsttypes.h"

namespace v3c {

// UTF-16 (VST3 String128 / TChar*) -> UTF-8
std::string toUtf8(const Steinberg::Vst::TChar* str);
// UTF-8 -> UTF-16 (null terminated)
std::u16string toUtf16(const std::string& str);

// Copy a std::string into a fixed-size C buffer, always null terminated.
void copyString(char* dst, size_t dstSize, const std::string& src);

// Lock-free single-producer / single-consumer ring buffer with a fixed capacity.
// Used to pass parameter changes between the main (UI) thread and the audio thread
// without allocating or locking on the audio thread.
template <typename T>
class SpscQueue
{
public:
	explicit SpscQueue(size_t capacity = 1024) { reset(capacity); }

	void reset(size_t capacity)
	{
		size_t cap = 2;
		while (cap < capacity + 1)
			cap <<= 1;
		buffer.assign(cap, T{});
		mask = cap - 1;
		head.store(0);
		tail.store(0);
	}

	bool push(const T& v)
	{
		const size_t h = head.load(std::memory_order_relaxed);
		const size_t next = (h + 1) & mask;
		if (next == tail.load(std::memory_order_acquire))
			return false; // full
		buffer[h] = v;
		head.store(next, std::memory_order_release);
		return true;
	}

	bool pop(T& out)
	{
		const size_t t = tail.load(std::memory_order_relaxed);
		if (t == head.load(std::memory_order_acquire))
			return false; // empty
		out = buffer[t];
		tail.store((t + 1) & mask, std::memory_order_release);
		return true;
	}

	bool empty() const
	{
		return tail.load(std::memory_order_acquire) == head.load(std::memory_order_acquire);
	}

private:
	std::vector<T> buffer;
	size_t mask = 0;
	std::atomic<size_t> head{0};
	std::atomic<size_t> tail{0};
};

} // namespace v3c
