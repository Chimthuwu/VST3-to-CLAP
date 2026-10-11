// vst3-to-clap - one CLAP plugin instance hosting one VST3 component/controller pair
#pragma once

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"

#include "registry.h"
#include "util.h"

namespace v3c {

class WrapperPlugin;

//------------------------------------------------------------------------
// Host-side objects handed to the VST3 plug-in. They hold a back pointer to the
// wrapper which is cleared before the wrapper is destroyed.
//------------------------------------------------------------------------
class ComponentHandler
: public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::Vst::IComponentHandler,
                                                         Steinberg::Vst::IComponentHandler2>>
{
public:
	explicit ComponentHandler(WrapperPlugin* o) : owner(o) {}
	void detach() { owner = nullptr; }

	Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID id) override;
	Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID id,
	                                          Steinberg::Vst::ParamValue valueNormalized) override;
	Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID id) override;
	Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override;

	Steinberg::tresult PLUGIN_API setDirty(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString) override;
	Steinberg::tresult PLUGIN_API startGroupEdit() override { return Steinberg::kResultOk; }
	Steinberg::tresult PLUGIN_API finishGroupEdit() override { return Steinberg::kResultOk; }

private:
	WrapperPlugin* owner;
};


class PlugFrame : public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::IPlugFrame>>
{
public:
	using Base = Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::IPlugFrame>>;
	explicit PlugFrame(WrapperPlugin* o) : owner(o) {}
	void detach() { owner = nullptr; }

	Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view,
	                                         Steinberg::ViewRect* newSize) override;
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;

private:
	WrapperPlugin* owner;
};

// Sits between component and controller, like a real host's connection proxy.
// (The SDK's ConnectionProxy refuses messages from non-UI threads; this one doesn't.)
class ConnectionProxy
: public Steinberg::U::Implements<Steinberg::U::Directly<Steinberg::Vst::IConnectionPoint>>
{
public:
	explicit ConnectionProxy(Steinberg::Vst::IConnectionPoint* src) : source(src) {}
	Steinberg::tresult PLUGIN_API connect(Steinberg::Vst::IConnectionPoint* other) override
	{
		if (!other || destination)
			return Steinberg::kResultFalse;
		destination = other;
		const auto r = source->connect(this);
		if (r != Steinberg::kResultTrue)
			destination = nullptr;
		return r;
	}
	Steinberg::tresult PLUGIN_API disconnect(Steinberg::Vst::IConnectionPoint* other) override
	{
		if (!other || other != destination.get())
			return Steinberg::kResultFalse;
		source->disconnect(this);
		destination = nullptr;
		return Steinberg::kResultTrue;
	}
	Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override
	{
		return destination ? destination->notify(message) : Steinberg::kResultFalse;
	}
	bool disconnectAll()
	{
		if (!destination)
			return false;
		source->disconnect(this);
		destination = nullptr;
		return true;
	}

private:
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> source;
	Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> destination;
};

// Per-instance IHostApplication. On Linux it also exposes the run loop.
class HostContext : public Steinberg::Vst::HostApplication
{
public:
	explicit HostContext(WrapperPlugin* o) : owner(o) {}
	void detach() { owner = nullptr; }
	Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override;
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;

private:
	WrapperPlugin* owner;
};

//------------------------------------------------------------------------
struct ParamDesc
{
	Steinberg::Vst::ParameterInfo info{};
	std::string name, units, module;
	uint32_t flags = 0;    // clap_param_info_flags
	bool midiMapped = false; // a VST3 "MIDI CC" proxy parameter

	Steinberg::int32 steps = 0; // 0 = continuous [0, 1]; otherwise integer [0, steps]

	double clapMin() const { return 0.0; }
	double clapMax() const { return steps > 0 ? double(steps) : 1.0; }
	double toNormalized(double clapValue) const;
	double fromNormalized(double normalized) const;
};

//------------------------------------------------------------------------
class WrapperPlugin
{
public:
	WrapperPlugin(const clap_host_t* host, const PluginEntry* entry);
	~WrapperPlugin();

	const clap_plugin_t* clapPlugin() const { return &plugin; }
	static WrapperPlugin* from(const clap_plugin_t* p)
	{
		return static_cast<WrapperPlugin*>(p->plugin_data);
	}

	// --- clap_plugin
	bool init();
	void destroy();
	bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
	void deactivate();
	bool startProcessing();
	void stopProcessing();
	void reset();
	clap_process_status process(const clap_process_t* process);
	const void* getExtension(const char* id);
	void onMainThread();
	void handleRestartFlags(Steinberg::int32 flags);

