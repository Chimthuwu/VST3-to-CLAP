// vst3-to-clap - one CLAP plugin instance hosting one VST3 component/controller pair
#include "plugin.h"
#include "runloop.h"

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_set>
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
#include <xmmintrin.h>
#endif

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace v3c {

// Flush denormals to zero while the wrapped plug-in processes (restored afterwards).
struct DenormalGuard
{
#if defined(__SSE__) || defined(_M_X64) || defined(_M_IX86_FP)
	unsigned int saved = _mm_getcsr();
	DenormalGuard() { _mm_setcsr(saved | 0x8040); } // FTZ | DAZ
	~DenormalGuard() { _mm_setcsr(saved); }
#elif defined(__aarch64__)
	uint64_t saved = 0;
	DenormalGuard()
	{
		asm volatile("mrs %0, fpcr" : "=r"(saved));
		asm volatile("msr fpcr, %0" : : "r"(saved | (1ull << 24)));
	}
	~DenormalGuard() { asm volatile("msr fpcr, %0" : : "r"(saved)); }
#endif
};

//========================================================================
// util
//========================================================================
std::string toUtf8(const TChar* str)
{
	if (!str)
		return {};
	return Steinberg::Vst::StringConvert::convert(std::u16string(reinterpret_cast<const char16_t*>(str)));
}

std::u16string toUtf16(const std::string& str)
{
	return Steinberg::Vst::StringConvert::convert(str);
}

void copyString(char* dst, size_t dstSize, const std::string& src)
{
	if (!dst || dstSize == 0)
		return;
	size_t n = std::min(dstSize - 1, src.size());
	// do not cut a UTF-8 sequence in half
	while (n > 0 && n < src.size() && (static_cast<unsigned char>(src[n]) & 0xC0) == 0x80)
		--n;
	std::memcpy(dst, src.data(), n);
	dst[n] = 0;
}

//========================================================================
// ParamDesc
//========================================================================
double ParamDesc::toNormalized(double v) const
{
	if (steps > 0)
		return std::clamp(std::round(v) / double(steps), 0.0, 1.0);
	return std::clamp(v, 0.0, 1.0);
}

double ParamDesc::fromNormalized(double n) const
{
	n = std::clamp(n, 0.0, 1.0);
	if (steps > 0)
		return std::min<double>(steps, std::floor(n * (steps + 1)));
	return n;
}

//========================================================================
// VST3 host-side objects
//========================================================================
tresult PLUGIN_API ComponentHandler::beginEdit(ParamID id)
{
	if (owner)
		owner->vstBeginEdit(id);
	return kResultOk;
}
tresult PLUGIN_API ComponentHandler::performEdit(ParamID id, ParamValue v)
{
	if (owner)
		owner->vstPerformEdit(id, v);
	return kResultOk;
}
tresult PLUGIN_API ComponentHandler::endEdit(ParamID id)
{
	if (owner)
		owner->vstEndEdit(id);
	return kResultOk;
}
tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags)
{
	if (owner)
		owner->vstRestartComponent(flags);
	return kResultOk;
}
tresult PLUGIN_API ComponentHandler::setDirty(TBool state)
{
	if (owner && state)
		owner->vstSetDirty();
	return kResultOk;
}
tresult PLUGIN_API ComponentHandler::requestOpenEditor(FIDString)
{
	return kNotImplemented; // CLAP has no "open editor" request
}

tresult PLUGIN_API PlugFrame::resizeView(IPlugView* view, ViewRect* newSize)
{
	if (!owner || !newSize)
		return kResultFalse;
	return owner->vstResizeView(view, newSize) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API PlugFrame::queryInterface(const TUID iid, void** obj)
{
#if SMTG_OS_LINUX
	if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
	{
		*obj = static_cast<Linux::IRunLoop*>(&SharedRunLoop::instance());
		return kResultOk;
	}
#endif
	return Base::queryInterface(iid, obj);
}

tresult PLUGIN_API HostContext::getName(String128 name)
{
	Steinberg::Vst::StringConvert::convert(std::string("VST3-to-CLAP"), name);
	return kResultOk;
}

tresult PLUGIN_API HostContext::queryInterface(const TUID iid, void** obj)
{
#if SMTG_OS_LINUX
	if (FUnknownPrivate::iidEqual(iid, Linux::IRunLoop::iid))
	{
		*obj = static_cast<Linux::IRunLoop*>(&SharedRunLoop::instance());
		return kResultOk;
	}
#endif
	return HostApplication::queryInterface(iid, obj);
}


//========================================================================
// WrapperPlugin - construction / VST3 lifetime
//========================================================================
WrapperPlugin::WrapperPlugin(const clap_host_t* h, const PluginEntry* e) : host(h), entry(e)
{
	plugin.desc = &entry->descriptor;
	plugin.plugin_data = this;
	plugin.init = [](const clap_plugin_t* p) { return from(p)->init(); };
	plugin.destroy = [](const clap_plugin_t* p) { from(p)->destroy(); };
	plugin.activate = [](const clap_plugin_t* p, double sr, uint32_t minF, uint32_t maxF) {
		return from(p)->activate(sr, minF, maxF);
	};
	plugin.deactivate = [](const clap_plugin_t* p) { from(p)->deactivate(); };
	plugin.start_processing = [](const clap_plugin_t* p) { return from(p)->startProcessing(); };
	plugin.stop_processing = [](const clap_plugin_t* p) { from(p)->stopProcessing(); };
	plugin.reset = [](const clap_plugin_t* p) { from(p)->reset(); };
	plugin.process = [](const clap_plugin_t* p, const clap_process_t* pr) {
		return from(p)->process(pr);
	};
	plugin.get_extension = [](const clap_plugin_t* p, const char* id) {
		return from(p)->getExtension(id);
	};
	plugin.on_main_thread = [](const clap_plugin_t* p) { from(p)->onMainThread(); };

	for (auto& ch : activeNoteIds)
		ch.fill(-1);
	outQueue.reserve(8192); // never grows: pushOut() drops events beyond this
}

WrapperPlugin::~WrapperPlugin() = default;

void WrapperPlugin::log(clap_log_severity severity, const std::string& msg) const
{
	if (hostLog)
		hostLog->log(host, severity, ("[vst3-to-clap] " + msg).c_str());
}

bool WrapperPlugin::isMainThread() const
{
	if (hostThreadCheck)
		return hostThreadCheck->is_main_thread(host);
	return std::this_thread::get_id() == mainThreadId;
}

bool WrapperPlugin::init()
{
	mainThreadId = std::this_thread::get_id(); // clap_plugin::init runs on the main thread
	auto ext = [this](const char* id) { return host->get_extension(host, id); };
	hostParams = static_cast<const clap_host_params_t*>(ext(CLAP_EXT_PARAMS));
	hostLatency = static_cast<const clap_host_latency_t*>(ext(CLAP_EXT_LATENCY));
	hostTail = static_cast<const clap_host_tail_t*>(ext(CLAP_EXT_TAIL));
	hostGui = static_cast<const clap_host_gui_t*>(ext(CLAP_EXT_GUI));
	hostState = static_cast<const clap_host_state_t*>(ext(CLAP_EXT_STATE));
	hostLog = static_cast<const clap_host_log_t*>(ext(CLAP_EXT_LOG));
	hostAudioPorts = static_cast<const clap_host_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
	hostNotePorts = static_cast<const clap_host_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS));
	hostThreadCheck = static_cast<const clap_host_thread_check_t*>(ext(CLAP_EXT_THREAD_CHECK));
	hostTimer = static_cast<const clap_host_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
	hostFd = static_cast<const clap_host_posix_fd_support_t*>(ext(CLAP_EXT_POSIX_FD_SUPPORT));

#if SMTG_OS_LINUX
	SharedRunLoop::instance().addProvider(this);
#endif
	if (!createVst3())
	{
		releaseVst3();
		return false;
	}
	buildParams();
	return true;
}

bool WrapperPlugin::createVst3()
{
	std::string error;
	module = Registry::instance().acquireModule(entry->modulePath, error);
	if (!module)
	{
		log(CLAP_LOG_ERROR, "Could not load " + entry->modulePath + ": " + error);
		return false;
	}

	hostContext = std::make_unique<HostContext>(this);
	componentHandler = owned(new ComponentHandler(this));
	plugFrame = owned(new PlugFrame(this));

	const auto& factory = module->getFactory();
	component = factory.createInstance<IComponent>(entry->classId);
	if (!component)
	{
		log(CLAP_LOG_ERROR, "Could not create component " + entry->name);
		return false;
	}
	if (component->initialize(static_cast<FUnknown*>(hostContext.get())) != kResultOk)
	{
		log(CLAP_LOG_ERROR, "Component initialize failed for " + entry->name);
		return false;
	}
	componentInitialized = true;

	processor = U::cast<IAudioProcessor>(component);
	if (!processor)
	{
		log(CLAP_LOG_ERROR, entry->name + " has no IAudioProcessor");
		return false;
	}

	// controller: either the component itself, or a separate class
	controller = U::cast<IEditController>(component);
	if (controller)
	{
		singleComponent = true;
	}
	else
	{
		TUID controllerCid;
		if (component->getControllerClassId(controllerCid) == kResultTrue)
		{
			controller = factory.createInstance<IEditController>(VST3::UID(controllerCid));
			if (controller)
			{
				if (controller->initialize(static_cast<FUnknown*>(hostContext.get())) != kResultOk)
				{
					log(CLAP_LOG_WARNING, "Controller initialize failed for " + entry->name);
					controller = nullptr;
				}
				else
					controllerInitialized = true;
			}
		}
	}

	if (controller)
	{
		controller->setComponentHandler(componentHandler);

		if (!singleComponent)
		{
			auto componentCP = U::cast<IConnectionPoint>(component);
			auto controllerCP = U::cast<IConnectionPoint>(controller);
			if (componentCP && controllerCP)
			{
				componentProxy = owned(new ConnectionProxy(componentCP));
				controllerProxy = owned(new ConnectionProxy(controllerCP));
				componentProxy->connect(controllerCP);
				controllerProxy->connect(componentCP);
			}
		}

		// bring the controller up to date with the processor's default state
		auto stream = owned(new MemoryStream());
		if (component->getState(stream) == kResultOk)
		{
			stream->seek(0, IBStream::kIBSeekSet, nullptr);
			controller->setComponentState(stream);
		}
		midiMapping = U::cast<IMidiMapping>(controller);
	}
	else
	{
		log(CLAP_LOG_WARNING, entry->name + " has no edit controller; no parameters or GUI");
	}

	// default bus activation: everything on (CLAP exposes every bus as a port)
	for (auto dir : {kInput, kOutput})
	{
		for (auto type : {kAudio, kEvent})
		{
			const int32 n = component->getBusCount(type, dir);
			for (int32 i = 0; i < n; ++i)
				component->activateBus(type, dir, i, true);
		}
	}
	return true;
}

