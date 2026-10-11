// vst3-to-clap - Linux run loop bridge
//
// VST3 plug-ins on Linux have no event loop of their own: the host provides
// Steinberg::Linux::IRunLoop (fd watches + timers), usually through
// IPluginFactory3::setHostContext(), i.e. once per *module*, not per instance.
// CLAP provides the same services per *instance* (clap.posix-fd-support and
// clap.timer-support). This shared run loop accepts registrations from any VST3
// object and hosts them on whichever wrapper instance currently offers those
// extensions, moving them to another instance when that one is destroyed.
#pragma once

#include <clap/clap.h>

#include <vector>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"

namespace v3c {

class WrapperPlugin;

#if SMTG_OS_LINUX
class SharedRunLoop
: public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::Linux::IRunLoop>>
{
public:
	static SharedRunLoop& instance();

	// instances that can host timers/fds
	void addProvider(WrapperPlugin* p);
	void removeProvider(WrapperPlugin* p);

	// dispatch from clap.timer-support / clap.posix-fd-support
	void onTimer(WrapperPlugin* p, clap_id id);
	void onFd(WrapperPlugin* p, int fd);

	// IRunLoop
	Steinberg::tresult PLUGIN_API registerEventHandler(Steinberg::Linux::IEventHandler* handler,
	                                                   Steinberg::Linux::FileDescriptor fd) override;
	Steinberg::tresult PLUGIN_API unregisterEventHandler(
	    Steinberg::Linux::IEventHandler* handler) override;
	Steinberg::tresult PLUGIN_API registerTimer(Steinberg::Linux::ITimerHandler* handler,
	                                            Steinberg::Linux::TimerInterval ms) override;
	Steinberg::tresult PLUGIN_API unregisterTimer(Steinberg::Linux::ITimerHandler* handler) override;

	// never destroyed through reference counting
	Steinberg::uint32 PLUGIN_API addRef() override { return 1000; }
	Steinberg::uint32 PLUGIN_API release() override { return 1000; }

private:
	struct FdEntry
	{
		Steinberg::IPtr<Steinberg::Linux::IEventHandler> handler;
		int fd = -1;
		WrapperPlugin* provider = nullptr;
	};
	struct TimerEntry
	{
		Steinberg::IPtr<Steinberg::Linux::ITimerHandler> handler;
		uint32_t periodMs = 10;
		WrapperPlugin* provider = nullptr;
		clap_id timerId = CLAP_INVALID_ID;
	};

	bool attach(FdEntry& e);
	bool attach(TimerEntry& e);
	void detach(FdEntry& e);
	void detach(TimerEntry& e);
	WrapperPlugin* pickProvider(bool needFd) const;

	std::vector<WrapperPlugin*> providers;
	std::vector<FdEntry> fds;
	std::vector<TimerEntry> timers;
};
#endif

} // namespace v3c