	// --- clap.audio-ports
	uint32_t audioPortsCount(bool isInput);
	bool audioPortsGet(uint32_t index, bool isInput, clap_audio_port_info_t* info);
	// --- clap.note-ports
	uint32_t notePortsCount(bool isInput);
	bool notePortsGet(uint32_t index, bool isInput, clap_note_port_info_t* info);
	// --- clap.params
	uint32_t paramsCount() { return uint32_t(params.size()); }
	bool paramsGetInfo(uint32_t index, clap_param_info_t* info);
	bool paramsGetValue(clap_id id, double* value);
	bool paramsValueToText(clap_id id, double value, char* display, uint32_t size);
	bool paramsTextToValue(clap_id id, const char* display, double* value);
	void paramsFlush(const clap_input_events_t* in, const clap_output_events_t* out);
	// --- clap.state
	bool stateSave(const clap_ostream_t* stream);
	bool stateLoad(const clap_istream_t* stream);
	// --- clap.latency / clap.tail
	uint32_t latencyGet();
	uint32_t tailGet();
	// --- clap.render
	bool renderSet(clap_plugin_render_mode mode);
	// --- clap.gui
	bool guiIsApiSupported(const char* api, bool isFloating);
	bool guiGetPreferredApi(const char** api, bool* isFloating);
	bool guiCreate(const char* api, bool isFloating);
	void guiDestroy();
	bool guiSetScale(double scale);
	bool guiGetSize(uint32_t* width, uint32_t* height);
	bool guiCanResize();
	bool guiGetResizeHints(clap_gui_resize_hints_t* hints);
	bool guiAdjustSize(uint32_t* width, uint32_t* height);
	bool guiSetSize(uint32_t width, uint32_t height);
	bool guiSetParent(const clap_window_t* window);
	// --- clap.timer-support / clap.posix-fd-support
	void onTimer(clap_id id);
	void onFd(int fd, clap_posix_fd_flags_t flags);

	// --- callbacks from the VST3 side
	void vstBeginEdit(Steinberg::Vst::ParamID id);
	void vstPerformEdit(Steinberg::Vst::ParamID id, double normalized);
	void vstEndEdit(Steinberg::Vst::ParamID id);
	void vstRestartComponent(Steinberg::int32 flags);
	void vstSetDirty();
	bool vstResizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize);

	const clap_host_t* getHost() const { return host; }
	const clap_host_timer_support_t* hostTimerExt() const { return hostTimer; }
	const clap_host_posix_fd_support_t* hostFdExt() const { return hostFd; }

	void log(clap_log_severity severity, const std::string& msg) const;