void WrapperPlugin::releaseVst3()
{
	if (view)
	{
		guiDestroy();
	}
	if (active)
		deactivate();

	if (componentProxy)
		componentProxy->disconnectAll();
	if (controllerProxy)
		controllerProxy->disconnectAll();
	componentProxy = nullptr;
	controllerProxy = nullptr;

	if (controller)
		controller->setComponentHandler(nullptr);
	midiMapping = nullptr;

	if (controller && controllerInitialized && !singleComponent)
		controller->terminate();
	controller = nullptr;
	processor = nullptr;
	if (component && componentInitialized)
		component->terminate();
	component = nullptr;

	if (componentHandler)
		componentHandler->detach();
	if (plugFrame)
		plugFrame->detach();
	componentHandler = nullptr;
	plugFrame = nullptr;
	if (hostContext)
		hostContext->detach();

	inParams = nullptr;
	outParams = nullptr;
	inEvents = nullptr;
	outEvents = nullptr;
	hostContext.reset();
#if SMTG_OS_LINUX
	SharedRunLoop::instance().removeProvider(this);
#endif
	module.reset(); // unloads the library if no other instance uses it
}

void WrapperPlugin::destroy()
{
	releaseVst3();
	delete this;
}

//========================================================================
// parameters
//========================================================================
void WrapperPlugin::buildUnitPaths(std::unordered_map<UnitID, std::string>& out)
{
	auto unitInfo = U::cast<IUnitInfo>(controller);
	if (!unitInfo)
		return;
	std::unordered_map<UnitID, std::pair<UnitID, std::string>> units;
	const int32 n = unitInfo->getUnitCount();
	for (int32 i = 0; i < n; ++i)
	{
		UnitInfo ui{};
		if (unitInfo->getUnitInfo(i, ui) == kResultOk)
			units[ui.id] = {ui.parentUnitId, toUtf8(ui.name)};
	}
	for (auto& kv : units)
	{
		std::string path;
		UnitID id = kv.first;
		int guard = 0;
		while (id != kRootUnitId && id != kNoParentUnitId && guard++ < 32)
		{
			auto it = units.find(id);
			if (it == units.end())
				break;
			path = path.empty() ? it->second.second : it->second.second + "/" + path;
			id = it->second.first;
		}
		out[kv.first] = path;
	}
}

void WrapperPlugin::rebuildMidiMap()
{
	if (!midiMap)
		midiMap.reset(new std::atomic<ParamID>[midiMapTableSize()]);
	for (size_t i = 0; i < midiMapTableSize(); ++i)
		midiMap[i].store(kNoParamId, std::memory_order_relaxed);
	if (!midiMapping || !component)
		return;
	const int32 eventBuses =
	    std::min<int32>(std::max<int32>(component->getBusCount(kEvent, kInput), 1), kMidiMapBuses);
	for (int32 bus = 0; bus < eventBuses; ++bus)
		for (int16 ch = 0; ch < 16; ++ch)
			for (CtrlNumber cc = 0; cc < kCountCtrlNumber; ++cc)
			{
				ParamID id = kNoParamId;
				if (midiMapping->getMidiControllerAssignment(bus, ch, cc, id) == kResultTrue)
					midiMap[(size_t(bus) * 16 + size_t(ch)) * kCountCtrlNumber + size_t(cc)].store(
					    id, std::memory_order_relaxed);
			}
}

void WrapperPlugin::describeParam(ParamDesc& d, const std::unordered_set<ParamID>& midiParams,
                                  const std::unordered_map<UnitID, std::string>& unitPaths)
{
	d.name = toUtf8(d.info.title);
	d.units = toUtf8(d.info.units);
	auto up = unitPaths.find(d.info.unitId);
	d.module = up != unitPaths.end() ? up->second : std::string();
	d.steps = std::max<int32>(d.info.stepCount, 0);

	const int32 f = d.info.flags;
	uint32_t cf = 0;
	if (f & ParameterInfo::kCanAutomate)
		cf |= CLAP_PARAM_IS_AUTOMATABLE;
	if (f & ParameterInfo::kIsReadOnly)
		cf |= CLAP_PARAM_IS_READONLY;
	if (f & ParameterInfo::kIsWrapAround)
		cf |= CLAP_PARAM_IS_PERIODIC;
	if (f & ParameterInfo::kIsHidden)
		cf |= CLAP_PARAM_IS_HIDDEN;
	if (f & ParameterInfo::kIsBypass)
	{
		cf |= CLAP_PARAM_IS_BYPASS;
		if (d.steps == 0)
			d.steps = 1; // CLAP requires bypass to be a stepped 0/1 parameter
	}
	if (d.steps > 0)
		cf |= CLAP_PARAM_IS_STEPPED;
	if ((f & ParameterInfo::kIsList) && d.steps > 0)
		cf |= CLAP_PARAM_IS_ENUM;
	d.midiMapped = midiParams.count(d.info.id) > 0;
	if (d.midiMapped)
	{
		// Parameters that only exist to receive MIDI CCs (IMidiMapping) are hidden from the
		// host; the wrapper feeds them from incoming MIDI instead.
		cf |= CLAP_PARAM_IS_HIDDEN;
		cf &= ~CLAP_PARAM_IS_AUTOMATABLE;
	}
	d.flags = cf;
}

std::unordered_set<ParamID> WrapperPlugin::midiMappedParams() const
{
	std::unordered_set<ParamID> ids;
	if (midiMap)
		for (size_t i = 0; i < midiMapTableSize(); ++i)
		{
			const ParamID id = midiMap[i].load(std::memory_order_relaxed);
			if (id != kNoParamId)
				ids.insert(id);
		}
	return ids;
}

void WrapperPlugin::buildParams()
{
	params.clear();
	paramIndex.clear();
	programChangeParam = kNoParamId;
	if (!controller)
		return;

	rebuildMidiMap();
	const auto midiParams = midiMappedParams();
	std::unordered_map<UnitID, std::string> unitPaths;
	buildUnitPaths(unitPaths);

	const int32 count = controller->getParameterCount();
	params.reserve(count);
	for (int32 i = 0; i < count; ++i)
	{
		ParamDesc d;
		if (controller->getParameterInfo(i, d.info) != kResultOk)
			continue;
		if (paramIndex.count(d.info.id))
			continue; // duplicate ids would break CLAP
		describeParam(d, midiParams, unitPaths);
		if ((d.info.flags & ParameterInfo::kIsProgramChange) && programChangeParam == kNoParamId)
			programChangeParam = d.info.id;
		paramIndex[d.info.id] = static_cast<uint32_t>(params.size());
		params.push_back(std::move(d));
	}
	pendingValue.assign(params.size(), 0.0);
	pendingDirty.assign(params.size(), 0);
	anyPending = false;
	valueCache.reset(new std::atomic<double>[params.size()]);
	refreshValueCache();
}

// Re-read names/units/flags of the existing parameters. Returns false if the set of
// parameters changed (which needs a full rebuild while deactivated).
bool WrapperPlugin::refreshParamInfos()
{
	if (!controller)
		return params.empty();
	const int32 count = controller->getParameterCount();
	const auto midiParams = midiMappedParams();
	std::unordered_map<UnitID, std::string> unitPaths;
	buildUnitPaths(unitPaths);
	size_t seen = 0;
	for (int32 i = 0; i < count; ++i)
	{
		ParameterInfo pi{};
		if (controller->getParameterInfo(i, pi) != kResultOk)
			continue;
		auto it = paramIndex.find(pi.id);
		if (it == paramIndex.end())
			return false;
		ParamDesc& d = params[it->second];
		if (pi.stepCount != d.info.stepCount)
			return false; // value range changed
		d.info = pi;
		describeParam(d, midiParams, unitPaths);
		++seen;
	}
	return seen == params.size();
}

bool WrapperPlugin::paramsGetInfo(uint32_t index, clap_param_info_t* info)
{
	if (index >= params.size() || !info)
		return false;
	const ParamDesc& d = params[index];
	std::memset(info, 0, sizeof(*info));
	info->id = d.info.id;
	info->flags = d.flags;
	info->cookie = nullptr;
	copyString(info->name, sizeof(info->name), d.name);
	copyString(info->module, sizeof(info->module), d.module);
	info->min_value = d.clapMin();
	info->max_value = d.clapMax();
	info->default_value = d.fromNormalized(d.info.defaultNormalizedValue);
	return true;
}

void WrapperPlugin::cacheValue(ParamID id, double normalized)
{
	auto it = paramIndex.find(id);
	if (it != paramIndex.end() && valueCache)
		valueCache[it->second].store(normalized, std::memory_order_relaxed);
}

void WrapperPlugin::refreshValueCache()
{
	if (!controller || !valueCache)
		return;
	for (size_t i = 0; i < params.size(); ++i)
		valueCache[i].store(controller->getParamNormalized(params[i].info.id),
		                    std::memory_order_relaxed);
}

bool WrapperPlugin::paramsGetValue(clap_id id, double* value)
{
	// Served from a cache that is updated synchronously by process()/flush() and GUI edits,
	// so the host sees automation immediately (the controller itself is only updated on the
	// main thread, a little later).
	auto it = paramIndex.find(id);
	if (it == paramIndex.end() || !valueCache || !value)
		return false;
	*value = params[it->second].fromNormalized(valueCache[it->second].load(std::memory_order_relaxed));
	return true;
}

