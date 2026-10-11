// vst3-to-clap - Linux run loop bridge (see runloop.h)
#include "runloop.h"
#include "plugin.h"

#include <algorithm>

namespace v3c {

#if SMTG_OS_LINUX
using namespace Steinberg;

SharedRunLoop& SharedRunLoop::instance()
{
	static SharedRunLoop loop;
	return loop;
}

WrapperPlugin* SharedRunLoop::pickProvider(bool needFd) const
{
	for (auto* p : providers)
		if (needFd ? p->hostFdExt() != nullptr : p->hostTimerExt() != nullptr)
			return p;
	return nullptr;
}

bool SharedRunLoop::attach(FdEntry& e)
{
	if (e.provider)
		return true;
	WrapperPlugin* p = pickProvider(true);
	if (!p)
		return false;
	// the same fd may already be watched on this provider by another handler
	const bool watched = std::any_of(fds.begin(), fds.end(), [&](const FdEntry& o) {
		return &o != &e && o.provider == p && o.fd == e.fd;
	});
	if (!watched &&
	    !p->hostFdExt()->register_fd(p->getHost(), e.fd, CLAP_POSIX_FD_READ | CLAP_POSIX_FD_ERROR))
		return false;
	e.provider = p;
	return true;
}

bool SharedRunLoop::attach(TimerEntry& e)
{
	if (e.provider)
		return true;
	WrapperPlugin* p = pickProvider(false);
	if (!p)
		return false;
	clap_id id = CLAP_INVALID_ID;
	if (!p->hostTimerExt()->register_timer(p->getHost(), e.periodMs, &id))
		return false;
	e.provider = p;
	e.timerId = id;
	return true;
}

void SharedRunLoop::detach(FdEntry& e)
{
	if (!e.provider)
		return;
	WrapperPlugin* p = e.provider;
	e.provider = nullptr;
	const bool stillWatched = std::any_of(fds.begin(), fds.end(), [&](const FdEntry& o) {
		return &o != &e && o.provider == p && o.fd == e.fd;
	});
	if (!stillWatched && p->hostFdExt())
		p->hostFdExt()->unregister_fd(p->getHost(), e.fd);
}

void SharedRunLoop::detach(TimerEntry& e)
{
	if (!e.provider)
		return;
	if (e.provider->hostTimerExt())
		e.provider->hostTimerExt()->unregister_timer(e.provider->getHost(), e.timerId);
	e.provider = nullptr;
	e.timerId = CLAP_INVALID_ID;
}

void SharedRunLoop::addProvider(WrapperPlugin* p)
{
	if (std::find(providers.begin(), providers.end(), p) != providers.end())
		return;
	if (!p->hostTimerExt() && !p->hostFdExt())
		return;
	providers.push_back(p);
	// give orphaned registrations a home
	for (auto& e : fds)
		attach(e);
	for (auto& e : timers)
		attach(e);
}

void SharedRunLoop::removeProvider(WrapperPlugin* p)
{
	auto it = std::find(providers.begin(), providers.end(), p);
	if (it == providers.end())
		return;
	for (auto& e : fds)
		if (e.provider == p)
			detach(e);
	for (auto& e : timers)
		if (e.provider == p)
			detach(e);
	providers.erase(it);
	// move them to another instance, if there is one
	for (auto& e : fds)
		attach(e);
	for (auto& e : timers)
		attach(e);
}

void SharedRunLoop::onTimer(WrapperPlugin* p, clap_id id)
{
	for (auto& e : timers)
	{
		if (e.provider == p && e.timerId == id)
		{
			IPtr<Linux::ITimerHandler> h = e.handler; // the handler may unregister itself
			h->onTimer();
			return;
		}
	}
}

void SharedRunLoop::onFd(WrapperPlugin* p, int fd)
{
	std::vector<IPtr<Linux::IEventHandler>> hs;
	for (auto& e : fds)
		if (e.provider == p && e.fd == fd)
			hs.push_back(e.handler);
	for (auto& h : hs)
		h->onFDIsSet(fd);
}

tresult PLUGIN_API SharedRunLoop::registerEventHandler(Linux::IEventHandler* handler,
                                                       Linux::FileDescriptor fd)
{
	if (!handler || fd < 0)
		return kInvalidArgument;
	fds.push_back({IPtr<Linux::IEventHandler>(handler), fd, nullptr});
	attach(fds.back()); // may stay pending until an instance can host it
	return kResultTrue;
}

tresult PLUGIN_API SharedRunLoop::unregisterEventHandler(Linux::IEventHandler* handler)
{
	bool found = false;
	for (size_t i = 0; i < fds.size();)
	{
		if (fds[i].handler.get() == handler)
		{
			detach(fds[i]);
			fds.erase(fds.begin() + long(i));
			found = true;
		}
		else
			++i;
	}
	return found ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API SharedRunLoop::registerTimer(Linux::ITimerHandler* handler,
                                                Linux::TimerInterval ms)
{
	if (!handler)
		return kInvalidArgument;
	TimerEntry e;
	e.handler = handler;
	e.periodMs = uint32_t(std::clamp<Linux::TimerInterval>(ms, 1, 60000));
	timers.push_back(e);
	attach(timers.back());
	return kResultTrue;
}

tresult PLUGIN_API SharedRunLoop::unregisterTimer(Linux::ITimerHandler* handler)
{
	bool found = false;
	for (size_t i = 0; i < timers.size();)
	{
		if (timers[i].handler.get() == handler)
		{
			detach(timers[i]);
			timers.erase(timers.begin() + long(i));
			found = true;
		}
		else
			++i;
	}
	return found ? kResultTrue : kResultFalse;
}
#endif

} // namespace v3c