private:
	enum class GuiEventKind : uint8_t { Begin, Value, End };
	struct GuiEvent
	{
		GuiEventKind kind = GuiEventKind::Value;
		Steinberg::Vst::ParamID id = 0;
		double normalized = 0.0;
	};
	struct ParamUpdate // processor/host -> controller
	{
		Steinberg::Vst::ParamID id = 0;
		double normalized = 0.0;
	};
	union OutEvent
	{
		clap_event_header_t header;
		clap_event_param_value_t paramValue;
		clap_event_param_gesture_t paramGesture;
		clap_event_note_t note;
		clap_event_note_expression_t noteExpression;
		clap_event_midi_t midi;
	};

	bool createVst3();
	void releaseVst3();
	void buildParams();
	bool refreshParamInfos();
	void describeParam(ParamDesc& d, const std::unordered_set<Steinberg::Vst::ParamID>& midiParams,
	                   const std::unordered_map<Steinberg::Vst::UnitID, std::string>& unitPaths);
	std::unordered_set<Steinberg::Vst::ParamID> midiMappedParams() const;
	void rebuildMidiMap();
	void rescanPorts();
	static constexpr int kMidiMapBuses = 4;
	static constexpr size_t midiMapTableSize()
	{
		return size_t(kMidiMapBuses) * 16 * Steinberg::Vst::kCountCtrlNumber;
	}
	void buildUnitPaths(std::unordered_map<Steinberg::Vst::UnitID, std::string>& out);
	bool isMainThread() const;
	void cacheValue(Steinberg::Vst::ParamID id, double normalized);
	void refreshValueCache();

	// processing helpers
	void addParamChange(Steinberg::Vst::ParamID id, double normalized, int32_t offset);
	void handleInputEvent(const clap_event_header_t* hdr);
	void handleMidi(uint16_t port, const uint8_t data[3], int32_t offset);
	void addNoteOn(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity,
	               int32_t offset);
	void addNoteOff(int16_t port, int16_t channel, int16_t key, int32_t noteId, float velocity,
	                int32_t offset);
	void addNoteExpression(const clap_event_note_expression_t* ev);
	int32_t lookupNoteId(int16_t channel, int16_t key) const;
	void drainGuiEvents(bool toProcessor);
	void drainPendingToProcessor();
	void emitOutputs(const clap_output_events_t* out);
	void flushOutQueue(const clap_output_events_t* out);
	void pushOut(const OutEvent& e);
	void fillProcessContext(const clap_event_transport_t* transport, uint32_t frames);
	bool runSilentProcess(uint32_t frames);
	void applyPendingWhileInactive();

	clap_plugin_t plugin{};
	const clap_host_t* host;
	const PluginEntry* entry;

	const clap_host_params_t* hostParams = nullptr;
	const clap_host_latency_t* hostLatency = nullptr;
	const clap_host_tail_t* hostTail = nullptr;
	const clap_host_gui_t* hostGui = nullptr;
	const clap_host_state_t* hostState = nullptr;
	const clap_host_log_t* hostLog = nullptr;
	const clap_host_audio_ports_t* hostAudioPorts = nullptr;
	const clap_host_note_ports_t* hostNotePorts = nullptr;
	const clap_host_thread_check_t* hostThreadCheck = nullptr;
	const clap_host_timer_support_t* hostTimer = nullptr;
	const clap_host_posix_fd_support_t* hostFd = nullptr;

	// VST3 objects
	VST3::Hosting::Module::Ptr module;
	std::unique_ptr<HostContext> hostContext;
	Steinberg::IPtr<Steinberg::Vst::IComponent> component;
	Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor;
	Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
	Steinberg::IPtr<Steinberg::Vst::IMidiMapping> midiMapping;
	Steinberg::IPtr<ConnectionProxy> componentProxy, controllerProxy;
	Steinberg::IPtr<ComponentHandler> componentHandler;
	Steinberg::IPtr<PlugFrame> plugFrame;
	bool singleComponent = false;
	bool componentInitialized = false;
	bool controllerInitialized = false;

	// GUI
	Steinberg::IPtr<Steinberg::IPlugView> view;
	bool viewAttached = false;
	double guiScale = 1.0;

	// parameters
	std::vector<ParamDesc> params;
	std::unordered_map<Steinberg::Vst::ParamID, uint32_t> paramIndex;
	Steinberg::Vst::ParamID programChangeParam = Steinberg::Vst::kNoParamId;
	// IMidiMapping snapshot [bus][channel][controller] -> ParamID, readable from the audio thread
	std::unique_ptr<std::atomic<Steinberg::Vst::ParamID>[]> midiMap;
	std::unique_ptr<std::atomic<double>[]> valueCache; // latest normalized value per param
	std::vector<double> pendingValue; // host -> processor while not processing
	std::vector<uint8_t> pendingDirty;
	bool anyPending = false;
	SpscQueue<GuiEvent> guiToAudio{4096};
	SpscQueue<ParamUpdate> audioToMain{8192};
	std::atomic<bool> mainCallbackRequested{false};
	bool inMainThreadCallback = false;
	std::thread::id mainThreadId;

	// state of the processor
	std::atomic<bool> active{false};
	std::atomic<bool> processing{false};
	std::atomic<Steinberg::int32> pendingRestartFlags{0};
	bool needsAudioPortsRescan = false;
	bool needsParamsRescanAll = false;
	bool latencyChanged = false;
	uint32_t latencySamples = 0;
	bool silentActivation = false;
	bool processorNeedsSync = false;
	Steinberg::int32 processMode = Steinberg::Vst::kRealtime;
	double sampleRate = 44100.0;
	uint32_t maxFrames = 0;

	// processing buffers (allocated in activate)
	Steinberg::Vst::ProcessData processData{};
	Steinberg::Vst::ProcessContext processContext{};
	std::vector<Steinberg::Vst::AudioBusBuffers> inBuses, outBuses;
	std::vector<std::vector<float*>> inChannelPtrs, outChannelPtrs;
	std::vector<float> zeroBuffer;
	std::vector<std::vector<float>> scratchOut;
	Steinberg::IPtr<Steinberg::Vst::ParameterChanges> inParams, outParams;
	Steinberg::IPtr<Steinberg::Vst::EventList> inEvents, outEvents;
	std::vector<OutEvent> outQueue;
	uint32_t currentFrames = 0;
	// VST3 plug-ins sometimes report events at offset == numSamples; CLAP needs time < frames
	uint32_t clampTime(int32_t offset) const
	{
		if (offset < 0 || currentFrames == 0)
			return 0;
		return std::min<uint32_t>(uint32_t(offset), currentFrames - 1);
	}
	int64_t continuousSamples = 0;
	int64_t projectSamplesFallback = 0;
	std::array<std::array<int32_t, 128>, 16> activeNoteIds{};
	int32_t nextNoteId = 1 << 24;
};

} // namespace v3c