bool WrapperPlugin::paramsValueToText(clap_id id, double value, char* display, uint32_t size)
{
	auto it = paramIndex.find(id);
	if (it == paramIndex.end() || !controller || !display || size == 0)
		return false;
	const ParamDesc& d = params[it->second];
	String128 str{};
	if (controller->getParamStringByValue(id, d.toNormalized(value), str) != kResultOk)
		return false;
	std::string text = toUtf8(str);
	if (!d.units.empty())
		text += " " + d.units;
	copyString(display, size, text);
	return true;
}

bool WrapperPlugin::paramsTextToValue(clap_id id, const char* display, double* value)
{
	auto it = paramIndex.find(id);
	if (it == paramIndex.end() || !controller || !display || !value)
		return false;
	const ParamDesc& d = params[it->second];
	auto tryParse = [&](const std::string& s) {
		std::u16string u = toUtf16(s);
		String128 buf{};
		const size_t n = std::min<size_t>(u.size(), 127);
		std::memcpy(buf, u.data(), n * sizeof(TChar));
		ParamValue norm = 0;
		if (controller->getParamValueByString(id, buf, norm) == kResultOk)
		{
			*value = d.fromNormalized(norm);
			return true;
		}
		return false;
	};
	// a parse is trusted only if formatting the result gives the same text back
	auto roundTrips = [&](double clapValue) {
		char back[256] = {};
		if (!paramsValueToText(id, clapValue, back, sizeof(back)))
			return false;
		return std::string(back) == display;
	};
	std::string s(display);
	double parsed = 0;
	bool haveParsed = false;
	if (tryParse(s))
	{
		parsed = *value;
		haveParsed = true;
		if (roundTrips(parsed))
			return true;
	}

	// Many plug-ins can format but not parse their values. Fall back to searching the
	// plug-in's own display strings: exact match first, otherwise the closest number.
	auto norm = [](std::string x) {
		while (!x.empty() && x.back() == ' ')
			x.pop_back();
		std::transform(x.begin(), x.end(), x.begin(),
		               [](unsigned char c) { return char(std::tolower(c)); });
		return x;
	};
	const std::string want = norm(s);
	if (d.steps > 0 && d.steps <= 4096)
	{
		for (int32 step = 0; step <= d.steps; ++step)
		{
			String128 str{};
			const double n = double(step) / d.steps;
			if (controller->getParamStringByValue(id, n, str) != kResultOk)
				continue;
			const std::string text = toUtf8(str);
			if (norm(text) == want || (!d.units.empty() && norm(text + " " + d.units) == want))
			{
				*value = double(step);
				return true;
			}
		}
	}
	else if (d.steps == 0)
	{
		auto leadingNumber = [](const std::string& t, double& out) {
			size_t i = t.find_first_of("+-.0123456789");
			if (i == std::string::npos)
				return false;
			char* end = nullptr;
			out = std::strtod(t.c_str() + i, &end);
			return end != t.c_str() + i;
		};
		auto shape = [](const std::string& t) { // text with digits/signs/points removed
			std::string r;
			for (char c : t)
				if (!std::strchr("+-.0123456789", c))
					r.push_back(c);
			return r;
		};
		double target = 0;
		const bool numeric = leadingNumber(want, target);
		const std::string wantShape = shape(want);
		double best = -1, bestErr = 1e300;
		auto probe = [&](double n) {
			String128 str{};
			if (controller->getParamStringByValue(id, n, str) != kResultOk)
				return false;
			const std::string text = toUtf8(str);
			if (norm(text) == want || (!d.units.empty() && norm(text + " " + d.units) == want))
			{
				best = n;
				bestErr = 0;
				return true;
			}
			double v = 0;
			if (numeric && leadingNumber(norm(text), v) && std::fabs(v - target) < bestErr &&
			    (shape(norm(text)) == wantShape ||
			     (!d.units.empty() && shape(norm(text + " " + d.units)) == wantShape)))
			{
				bestErr = std::fabs(v - target);
				best = n;
			}
			return false;
		};
		constexpr int kCoarse = 1000, kFine = 200;
		// zoom in around a centre, shrinking the window until the text matches exactly
		auto zoom = [&](double centre, double halfWidth) {
			for (int level = 0; level < 8 && bestErr > 0; ++level)
			{
				for (int i = 0; i <= kFine && bestErr > 0; ++i)
					if (probe(std::clamp(centre + halfWidth * (2.0 * i / kFine - 1.0), 0.0, 1.0)))
						return;
				if (best >= 0)
					centre = best;
				halfWidth /= 20.0;
			}
		};
		if (haveParsed) // the plug-in's own (inexact) parse is a good place to look first
			zoom(d.toNormalized(parsed), 1e-3);
		for (int i = 0; i <= kCoarse && bestErr > 0; ++i)
			if (probe(double(i) / kCoarse))
				break;
		if (best >= 0 && bestErr > 0)
			zoom(best, 1.0 / kCoarse);
		if (best >= 0)
		{
			*value = d.fromNormalized(best);
			return true;
		}
	}
	if (haveParsed)
	{
		*value = parsed;
		return true;
	}
	// retry without a trailing unit suffix ("3.5 dB" -> "3.5")
	if (!d.units.empty() && s.size() > d.units.size() &&
	    s.compare(s.size() - d.units.size(), d.units.size(), d.units) == 0)
	{
		std::string trimmed = s.substr(0, s.size() - d.units.size());
		while (!trimmed.empty() && trimmed.back() == ' ')
			trimmed.pop_back();
		if (tryParse(trimmed))
		{
			if (roundTrips(*value))
				return true;
			if (!haveParsed)
			{
				parsed = *value;
				haveParsed = true;
			}
		}
	}
	return false;
}

//------------------------------------------------------------------------
// The VST3 controller (GUI) edited a parameter. Main thread.
void WrapperPlugin::vstBeginEdit(ParamID id)
{
	guiToAudio.push({GuiEventKind::Begin, id, 0.0});
	if (hostParams && !processing)
		hostParams->request_flush(host);
}

void WrapperPlugin::vstPerformEdit(ParamID id, double normalized)
{
	cacheValue(id, normalized);
	guiToAudio.push({GuiEventKind::Value, id, normalized});
	if (hostParams && !processing)
		hostParams->request_flush(host);
}

void WrapperPlugin::vstEndEdit(ParamID id)
{
	guiToAudio.push({GuiEventKind::End, id, 0.0});
	if (hostParams && !processing)
		hostParams->request_flush(host);
}

void WrapperPlugin::vstSetDirty()
{
	if (hostState)
		hostState->mark_dirty(host);
}

void WrapperPlugin::vstRestartComponent(int32 flags)
{
	pendingRestartFlags.fetch_or(flags);
	if (isMainThread())
		onMainThread();
	else
		host->request_callback(host);
}

void WrapperPlugin::onMainThread()
{
	// The controller may call restartComponent() from inside setParamNormalized() etc.;
	// such nested requests are picked up by the loop below instead of recursing.
	if (inMainThreadCallback)
		return;
	inMainThreadCallback = true;
	mainCallbackRequested = false;
	for (int guard = 0; guard < 8; ++guard)
	{
		// processor/host parameter changes -> controller (keeps the GUI in sync)
		ParamUpdate u;
		while (audioToMain.pop(u))
			if (controller)
				controller->setParamNormalized(u.id, u.normalized);

		const int32 flags = pendingRestartFlags.exchange(0);
		if (!flags && audioToMain.empty())
			break;
		if (flags)
			handleRestartFlags(flags);
	}
	inMainThreadCallback = false;
}

void WrapperPlugin::handleRestartFlags(int32 flags)
{
	if (flags & kReloadComponent)
	{
		if (active)
			host->request_restart(host);
	}
	if (flags & kLatencyChanged)
	{
		// CLAP: clap_host_latency::changed() may only be called from within activate()
		latencyChanged = true;
		if (active)
			host->request_restart(host);
	}
	if (flags & kIoChanged)
	{
		if (active)
		{
			needsAudioPortsRescan = true;
			host->request_restart(host);
		}
		else
			rescanPorts();
	}
	if (flags & kMidiCCAssignmentChanged)
		rebuildMidiMap();
	if ((flags & kParamTitlesChanged) || (flags & kMidiCCAssignmentChanged))
	{
		// Names/flags are refreshed in place. If the parameter set itself changed, CLAP needs
		// a full rescan, which is only allowed while deactivated.
		const bool sameSet = refreshParamInfos();
		refreshValueCache();
		if (sameSet)
		{
			if (hostParams)
				hostParams->rescan(host, CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO |
				                             CLAP_PARAM_RESCAN_VALUES);
		}
		else if (active)
		{
			needsParamsRescanAll = true;
			host->request_restart(host);
		}
		else
		{
			buildParams();
			if (hostParams)
				hostParams->rescan(host, CLAP_PARAM_RESCAN_ALL);
		}
	}
	else if (flags & kParamValuesChanged)
	{
		refreshValueCache();
		if (hostParams)
			hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
	}
}

//========================================================================
// ports
//========================================================================
uint32_t WrapperPlugin::audioPortsCount(bool isInput)
{
	return component ? uint32_t(component->getBusCount(kAudio, isInput ? kInput : kOutput)) : 0;
}

bool WrapperPlugin::audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
	if (!component || !info)
		return false;
	BusInfo bi{};
	if (component->getBusInfo(kAudio, isInput ? kInput : kOutput, int32(index), bi) != kResultOk)
		return false;
	std::memset(info, 0, sizeof(*info));
	info->id = index;
	copyString(info->name, sizeof(info->name), toUtf8(bi.name));
	info->channel_count = uint32_t(std::max<int32>(bi.channelCount, 0));
	info->flags = 0;
	if (index == 0 && bi.busType == kMain)
		info->flags |= CLAP_AUDIO_PORT_IS_MAIN;
	info->port_type = bi.channelCount == 1   ? CLAP_PORT_MONO
	                  : bi.channelCount == 2 ? CLAP_PORT_STEREO
	                                         : nullptr;
	info->in_place_pair = CLAP_INVALID_ID;
	if (index == 0)
	{
		BusInfo other{};
		if (component->getBusInfo(kAudio, isInput ? kOutput : kInput, 0, other) == kResultOk &&
		    other.channelCount == bi.channelCount)
			info->in_place_pair = 0;
	}
	return true;
}

uint32_t WrapperPlugin::notePortsCount(bool isInput)
{
	return component ? uint32_t(component->getBusCount(kEvent, isInput ? kInput : kOutput)) : 0;
}

bool WrapperPlugin::notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info)
{
	if (!component || !info)
		return false;
	BusInfo bi{};
	if (component->getBusInfo(kEvent, isInput ? kInput : kOutput, int32(index), bi) != kResultOk)
		return false;
	std::memset(info, 0, sizeof(*info));
	info->id = index;
	info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
	info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
	copyString(info->name, sizeof(info->name), toUtf8(bi.name));
	return true;
}

//========================================================================
// activation
//========================================================================
bool WrapperPlugin::activate(double sr, uint32_t /*minFrames*/, uint32_t maxF)
{
	if (!processor || active)
		return false;
	sampleRate = sr;
	maxFrames = std::max<uint32_t>(maxF, 1);

	ProcessSetup setup{};
	setup.processMode = processMode;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock = int32(maxFrames);
	setup.sampleRate = sr;
	if (processor->setupProcessing(setup) != kResultOk)
	{
		log(CLAP_LOG_ERROR, "setupProcessing failed");
		return false;
	}

	// confirm the default speaker arrangements (some plug-ins expect this call)
	{
		const int32 nIn = component->getBusCount(kAudio, kInput);
		const int32 nOut = component->getBusCount(kAudio, kOutput);
		std::vector<SpeakerArrangement> ins(nIn), outs(nOut);
		for (int32 i = 0; i < nIn; ++i)
			processor->getBusArrangement(kInput, i, ins[i]);
		for (int32 i = 0; i < nOut; ++i)
			processor->getBusArrangement(kOutput, i, outs[i]);
		processor->setBusArrangements(ins.data(), nIn, outs.data(), nOut);
	}

	// buffers
	auto setupBuses = [&](BusDirection dir, std::vector<AudioBusBuffers>& buses,
	                      std::vector<std::vector<float*>>& ptrs) {
		const int32 n = component->getBusCount(kAudio, dir);
		buses.assign(n, AudioBusBuffers{});
		ptrs.assign(n, {});
		for (int32 i = 0; i < n; ++i)
		{
			BusInfo bi{};
			component->getBusInfo(kAudio, dir, i, bi);
			buses[i].numChannels = bi.channelCount;
			ptrs[i].assign(std::max<int32>(bi.channelCount, 0), nullptr);
			buses[i].channelBuffers32 = ptrs[i].empty() ? nullptr : ptrs[i].data();
		}
	};
	setupBuses(kInput, inBuses, inChannelPtrs);
	setupBuses(kOutput, outBuses, outChannelPtrs);
	zeroBuffer.assign(maxFrames, 0.f);
	size_t maxOutCh = 0;
	for (auto& p : outChannelPtrs)
		maxOutCh += p.size();
	scratchOut.assign(maxOutCh, std::vector<float>(maxFrames, 0.f));

	const int32 maxParamQueues = int32(params.size()) + 64;
	inParams = owned(new ParameterChanges(maxParamQueues));
	outParams = owned(new ParameterChanges(maxParamQueues));
	inEvents = owned(new EventList(4096));
	outEvents = owned(new EventList(4096));
	outQueue.clear();

	std::memset(&processData, 0, sizeof(processData));
	processData.processMode = processMode;
	processData.symbolicSampleSize = kSample32;
	processData.numInputs = int32(inBuses.size());
	processData.numOutputs = int32(outBuses.size());
	processData.inputs = inBuses.empty() ? nullptr : inBuses.data();
	processData.outputs = outBuses.empty() ? nullptr : outBuses.data();
	processData.inputParameterChanges = inParams;
	processData.outputParameterChanges = outParams;
	processData.inputEvents = inEvents;
	processData.outputEvents = outEvents;
	processData.processContext = &processContext;

	std::memset(&processContext, 0, sizeof(processContext));
	continuousSamples = 0;
	projectSamplesFallback = 0;
	for (auto& ch : activeNoteIds)
		ch.fill(-1);

	if (component->setActive(true) != kResultOk)
	{
		log(CLAP_LOG_ERROR, "setActive(true) failed");
		return false;
	}
	active = true;
	// VST3 hosts are expected to (re)query the latency after every activation
	latencySamples = processor->getLatencySamples();
	if (latencyChanged && !silentActivation)
	{
		latencyChanged = false;
		if (hostLatency)
			hostLatency->changed(host);
	}
	return true;
}

void WrapperPlugin::deactivate()
{
	if (!active)
		return;
	if (processing)
		stopProcessing();
	component->setActive(false);
	active = false;

	if (needsAudioPortsRescan)
	{
		needsAudioPortsRescan = false;
		rescanPorts();
	}
	if (needsParamsRescanAll)
	{
		needsParamsRescanAll = false;
		buildParams();
		if (hostParams)
			hostParams->rescan(host, CLAP_PARAM_RESCAN_ALL);
	}
}

// Host parameter changes made while deactivated only reach the VST3 processor with the next
// process() call. Before saving state we need them applied, so run a short private
// activate -> process(0 samples) -> deactivate cycle.
void WrapperPlugin::applyPendingWhileInactive()
{
	if (active || (!anyPending && !processorNeedsSync) || !processor)
		return;
	processorNeedsSync = false;
	silentActivation = true;
	const double sr = sampleRate > 0 ? sampleRate : 44100.0;
	const uint32_t mf = maxFrames > 0 ? maxFrames : 512;
	if (activate(sr, 1, mf))
	{
		processor->setProcessing(true);
		{
			DenormalGuard denormalGuard;
			inParams->clearQueue();
			outParams->clearQueue();
			inEvents->clear();
			outEvents->clear();
			drainPendingToProcessor();
			// one sample of silence: plug-ins that slice blocks skip parameters for 0 samples
			runSilentProcess(1);
			emitOutputs(nullptr); // keeps the value cache and controller queue up to date
		}
		processor->setProcessing(false);
		component->setActive(false);
		active = false;
	}
	silentActivation = false;
	ParamUpdate u;
	while (audioToMain.pop(u))
		if (controller)
			controller->setParamNormalized(u.id, u.normalized);
}

void WrapperPlugin::rescanPorts()
{
	if (hostAudioPorts &&
	    hostAudioPorts->is_rescan_flag_supported(host, CLAP_AUDIO_PORTS_RESCAN_LIST))
		hostAudioPorts->rescan(host, CLAP_AUDIO_PORTS_RESCAN_LIST);
	if (hostNotePorts &&
	    (hostNotePorts->supported_dialects(host) & (CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI)))
		hostNotePorts->rescan(host, CLAP_NOTE_PORTS_RESCAN_ALL);
}

bool WrapperPlugin::startProcessing()
{
	if (!active)
		return false;
	const tresult r = processor->setProcessing(true);
	if (r != kResultOk && r != kNotImplemented && r != kResultFalse)
		return false;
	processing = true;
	return true;
}

void WrapperPlugin::stopProcessing()
{
	if (!processing)
		return;
	processor->setProcessing(false);
	processing = false;
}

void WrapperPlugin::reset()
{
	// VST3 plug-ins reset their internal state when processing is switched on.
	if (processing)
	{
		processor->setProcessing(false);
		processor->setProcessing(true);
	}
	for (auto& ch : activeNoteIds)
		ch.fill(-1);
}

//========================================================================
// processing
//========================================================================
void WrapperPlugin::addParamChange(ParamID id, double normalized, int32_t offset)
{
	int32 queueIndex = 0;
	if (auto* q = inParams->addParameterData(id, queueIndex))
	{
		int32 pointIndex = 0;
		q->addPoint(offset, normalized, pointIndex);
	}
}

void WrapperPlugin::pushOut(const OutEvent& e)
{
	if (outQueue.size() < outQueue.capacity())
		outQueue.push_back(e);
}

int32_t WrapperPlugin::lookupNoteId(int16_t channel, int16_t key) const
{
	if (channel < 0 || channel > 15 || key < 0 || key > 127)
		return -1;
	return activeNoteIds[channel][key];
}

void WrapperPlugin::addNoteOn(int16_t port, int16_t channel, int16_t key, int32_t noteId,
                              float velocity, int32_t offset)
{
	if (key < 0 || key > 127)
		return;
	channel = int16_t(std::clamp<int16_t>(channel, 0, 15));
	if (noteId < 0)
		noteId = nextNoteId++ & 0x7FFFFFFF;
	activeNoteIds[channel][key] = noteId;

	Event e{};
	e.busIndex = port;
	e.sampleOffset = offset;
	e.type = Event::kNoteOnEvent;
	e.noteOn.channel = channel;
	e.noteOn.pitch = key;
	e.noteOn.tuning = 0.f;
	e.noteOn.velocity = velocity;
	e.noteOn.length = 0;
	e.noteOn.noteId = noteId;
	inEvents->addEvent(e);
}

void WrapperPlugin::addNoteOff(int16_t port, int16_t channel, int16_t key, int32_t noteId,
                               float velocity, int32_t offset)
{
	auto send = [&](int16_t ch, int16_t k) {
		int32_t id = noteId >= 0 ? noteId : activeNoteIds[ch][k];
		Event e{};
		e.busIndex = port;
		e.sampleOffset = offset;
		e.type = Event::kNoteOffEvent;
		e.noteOff.channel = ch;
		e.noteOff.pitch = k;
		e.noteOff.velocity = velocity;
		e.noteOff.noteId = id;
		e.noteOff.tuning = 0.f;
		inEvents->addEvent(e);
		activeNoteIds[ch][k] = -1;
	};

	if (key >= 0 && key <= 127 && channel >= 0 && channel <= 15)
	{
		send(channel, key);
		return;
	}
	// wildcard: match by note id and/or channel/key
	for (int16_t ch = 0; ch < 16; ++ch)
	{
		if (channel >= 0 && ch != channel)
			continue;
		for (int16_t k = 0; k < 128; ++k)
		{
			if (key >= 0 && k != key)
				continue;
			const int32_t id = activeNoteIds[ch][k];
			if (id < 0 || (noteId >= 0 && id != noteId))
				continue;
			send(ch, k);
		}
	}
}

void WrapperPlugin::addNoteExpression(const clap_event_note_expression_t* ev)
{
	auto forEachNote = [&](auto&& fn) {
		if (ev->note_id >= 0)
		{
			int16_t key = ev->key, ch = ev->channel;
			if (key < 0 || ch < 0)
			{
				for (int16_t c = 0; c < 16; ++c)
					for (int16_t k = 0; k < 128; ++k)
						if (activeNoteIds[c][k] == ev->note_id)
						{
							ch = c;
							key = k;
						}
			}
			fn(ev->note_id, ch, key);
			return;
		}
		for (int16_t c = 0; c < 16; ++c)
		{
			if (ev->channel >= 0 && c != ev->channel)
				continue;
			for (int16_t k = 0; k < 128; ++k)
			{
				if (ev->key >= 0 && k != ev->key)
					continue;
				if (activeNoteIds[c][k] >= 0)
					fn(activeNoteIds[c][k], c, k);
			}
		}
	};

	const int32_t offset = int32_t(ev->header.time);
	if (ev->expression_id == CLAP_NOTE_EXPRESSION_PRESSURE)
	{
		forEachNote([&](int32_t id, int16_t ch, int16_t key) {
			Event e{};
			e.busIndex = ev->port_index < 0 ? 0 : ev->port_index;
			e.sampleOffset = offset;
			e.type = Event::kPolyPressureEvent;
			e.polyPressure.channel = std::max<int16_t>(ch, 0);
			e.polyPressure.pitch = std::max<int16_t>(key, 0);
			e.polyPressure.pressure = float(std::clamp(ev->value, 0.0, 1.0));
			e.polyPressure.noteId = id;
			inEvents->addEvent(e);
		});
		return;
	}

	NoteExpressionTypeID type;
	double norm;
	switch (ev->expression_id)
	{
		case CLAP_NOTE_EXPRESSION_VOLUME: // 0..4 linear gain, VST3: 0.25 == 0 dB, 1 == +12 dB
			type = kVolumeTypeID;
			norm = ev->value / 4.0;
			break;
		case CLAP_NOTE_EXPRESSION_PAN:
			type = kPanTypeID;
			norm = ev->value;
			break;
		case CLAP_NOTE_EXPRESSION_TUNING: // semitones, VST3: +-120 semitones around 0.5
			type = kTuningTypeID;
			norm = 0.5 + ev->value / 240.0;
			break;
		case CLAP_NOTE_EXPRESSION_VIBRATO:
			type = kVibratoTypeID;
			norm = ev->value;
			break;
		case CLAP_NOTE_EXPRESSION_EXPRESSION:
			type = kExpressionTypeID;
			norm = ev->value;
			break;
		case CLAP_NOTE_EXPRESSION_BRIGHTNESS:
			type = kBrightnessTypeID;
			norm = ev->value;
			break;
		default:
			return;
	}
	norm = std::clamp(norm, 0.0, 1.0);
	forEachNote([&](int32_t id, int16_t, int16_t) {
		Event e{};
		e.busIndex = ev->port_index < 0 ? 0 : ev->port_index;
		e.sampleOffset = offset;
		e.type = Event::kNoteExpressionValueEvent;
		e.noteExpressionValue.typeId = type;
		e.noteExpressionValue.noteId = id;
		e.noteExpressionValue.value = norm;
		inEvents->addEvent(e);
	});
}

void WrapperPlugin::handleMidi(uint16_t port, const uint8_t d[3], int32_t offset)
{
	const uint8_t status = d[0] & 0xF0;
	const int16_t ch = d[0] & 0x0F;
	auto mapped = [&](CtrlNumber cc, double norm) {
		// IMidiMapping is a controller (UI thread) interface: use the table built from it
		if (port >= kMidiMapBuses || cc < 0 || cc >= kCountCtrlNumber)
			return;
		const ParamID id =
		    midiMap[(size_t(port) * 16 + size_t(ch)) * kCountCtrlNumber + size_t(cc)].load(
		        std::memory_order_relaxed);
		if (id != kNoParamId)
		{
			addParamChange(id, norm, offset);
			cacheValue(id, norm);
			audioToMain.push({id, norm});
		}
	};
	switch (status)
	{
		case 0x90:
			if (d[2] > 0)
			{
				addNoteOn(port, ch, d[1], -1, d[2] / 127.f, offset);
				break;
			}
			[[fallthrough]];
		case 0x80:
			addNoteOff(port, ch, d[1], -1, d[2] / 127.f, offset);
			break;
		case 0xA0:
		{
			Event e{};
			e.busIndex = port;
			e.sampleOffset = offset;
			e.type = Event::kPolyPressureEvent;
			e.polyPressure.channel = ch;
			e.polyPressure.pitch = d[1];
			e.polyPressure.pressure = d[2] / 127.f;
			e.polyPressure.noteId = lookupNoteId(ch, d[1]);
			inEvents->addEvent(e);
			break;
		}
		case 0xB0:
			mapped(CtrlNumber(d[1]), d[2] / 127.0);
			break;
		case 0xD0:
			mapped(kAfterTouch, d[1] / 127.0);
			break;
		case 0xE0:
			mapped(kPitchBend, double((d[2] << 7) | d[1]) / 16383.0);
			break;
		case 0xC0:
			if (programChangeParam != kNoParamId)
			{
				auto it = paramIndex.find(programChangeParam);
				if (it != paramIndex.end())
				{
					const ParamDesc& pd = params[it->second];
					const double steps = std::max<int32>(pd.steps, 1);
					const double norm = std::min(1.0, d[1] / steps);
					addParamChange(programChangeParam, norm, offset);
					valueCache[it->second].store(norm, std::memory_order_relaxed);
					audioToMain.push({programChangeParam, norm});
				}
			}
			break;
		default:
			break;
	}
}

void WrapperPlugin::handleInputEvent(const clap_event_header_t* hdr)
{
	if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID)
		return;
	const int32_t offset = int32_t(hdr->time);
	switch (hdr->type)
	{
		case CLAP_EVENT_NOTE_ON:
		{
			auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
			addNoteOn(ev->port_index < 0 ? 0 : ev->port_index, ev->channel < 0 ? 0 : ev->channel,
			          ev->key, ev->note_id, float(ev->velocity), offset);
			break;
		}
		case CLAP_EVENT_NOTE_OFF:
		case CLAP_EVENT_NOTE_CHOKE:
		{
			auto* ev = reinterpret_cast<const clap_event_note_t*>(hdr);
			addNoteOff(ev->port_index < 0 ? 0 : ev->port_index, ev->channel, ev->key, ev->note_id,
			           hdr->type == CLAP_EVENT_NOTE_CHOKE ? 0.f : float(ev->velocity), offset);
			break;
		}
		case CLAP_EVENT_NOTE_EXPRESSION:
			addNoteExpression(reinterpret_cast<const clap_event_note_expression_t*>(hdr));
			break;
		case CLAP_EVENT_PARAM_VALUE:
		{
			auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
			// VST3 has no per-note/per-channel parameters: only global automation applies
			if (ev->note_id != -1 || ev->key != -1 || ev->channel != -1)
				break;
			auto it = paramIndex.find(ev->param_id);
			if (it == paramIndex.end())
				break;
			const double norm = params[it->second].toNormalized(ev->value);
			addParamChange(ev->param_id, norm, offset);
			valueCache[it->second].store(norm, std::memory_order_relaxed);
			audioToMain.push({ev->param_id, norm});
			break;
		}
		case CLAP_EVENT_MIDI:
		{
			auto* ev = reinterpret_cast<const clap_event_midi_t*>(hdr);
			handleMidi(ev->port_index, ev->data, offset);
			break;
		}
		case CLAP_EVENT_MIDI_SYSEX:
		{
			auto* ev = reinterpret_cast<const clap_event_midi_sysex_t*>(hdr);
			Event e{};
			e.busIndex = ev->port_index;
			e.sampleOffset = offset;
			e.type = Event::kDataEvent;
			e.data.type = DataEvent::kMidiSysEx;
			e.data.size = ev->size;
			e.data.bytes = ev->buffer;
			inEvents->addEvent(e);
			break;
		}
		default:
			break;
	}
}

// GUI edits waiting in the queue: forward them to the host as gestures/values and,
// when `toProcessor`, into the processor's parameter changes.
void WrapperPlugin::drainGuiEvents(bool toProcessor)
{
	GuiEvent g;
	while (guiToAudio.pop(g))
	{
		auto it = paramIndex.find(g.id);
		if (it == paramIndex.end())
			continue;
		OutEvent e{};
		switch (g.kind)
		{
			case GuiEventKind::Begin:
			case GuiEventKind::End:
				e.paramGesture.header.size = sizeof(clap_event_param_gesture_t);
				e.paramGesture.header.type = g.kind == GuiEventKind::Begin
				                                 ? CLAP_EVENT_PARAM_GESTURE_BEGIN
				                                 : CLAP_EVENT_PARAM_GESTURE_END;
				e.paramGesture.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.paramGesture.header.time = 0;
				e.paramGesture.param_id = g.id;
				break;
			case GuiEventKind::Value:
				e.paramValue.header.size = sizeof(clap_event_param_value_t);
				e.paramValue.header.type = CLAP_EVENT_PARAM_VALUE;
				e.paramValue.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.paramValue.header.time = 0;
				e.paramValue.param_id = g.id;
				e.paramValue.cookie = nullptr;
				e.paramValue.note_id = -1;
				e.paramValue.port_index = -1;
				e.paramValue.channel = -1;
				e.paramValue.key = -1;
				e.paramValue.value = params[it->second].fromNormalized(g.normalized);
				if (toProcessor)
					addParamChange(g.id, g.normalized, 0);
				else
				{
					pendingValue[it->second] = g.normalized;
					pendingDirty[it->second] = 1;
					anyPending = true;
				}
				break;
		}
		pushOut(e);
	}
}

void WrapperPlugin::drainPendingToProcessor()
{
	if (!anyPending)
		return;
	for (size_t i = 0; i < params.size(); ++i)
	{
		if (pendingDirty[i])
		{
			addParamChange(params[i].info.id, pendingValue[i], 0);
			pendingDirty[i] = 0;
		}
	}
	anyPending = false;
}

void WrapperPlugin::emitOutputs(const clap_output_events_t* out)
{
	// processor -> host: output parameter changes
	if (outParams)
	{
		const int32 n = outParams->getParameterCount();
		for (int32 i = 0; i < n; ++i)
		{
			IParamValueQueue* q = outParams->getParameterData(i);
			if (!q)
				continue;
			const ParamID id = q->getParameterId();
			auto it = paramIndex.find(id);
			if (it == paramIndex.end())
				continue;
			const int32 points = q->getPointCount();
			for (int32 p = 0; p < points; ++p)
			{
				int32 offset = 0;
				ParamValue v = 0;
				if (q->getPoint(p, offset, v) != kResultOk)
					continue;
				OutEvent e{};
				e.paramValue.header.size = sizeof(clap_event_param_value_t);
				e.paramValue.header.type = CLAP_EVENT_PARAM_VALUE;
				e.paramValue.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.paramValue.header.time = clampTime(offset);
				e.paramValue.param_id = id;
				e.paramValue.note_id = -1;
				e.paramValue.port_index = -1;
				e.paramValue.channel = -1;
				e.paramValue.key = -1;
				e.paramValue.value = params[it->second].fromNormalized(v);
				pushOut(e);
				valueCache[it->second].store(v, std::memory_order_relaxed);
				audioToMain.push({id, v});
			}
		}
	}

	// processor -> host: note / MIDI output
	if (outEvents)
	{
		const int32 n = outEvents->getEventCount();
		for (int32 i = 0; i < n; ++i)
		{
			Event ve{};
			if (outEvents->getEvent(i, ve) != kResultOk)
				continue;
			OutEvent e{};
			const uint32_t time = clampTime(ve.sampleOffset);
			auto note = [&](uint16_t type, int16_t ch, int16_t key, int32_t id, double vel) {
				e.note.header.size = sizeof(clap_event_note_t);
				e.note.header.type = type;
				e.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.note.header.time = time;
				e.note.note_id = id;
				e.note.port_index = ve.busIndex;
				e.note.channel = ch;
				e.note.key = key;
				e.note.velocity = vel;
				pushOut(e);
			};
			auto midi = [&](uint8_t a, uint8_t b, uint8_t c) {
				e.midi.header.size = sizeof(clap_event_midi_t);
				e.midi.header.type = CLAP_EVENT_MIDI;
				e.midi.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
				e.midi.header.time = time;
				e.midi.port_index = uint16_t(ve.busIndex);
				e.midi.data[0] = a;
				e.midi.data[1] = b;
				e.midi.data[2] = c;
				pushOut(e);
			};
			switch (ve.type)
			{
				case Event::kNoteOnEvent:
					note(CLAP_EVENT_NOTE_ON, ve.noteOn.channel, ve.noteOn.pitch, ve.noteOn.noteId,
					     ve.noteOn.velocity);
					break;
				case Event::kNoteOffEvent:
					note(CLAP_EVENT_NOTE_OFF, ve.noteOff.channel, ve.noteOff.pitch,
					     ve.noteOff.noteId, ve.noteOff.velocity);
					break;
				case Event::kPolyPressureEvent:
					midi(uint8_t(0xA0 | (ve.polyPressure.channel & 0x0F)),
					     uint8_t(ve.polyPressure.pitch & 0x7F),
					     uint8_t(std::lround(std::clamp(ve.polyPressure.pressure, 0.f, 1.f) * 127)));
					break;
				case Event::kLegacyMIDICCOutEvent:
				{
					const auto& cc = ve.midiCCOut;
					const uint8_t ch = uint8_t(cc.channel & 0x0F);
					if (cc.controlNumber < 128)
						midi(0xB0 | ch, cc.controlNumber, uint8_t(cc.value & 0x7F));
					else if (cc.controlNumber == kAfterTouch)
						midi(0xD0 | ch, uint8_t(cc.value & 0x7F), 0);
					else if (cc.controlNumber == kPitchBend)
						midi(0xE0 | ch, uint8_t(cc.value & 0x7F), uint8_t(cc.value2 & 0x7F));
					else if (cc.controlNumber == kCtrlProgramChange)
						midi(0xC0 | ch, uint8_t(cc.value & 0x7F), 0);
					else if (cc.controlNumber == kCtrlPolyPressure)
						midi(0xA0 | ch, uint8_t(cc.value & 0x7F), uint8_t(cc.value2 & 0x7F));
					break;
				}
				default:
					break;
			}
		}
	}

	flushOutQueue(out);
}

void WrapperPlugin::flushOutQueue(const clap_output_events_t* out)
{
	// CLAP requires output events to be ordered by time (stable insertion sort, no allocation)
	for (size_t i = 1; i < outQueue.size(); ++i)
	{
		OutEvent tmp = outQueue[i];
		size_t j = i;
		while (j > 0 && outQueue[j - 1].header.time > tmp.header.time)
		{
			outQueue[j] = outQueue[j - 1];
			--j;
		}
		outQueue[j] = tmp;
	}
	if (out)
		for (auto& e : outQueue)
			out->try_push(out, &e.header);
	outQueue.clear();
}

void WrapperPlugin::fillProcessContext(const clap_event_transport_t* t, uint32_t frames)
{
	ProcessContext& c = processContext;
	c.state = 0;
	c.sampleRate = sampleRate;

	const auto now = std::chrono::steady_clock::now().time_since_epoch();
	c.systemTime = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
	c.state |= ProcessContext::kSystemTimeValid;
	c.continousTimeSamples = continuousSamples;
	c.state |= ProcessContext::kContTimeValid;

	bool playing = false;
	if (t)
	{
		playing = (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
		if (playing)
			c.state |= ProcessContext::kPlaying;
		if (t->flags & CLAP_TRANSPORT_IS_RECORDING)
			c.state |= ProcessContext::kRecording;
		if (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE)
			c.state |= ProcessContext::kCycleActive;

		if (t->flags & CLAP_TRANSPORT_HAS_TEMPO)
		{
			c.tempo = t->tempo;
			c.state |= ProcessContext::kTempoValid;
		}
		if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
		{
			c.timeSigNumerator = t->tsig_num;
			c.timeSigDenominator = t->tsig_denom;
			c.state |= ProcessContext::kTimeSigValid;
		}
		if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
		{
			c.projectTimeMusic = double(t->song_pos_beats) / double(CLAP_BEATTIME_FACTOR);
			c.barPositionMusic = double(t->bar_start) / double(CLAP_BEATTIME_FACTOR);
			c.state |= ProcessContext::kProjectTimeMusicValid | ProcessContext::kBarPositionValid;
			c.cycleStartMusic = double(t->loop_start_beats) / double(CLAP_BEATTIME_FACTOR);
			c.cycleEndMusic = double(t->loop_end_beats) / double(CLAP_BEATTIME_FACTOR);
			if (t->flags & CLAP_TRANSPORT_IS_LOOP_ACTIVE)
				c.state |= ProcessContext::kCycleValid;
		}
		if (t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
		{
			const double secs = double(t->song_pos_seconds) / double(CLAP_SECTIME_FACTOR);
			c.projectTimeSamples = TSamples(std::llround(secs * sampleRate));
		}
		else if ((t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) &&
		         (t->flags & CLAP_TRANSPORT_HAS_TEMPO) && t->tempo > 0)
		{
			c.projectTimeSamples =
			    TSamples(std::llround(c.projectTimeMusic * 60.0 / t->tempo * sampleRate));
		}
		else
			c.projectTimeSamples = projectSamplesFallback;
	}
	else
	{
		c.projectTimeSamples = projectSamplesFallback;
		c.tempo = 120.0;
		c.timeSigNumerator = 4;
		c.timeSigDenominator = 4;
	}

	continuousSamples += frames;
	if (playing)
		projectSamplesFallback = c.projectTimeSamples + frames;
}

clap_process_status WrapperPlugin::process(const clap_process_t* p)
{
	if (!active || !processor)
		return CLAP_PROCESS_ERROR;
	DenormalGuard denormalGuard;
	const uint32_t frames = std::min(p->frames_count, maxFrames);

	// --- audio buffers
	if (zeroBuffer.size() >= frames)
		std::fill(zeroBuffer.begin(), zeroBuffer.begin() + frames, 0.f);
	for (size_t b = 0; b < inBuses.size(); ++b)
	{
		auto& ptrs = inChannelPtrs[b];
		const clap_audio_buffer_t* cb = b < p->audio_inputs_count ? &p->audio_inputs[b] : nullptr;
		uint64 silence = 0;
		for (size_t c = 0; c < ptrs.size(); ++c)
		{
			float* src = (cb && cb->data32 && c < cb->channel_count) ? cb->data32[c] : nullptr;
			if (src)
			{
				ptrs[c] = src;
				if (c < 64 && (cb->constant_mask & (1ull << c)) && frames > 0 && src[0] == 0.f)
					silence |= (1ull << c);
			}
			else
			{
				ptrs[c] = zeroBuffer.data();
				if (c < 64)
					silence |= (1ull << c);
			}
		}
		inBuses[b].silenceFlags = silence;
	}
	size_t scratchIdx = 0;
	for (size_t b = 0; b < outBuses.size(); ++b)
	{
		auto& ptrs = outChannelPtrs[b];
		const clap_audio_buffer_t* cb = b < p->audio_outputs_count ? &p->audio_outputs[b] : nullptr;
		for (size_t c = 0; c < ptrs.size(); ++c)
		{
			float* dst = (cb && cb->data32 && c < cb->channel_count) ? cb->data32[c] : nullptr;
			ptrs[c] = dst ? dst : scratchOut[scratchIdx].data();
			// Some VST3 plug-ins leave outputs untouched (e.g. instruments in bypass); never
			// hand stale host memory back. Skipped for in-place buffers, which hold the input.
			bool inPlace = false;
			for (size_t ib = 0; ib < inChannelPtrs.size() && !inPlace; ++ib)
				for (float* ip : inChannelPtrs[ib])
					inPlace |= (ip == ptrs[c]);
			if (!inPlace)
				std::memset(ptrs[c], 0, sizeof(float) * frames);
			++scratchIdx;
		}
		outBuses[b].silenceFlags = 0;
	}

	// --- events
	inParams->clearQueue();
	outParams->clearQueue();
	inEvents->clear();
	outEvents->clear();

	drainPendingToProcessor();
	drainGuiEvents(true);

	if (p->in_events)
	{
		const uint32_t n = p->in_events->size(p->in_events);
		for (uint32_t i = 0; i < n; ++i)
			if (auto* hdr = p->in_events->get(p->in_events, i))
				handleInputEvent(hdr);
	}

	fillProcessContext(p->transport, frames);

	processData.processMode = processMode;
	processData.numSamples = int32(frames);
	currentFrames = frames;
	processor->process(processData); // result ignored: plug-ins disagree on kResultFalse

	// VST3 plug-ins may flag a channel silent without clearing it; CLAP wants the
	// buffer to really be constant, so clear flagged channels ourselves.
	for (size_t b = 0; b < outBuses.size() && b < p->audio_outputs_count; ++b)
	{
		uint64_t mask = 0;
		const auto& cb = p->audio_outputs[b];
		for (uint32_t c = 0; c < cb.channel_count && c < 64; ++c)
		{
			if ((outBuses[b].silenceFlags & (1ull << c)) && cb.data32 && cb.data32[c])
			{
				std::memset(cb.data32[c], 0, sizeof(float) * frames);
				mask |= (1ull << c);
			}
		}
		p->audio_outputs[b].constant_mask = mask;
	}

	emitOutputs(p->out_events);

	if (!audioToMain.empty() && !mainCallbackRequested.exchange(true))
		host->request_callback(host);

	return CLAP_PROCESS_CONTINUE;
}

bool WrapperPlugin::runSilentProcess(uint32_t frames)
{
	for (auto& b : inBuses)
		b.silenceFlags = ~uint64(0);
	if (zeroBuffer.size() >= frames)
		std::fill(zeroBuffer.begin(), zeroBuffer.begin() + frames, 0.f);
	for (size_t b = 0; b < inBuses.size(); ++b)
		for (auto& ptr : inChannelPtrs[b])
			ptr = zeroBuffer.data();
	size_t scratchIdx = 0;
	for (size_t b = 0; b < outBuses.size(); ++b)
		for (auto& ptr : outChannelPtrs[b])
			ptr = scratchOut[scratchIdx++].data();
	fillProcessContext(nullptr, frames);
	processData.numSamples = int32(frames);
	currentFrames = frames;
	return processor->process(processData) == kResultOk;
}

void WrapperPlugin::paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out)
{
	// CLAP: flush() is never called concurrently with process().
	// Values are handed to the processor with the next process() call. (A zero-sample
	// process() would be quicker, but many plug-ins ignore parameter changes in it.)
	drainGuiEvents(false);

	if (in)
	{
		const uint32_t n = in->size(in);
		for (uint32_t i = 0; i < n; ++i)
		{
			auto* hdr = in->get(in, i);
			if (!hdr || hdr->space_id != CLAP_CORE_EVENT_SPACE_ID ||
			    hdr->type != CLAP_EVENT_PARAM_VALUE)
				continue;
			auto* ev = reinterpret_cast<const clap_event_param_value_t*>(hdr);
			auto it = paramIndex.find(ev->param_id);
			if (it == paramIndex.end())
				continue;
			const double norm = params[it->second].toNormalized(ev->value);
			valueCache[it->second].store(norm, std::memory_order_relaxed);
			pendingValue[it->second] = norm;
			pendingDirty[it->second] = 1;
			anyPending = true;
			audioToMain.push({ev->param_id, norm});
		}
	}

	flushOutQueue(out); // only the GUI gestures/values collected above

	if (!audioToMain.empty())
	{
		if (!active && isMainThread())
			onMainThread();
		else if (!mainCallbackRequested.exchange(true))
			host->request_callback(host);
	}
}

//========================================================================
// state
//========================================================================
static constexpr char kStateMagic[4] = {'V', '3', 'C', 'W'};
static constexpr uint32_t kStateVersion = 1;

static bool writeAll(const clap_ostream_t* s, const void* data, uint64_t size)
{
	auto* p = static_cast<const uint8_t*>(data);
	while (size > 0)
	{
		const int64_t n = s->write(s, p, size);
		if (n <= 0)
			return false;
		p += n;
		size -= uint64_t(n);
	}
	return true;
}

static bool writeBlock(const clap_ostream_t* s, MemoryStream* m)
{
	const uint64_t size = m ? uint64_t(m->getSize()) : 0;
	uint8_t le[8];
	for (int i = 0; i < 8; ++i)
		le[i] = uint8_t(size >> (8 * i));
	if (!writeAll(s, le, 8))
		return false;
	return size == 0 || writeAll(s, m->getData(), size);
}

bool WrapperPlugin::stateSave(const clap_ostream_t* stream)
{
	if (!component || !stream)
		return false;
	applyPendingWhileInactive();
	auto compState = owned(new MemoryStream());
	if (component->getState(compState) != kResultOk)
		compState = owned(new MemoryStream()); // plug-ins without state are fine
	auto ctrlState = owned(new MemoryStream());
	if (!controller || controller->getState(ctrlState) != kResultOk)
		ctrlState = owned(new MemoryStream());

	uint8_t header[8];
	std::memcpy(header, kStateMagic, 4);
	for (int i = 0; i < 4; ++i)
		header[4 + i] = uint8_t(kStateVersion >> (8 * i));
	return writeAll(stream, header, sizeof(header)) && writeBlock(stream, compState) &&
	       writeBlock(stream, ctrlState);
}

bool WrapperPlugin::stateLoad(const clap_istream_t* stream)
{
	if (!component || !stream)
		return false;
	std::vector<uint8_t> data;
	uint8_t buf[16384];
	while (true)
	{
		const int64_t n = stream->read(stream, buf, sizeof(buf));
		if (n < 0)
			return false;
		if (n == 0)
			break;
		data.insert(data.end(), buf, buf + n);
	}

	const uint8_t* compData = nullptr;
	uint64_t compSize = 0;
	const uint8_t* ctrlData = nullptr;
	uint64_t ctrlSize = 0;

	auto readU64 = [&](size_t pos) {
		uint64_t v = 0;
		for (int i = 0; i < 8; ++i)
			v |= uint64_t(data[pos + i]) << (8 * i);
		return v;
	};
	if (data.size() >= 8 && std::memcmp(data.data(), kStateMagic, 4) == 0)
	{
		size_t pos = 8;
		if (pos + 8 > data.size())
			return false;
		compSize = readU64(pos);
		pos += 8;
		if (compSize > data.size() - pos)
			return false;
		compData = data.data() + pos;
		pos += size_t(compSize);
		if (pos + 8 <= data.size())
		{
			ctrlSize = readU64(pos);
			pos += 8;
			if (ctrlSize > data.size() - pos)
				return false;
			ctrlData = data.data() + pos;
		}
	}
	else
	{
		log(CLAP_LOG_ERROR, "state was not written by vst3-to-clap");
		return false;
	}

	if (compSize > 0)
	{
		auto s = owned(new MemoryStream(const_cast<uint8_t*>(compData), TSize(compSize)));
		if (component->setState(s) != kResultOk)
		{
			log(CLAP_LOG_WARNING, "component rejected state");
			return false;
		}
		if (controller)
		{
			s->seek(0, IBStream::kIBSeekSet, nullptr);
			controller->setComponentState(s);
		}
	}
	if (controller && ctrlSize > 0)
	{
		auto s = owned(new MemoryStream(const_cast<uint8_t*>(ctrlData), TSize(ctrlSize)));
		controller->setState(s);
	}

	// the processor got its values from setState: drop stale pending changes
	std::fill(pendingDirty.begin(), pendingDirty.end(), 0);
	anyPending = false;
	// many processors only apply a new state inside process(): make sure that happens before
	// the next save even if the plug-in is never activated in between
	if (!active)
		processorNeedsSync = true;
	refreshValueCache();
	if (hostParams)
		hostParams->rescan(host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
	return true;
}

//========================================================================
// latency / tail / render
//========================================================================
uint32_t WrapperPlugin::latencyGet()
{
	if (active)
		return latencySamples;
	return processor ? processor->getLatencySamples() : 0;
}

uint32_t WrapperPlugin::tailGet()
{
	if (!processor)
		return 0;
	const uint32 t = processor->getTailSamples();
	return t == kInfiniteTail ? UINT32_MAX : t;
}

bool WrapperPlugin::renderSet(clap_plugin_render_mode mode)
{
	processMode = mode == CLAP_RENDER_OFFLINE ? kOffline : kRealtime;
	// VST3 only picks up the process mode in setupProcessing(), i.e. on the next activation
	if (active)
		host->request_restart(host);
	return true;
}

//========================================================================
// GUI
//========================================================================
#if SMTG_OS_WINDOWS
static constexpr const char* kClapGuiApi = CLAP_WINDOW_API_WIN32;
static const FIDString kVstPlatform = kPlatformTypeHWND;
#elif SMTG_OS_MACOS
static constexpr const char* kClapGuiApi = CLAP_WINDOW_API_COCOA;
static const FIDString kVstPlatform = kPlatformTypeNSView;
#else
static constexpr const char* kClapGuiApi = CLAP_WINDOW_API_X11;
static const FIDString kVstPlatform = kPlatformTypeX11EmbedWindowID;
#endif

bool WrapperPlugin::guiIsApiSupported(const char* api, bool isFloating)
{
	return controller && !isFloating && api && std::strcmp(api, kClapGuiApi) == 0;
}

bool WrapperPlugin::guiGetPreferredApi(const char** api, bool* isFloating)
{
	if (!controller)
		return false;
	*api = kClapGuiApi;
	*isFloating = false;
	return true;
}

bool WrapperPlugin::guiCreate(const char* api, bool isFloating)
{
	if (!guiIsApiSupported(api, isFloating) || view)
		return false;
	IPlugView* v = controller->createView(ViewType::kEditor);
	if (!v)
		return false;
	view = owned(v);
	if (view->isPlatformTypeSupported(kVstPlatform) != kResultTrue)
	{
		view = nullptr;
		return false;
	}
	view->setFrame(plugFrame);
	if (guiScale != 1.0)
		guiSetScale(guiScale);
	return true;
}

void WrapperPlugin::guiDestroy()
{
	if (!view)
		return;
	if (viewAttached)
		view->removed();
	viewAttached = false;
	view->setFrame(nullptr);
	view = nullptr;
}

bool WrapperPlugin::guiSetScale(double scale)
{
	guiScale = scale;
#if SMTG_OS_MACOS
	return false; // cocoa works in logical points; the OS handles scaling
#else
	if (!view)
		return true;
	if (auto s = U::cast<IPlugViewContentScaleSupport>(view))
		return s->setContentScaleFactor(float(scale)) == kResultTrue;
	return false;
#endif
}

bool WrapperPlugin::guiGetSize(uint32_t* width, uint32_t* height)
{
	if (!view)
		return false;
	ViewRect r{};
	if (view->getSize(&r) != kResultTrue)
		return false;
	*width = uint32_t(std::max<int32>(r.getWidth(), 0));
	*height = uint32_t(std::max<int32>(r.getHeight(), 0));
	return true;
}

bool WrapperPlugin::guiCanResize()
{
	return view && view->canResize() == kResultTrue;
}

bool WrapperPlugin::guiGetResizeHints(clap_gui_resize_hints_t* hints)
{
	if (!view)
		return false;
	const bool r = guiCanResize();
	hints->can_resize_horizontally = r;
	hints->can_resize_vertically = r;
	hints->preserve_aspect_ratio = false;
	hints->aspect_ratio_width = 1;
	hints->aspect_ratio_height = 1;
	return true;
}

bool WrapperPlugin::guiAdjustSize(uint32_t* width, uint32_t* height)
{
	if (!view)
		return false;
	ViewRect r(0, 0, int32(*width), int32(*height));
	if (view->checkSizeConstraint(&r) != kResultTrue)
		return false;
	*width = uint32_t(std::max<int32>(r.getWidth(), 0));
	*height = uint32_t(std::max<int32>(r.getHeight(), 0));
	return true;
}

bool WrapperPlugin::guiSetSize(uint32_t width, uint32_t height)
{
	if (!view)
		return false;
	ViewRect r(0, 0, int32(width), int32(height));
	return view->onSize(&r) == kResultTrue;
}

bool WrapperPlugin::guiSetParent(const clap_window_t* window)
{
	if (!view || !window)
		return false;
	if (viewAttached)
	{
		view->removed();
		viewAttached = false;
	}
#if SMTG_OS_WINDOWS
	void* parent = window->win32;
#elif SMTG_OS_MACOS
	void* parent = window->cocoa;
#else
	void* parent = reinterpret_cast<void*>(static_cast<uintptr_t>(window->x11));
#endif
	if (view->attached(parent, kVstPlatform) != kResultTrue)
		return false;
	viewAttached = true;
	return true;
}

bool WrapperPlugin::vstResizeView(IPlugView* v, ViewRect* newSize)
{
	if (!v || v != view.get() || !hostGui)
		return false;
	const uint32_t w = uint32_t(std::max<int32>(newSize->getWidth(), 1));
	const uint32_t h = uint32_t(std::max<int32>(newSize->getHeight(), 1));
	if (!hostGui->request_resize(host, w, h))
		return false;
	// CLAP hosts don't necessarily call set_size() after accepting a resize request
	ViewRect r(0, 0, int32(w), int32(h));
	view->onSize(&r);
	return true;
}

void WrapperPlugin::onTimer(clap_id id)
{
#if SMTG_OS_LINUX
	SharedRunLoop::instance().onTimer(this, id);
#else
	(void)id;
#endif
}

void WrapperPlugin::onFd(int fd, clap_posix_fd_flags_t)
{
#if SMTG_OS_LINUX
	SharedRunLoop::instance().onFd(this, fd);
#else
	(void)fd;
#endif
}

//========================================================================
// extension tables
//========================================================================
static const clap_plugin_audio_ports_t kAudioPorts = {
    [](const clap_plugin_t* p, bool in) { return WrapperPlugin::from(p)->audioPortsCount(in); },
    [](const clap_plugin_t* p, uint32_t i, bool in, clap_audio_port_info_t* info) {
	    return WrapperPlugin::from(p)->audioPortsGet(i, in, info);
    },
};

static const clap_plugin_note_ports_t kNotePorts = {
    [](const clap_plugin_t* p, bool in) { return WrapperPlugin::from(p)->notePortsCount(in); },
    [](const clap_plugin_t* p, uint32_t i, bool in, clap_note_port_info_t* info) {
	    return WrapperPlugin::from(p)->notePortsGet(i, in, info);
    },
};

static const clap_plugin_params_t kParams = {
    [](const clap_plugin_t* p) { return WrapperPlugin::from(p)->paramsCount(); },
    [](const clap_plugin_t* p, uint32_t i, clap_param_info_t* info) {
	    return WrapperPlugin::from(p)->paramsGetInfo(i, info);
    },
    [](const clap_plugin_t* p, clap_id id, double* v) {
	    return WrapperPlugin::from(p)->paramsGetValue(id, v);
    },
    [](const clap_plugin_t* p, clap_id id, double v, char* d, uint32_t s) {
	    return WrapperPlugin::from(p)->paramsValueToText(id, v, d, s);
    },
    [](const clap_plugin_t* p, clap_id id, const char* d, double* v) {
	    return WrapperPlugin::from(p)->paramsTextToValue(id, d, v);
    },
    [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out) {
	    WrapperPlugin::from(p)->paramsFlush(in, out);
    },
};

static const clap_plugin_state_t kState = {
    [](const clap_plugin_t* p, const clap_ostream_t* s) {
	    return WrapperPlugin::from(p)->stateSave(s);
    },
    [](const clap_plugin_t* p, const clap_istream_t* s) {
	    return WrapperPlugin::from(p)->stateLoad(s);
    },
};

static const clap_plugin_latency_t kLatency = {
    [](const clap_plugin_t* p) { return WrapperPlugin::from(p)->latencyGet(); },
};

static const clap_plugin_tail_t kTail = {
    [](const clap_plugin_t* p) { return WrapperPlugin::from(p)->tailGet(); },
};

static const clap_plugin_render_t kRender = {
    [](const clap_plugin_t*) { return false; },
    [](const clap_plugin_t* p, clap_plugin_render_mode m) {
	    return WrapperPlugin::from(p)->renderSet(m);
    },
};

static const clap_plugin_gui_t kGui = {
    [](const clap_plugin_t* p, const char* api, bool f) {
	    return WrapperPlugin::from(p)->guiIsApiSupported(api, f);
    },
    [](const clap_plugin_t* p, const char** api, bool* f) {
	    return WrapperPlugin::from(p)->guiGetPreferredApi(api, f);
    },
    [](const clap_plugin_t* p, const char* api, bool f) {
	    return WrapperPlugin::from(p)->guiCreate(api, f);
    },
    [](const clap_plugin_t* p) { WrapperPlugin::from(p)->guiDestroy(); },
    [](const clap_plugin_t* p, double s) { return WrapperPlugin::from(p)->guiSetScale(s); },
    [](const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
	    return WrapperPlugin::from(p)->guiGetSize(w, h);
    },
    [](const clap_plugin_t* p) { return WrapperPlugin::from(p)->guiCanResize(); },
    [](const clap_plugin_t* p, clap_gui_resize_hints_t* h) {
	    return WrapperPlugin::from(p)->guiGetResizeHints(h);
    },
    [](const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
	    return WrapperPlugin::from(p)->guiAdjustSize(w, h);
    },
    [](const clap_plugin_t* p, uint32_t w, uint32_t h) {
	    return WrapperPlugin::from(p)->guiSetSize(w, h);
    },
    [](const clap_plugin_t* p, const clap_window_t* w) {
	    return WrapperPlugin::from(p)->guiSetParent(w);
    },
    [](const clap_plugin_t*, const clap_window_t*) { return false; }, // set_transient: floating only
    [](const clap_plugin_t*, const char*) {},                          // suggest_title: floating only
    [](const clap_plugin_t*) { return true; },                        // show
    [](const clap_plugin_t*) { return true; },                        // hide
};

static const clap_plugin_timer_support_t kTimerSupport = {
    [](const clap_plugin_t* p, clap_id id) { WrapperPlugin::from(p)->onTimer(id); },
};

static const clap_plugin_posix_fd_support_t kPosixFdSupport = {
    [](const clap_plugin_t* p, int fd, clap_posix_fd_flags_t f) {
	    WrapperPlugin::from(p)->onFd(fd, f);
    },
};

const void* WrapperPlugin::getExtension(const char* id)
{
	if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
		return &kAudioPorts;
	if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
		return &kNotePorts;
	if (!std::strcmp(id, CLAP_EXT_PARAMS))
		return &kParams;
	if (!std::strcmp(id, CLAP_EXT_STATE))
		return &kState;
	if (!std::strcmp(id, CLAP_EXT_LATENCY))
		return &kLatency;
	if (!std::strcmp(id, CLAP_EXT_TAIL))
		return &kTail;
	if (!std::strcmp(id, CLAP_EXT_RENDER))
		return &kRender;
	if (!std::strcmp(id, CLAP_EXT_GUI))
		return controller ? &kGui : nullptr;
#if SMTG_OS_LINUX
	if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
		return &kTimerSupport;
	if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
		return &kPosixFdSupport;
#else
	(void)kTimerSupport;
	(void)kPosixFdSupport;
#endif
	return nullptr;
}

} // namespace v3c
