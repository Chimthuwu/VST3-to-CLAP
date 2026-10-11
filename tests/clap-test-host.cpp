// Minimal CLAP host used to exercise the vst3-to-clap wrapper headlessly.
//
//   clap-test-host <plugin.clap> [plugin-id]
//
// For every plugin in the file (or just plugin-id): instantiate, list ports and
// parameters, run audio with notes and automation, round-trip the state and
// check that the output is finite. Exit code 0 on success.
#define _USE_MATH_DEFINES
#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int gFailures = 0;
#define CHECK(cond, ...)                                                                           \
	do                                                                                             \
	{                                                                                              \
		if (!(cond))                                                                               \
		{                                                                                          \
			std::printf("  FAIL: " __VA_ARGS__);                                                   \
			std::printf("\n");                                                                     \
			++gFailures;                                                                           \
		}                                                                                          \
	} while (0)

// ---------------------------------------------------------------------------
// host
// ---------------------------------------------------------------------------
struct HostState
{
	bool callbackRequested = false;
	bool restartRequested = false;
	int flushRequests = 0;
	int rescans = 0;
} gHost;

static const clap_host_log_t kLog = {[](const clap_host_t*, clap_log_severity s, const char* m) {
	std::printf("  [plugin log %d] %s\n", int(s), m);
}};
static const clap_host_thread_check_t kThreadCheck = {
    [](const clap_host_t*) { return true; }, [](const clap_host_t*) { return true; }};
static const clap_host_params_t kHostParams = {
    [](const clap_host_t*, clap_param_rescan_flags) { ++gHost.rescans; },
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t*) { ++gHost.flushRequests; }};
static const clap_host_state_t kHostState = {[](const clap_host_t*) {}};
static const clap_host_latency_t kHostLatency = {[](const clap_host_t*) {}};

static clap_host_t gClapHost = {
    CLAP_VERSION_INIT,
    nullptr,
    "clap-test-host",
    "vst3-to-clap",
    "",
    "1.0",
    [](const clap_host_t*, const char* id) -> const void* {
	    if (!std::strcmp(id, CLAP_EXT_LOG))
		    return &kLog;
	    if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK))
		    return &kThreadCheck;
	    if (!std::strcmp(id, CLAP_EXT_PARAMS))
		    return &kHostParams;
	    if (!std::strcmp(id, CLAP_EXT_STATE))
		    return &kHostState;
	    if (!std::strcmp(id, CLAP_EXT_LATENCY))
		    return &kHostLatency;
	    return nullptr;
    },
    [](const clap_host_t*) { gHost.restartRequested = true; },
    [](const clap_host_t*) {},
    [](const clap_host_t*) { gHost.callbackRequested = true; },
};

// ---------------------------------------------------------------------------
// event lists
// ---------------------------------------------------------------------------
struct EventList
{
	std::vector<std::vector<uint8_t>> events;
	clap_input_events_t in{this, [](const clap_input_events_t* l) {
		                       return uint32_t(static_cast<EventList*>(l->ctx)->events.size());
	                       },
	                       [](const clap_input_events_t* l, uint32_t i) {
		                       return reinterpret_cast<const clap_event_header_t*>(
		                           static_cast<EventList*>(l->ctx)->events[i].data());
	                       }};
	clap_output_events_t out{this,
	                         [](const clap_output_events_t* l, const clap_event_header_t* e) {
		                         auto* self = static_cast<EventList*>(l->ctx);
		                         auto* p = reinterpret_cast<const uint8_t*>(e);
		                         self->events.emplace_back(p, p + e->size);
		                         return true;
	                         }};
	template <typename T>
	void push(const T& ev)
	{
		auto* p = reinterpret_cast<const uint8_t*>(&ev);
		events.emplace_back(p, p + sizeof(T));
	}
	void clear() { events.clear(); }
};

static clap_event_note_t makeNote(uint16_t type, uint32_t time, int16_t key, double vel)
{
	clap_event_note_t n{};
	n.header = {sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
	n.note_id = -1;
	n.port_index = 0;
	n.channel = 0;
	n.key = key;
	n.velocity = vel;
	return n;
}

static clap_event_param_value_t makeParam(uint32_t time, clap_id id, double v)
{
	clap_event_param_value_t p{};
	p.header = {sizeof(p), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
	p.param_id = id;
	p.note_id = -1;
	p.port_index = -1;
	p.channel = -1;
	p.key = -1;
	p.value = v;
	return p;
}

// ---------------------------------------------------------------------------
// streams
// ---------------------------------------------------------------------------
struct Blob
{
	std::vector<uint8_t> data;
	size_t readPos = 0;
	clap_ostream_t os{this, [](const clap_ostream_t* s, const void* buf, uint64_t size) -> int64_t {
		                  auto* b = static_cast<Blob*>(s->ctx);
		                  auto* p = static_cast<const uint8_t*>(buf);
		                  b->data.insert(b->data.end(), p, p + size);
		                  return int64_t(size);
	                  }};
	clap_istream_t is{this, [](const clap_istream_t* s, void* buf, uint64_t size) -> int64_t {
		                  auto* b = static_cast<Blob*>(s->ctx);
		                  // deliberately small chunks to test partial reads
		                  size_t n = std::min<size_t>(std::min<size_t>(size, b->data.size() - b->readPos), 1000);
		                  std::memcpy(buf, b->data.data() + b->readPos, n);
		                  b->readPos += n;
		                  return int64_t(n);
	                  }};
};

// ---------------------------------------------------------------------------
static void pumpMainThread(const clap_plugin_t* p)
{
	if (gHost.callbackRequested)
	{
		gHost.callbackRequested = false;
		p->on_main_thread(p);
	}
}

static void testPlugin(const clap_plugin_factory_t* factory, const clap_plugin_descriptor_t* d)
{
	std::printf("\n=== %s  (%s)\n    vendor: %s, version: %s\n    features:", d->name, d->id,
	            d->vendor, d->version);
	for (auto f = d->features; f && *f; ++f)
		std::printf(" %s", *f);
	std::printf("\n");

	const clap_plugin_t* p = factory->create_plugin(factory, &gClapHost, d->id);
	CHECK(p, "create_plugin returned null");
	if (!p)
		return;
	CHECK(p->init(p), "init failed");

	auto audioPorts = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
	auto notePorts = static_cast<const clap_plugin_note_ports_t*>(p->get_extension(p, CLAP_EXT_NOTE_PORTS));
	auto params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
	auto state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
	auto latency = static_cast<const clap_plugin_latency_t*>(p->get_extension(p, CLAP_EXT_LATENCY));
	auto tail = static_cast<const clap_plugin_tail_t*>(p->get_extension(p, CLAP_EXT_TAIL));
	auto gui = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
	CHECK(audioPorts && params && state && notePorts, "missing core extension");

	// --- ports
	std::vector<uint32_t> inCh, outCh;
	for (int dir = 1; dir >= 0; --dir)
	{
		const uint32_t n = audioPorts->count(p, dir);
		for (uint32_t i = 0; i < n; ++i)
		{
			clap_audio_port_info_t info{};
			CHECK(audioPorts->get(p, i, dir, &info), "audio port get");
			std::printf("    audio %s %u: '%s' %u ch%s\n", dir ? "in " : "out", i, info.name,
			            info.channel_count, (info.flags & CLAP_AUDIO_PORT_IS_MAIN) ? " (main)" : "");
			(dir ? inCh : outCh).push_back(info.channel_count);
		}
	}
	const uint32_t noteIns = notePorts->count(p, true);
	for (uint32_t i = 0; i < noteIns; ++i)
	{
		clap_note_port_info_t info{};
		notePorts->get(p, i, true, &info);
		std::printf("    note in %u: '%s'\n", i, info.name);
	}
	for (uint32_t i = 0, n = notePorts->count(p, false); i < n; ++i)
	{
		clap_note_port_info_t info{};
		notePorts->get(p, i, false, &info);
		std::printf("    note out %u: '%s'\n", i, info.name);
	}

	// --- params
	const uint32_t paramCount = params->count(p);
	std::printf("    %u parameters\n", paramCount);
	clap_id testParam = CLAP_INVALID_ID;
	clap_param_info_t testInfo{};
	uint32_t hidden = 0;
	for (uint32_t i = 0; i < paramCount; ++i)
	{
		clap_param_info_t info{};
		CHECK(params->get_info(p, i, &info), "param get_info %u", i);
		double v = 0;
		CHECK(params->get_value(p, info.id, &v), "get_value %u", info.id);
		CHECK(v >= info.min_value - 1e-9 && v <= info.max_value + 1e-9, "value out of range");
		char text[256] = {};
		params->value_to_text(p, info.id, v, text, sizeof(text));
		if (info.flags & CLAP_PARAM_IS_HIDDEN)
		{
			++hidden;
			continue;
		}
		if (i < 12)
			std::printf("      #%u '%s'%s%s [%g..%g] = %g  \"%s\"\n", info.id, info.name,
			            info.module[0] ? " in " : "", info.module, info.min_value, info.max_value,
			            v, text);
		if (testParam == CLAP_INVALID_ID && (info.flags & CLAP_PARAM_IS_AUTOMATABLE) &&
		    !(info.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_STEPPED)))
		{
			testParam = info.id;
			testInfo = info;
		}
	}
	if (hidden)
		std::printf("      (+%u hidden, e.g. MIDI CC proxies)\n", hidden);
	if (const char* forced = std::getenv("TEST_PARAM_ID"))
	{
		for (uint32_t i = 0; i < paramCount; ++i)
		{
			clap_param_info_t info{};
			params->get_info(p, i, &info);
			if (info.id == clap_id(std::strtoul(forced, nullptr, 10)))
			{
				testParam = info.id;
				testInfo = info;
			}
		}
	}
	if (testParam == CLAP_INVALID_ID)
	{
		// fall back to a stepped parameter (values below are then rounded)
		for (uint32_t i = 0; i < paramCount && testParam == CLAP_INVALID_ID; ++i)
		{
			clap_param_info_t info{};
			params->get_info(p, i, &info);
			if ((info.flags & CLAP_PARAM_IS_AUTOMATABLE) && info.max_value >= 4 &&
			    !(info.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_BYPASS | CLAP_PARAM_IS_HIDDEN)))
			{
				testParam = info.id;
				testInfo = info;
			}
		}
	}
	auto testValue = [&](double frac) {
		double v = testInfo.min_value + frac * (testInfo.max_value - testInfo.min_value);
		return (testInfo.flags & CLAP_PARAM_IS_STEPPED) ? std::round(v) : v;
	};
	if (testParam != CLAP_INVALID_ID)
		std::printf("    test parameter: #%u '%s'\n", testParam, testInfo.name);

	// text round trip
	if (testParam != CLAP_INVALID_ID)
	{
		char text[256] = {};
		const double mid = (testInfo.min_value + testInfo.max_value) * 0.37;
		if (params->value_to_text(p, testParam, mid, text, sizeof(text)))
		{
			double back = -1;
			if (params->text_to_value(p, testParam, text, &back))
				std::printf("    text round trip: %g -> \"%s\" -> %g\n", mid, text, back);
		}
	}

	// --- flush while inactive
	if (testParam != CLAP_INVALID_ID)
	{
		EventList in, out;
		in.push(makeParam(0, testParam, testValue(0.25)));
		params->flush(p, &in.in, &out.out);
		pumpMainThread(p);
		double v = 0;
		params->get_value(p, testParam, &v);
		CHECK(std::fabs(v - testValue(0.25)) < 1e-4, "flush did not update value (%g)", v);
	}

	if (latency)
		std::printf("    latency: %u samples\n", latency->get(p));

	// --- process
	const double sr = 48000;
	const uint32_t block = 256;
	CHECK(p->activate(p, sr, 1, block), "activate failed");
	if (tail)
		std::printf("    tail: %u samples\n", tail->get(p));
	CHECK(p->start_processing(p), "start_processing failed");

	std::vector<std::vector<float>> inData, outData;
	std::vector<std::vector<float*>> inPtrs, outPtrs;
	std::vector<clap_audio_buffer_t> inBufs, outBufs;
	auto makeBufs = [&](const std::vector<uint32_t>& ch, std::vector<std::vector<float>>& data,
	                    std::vector<std::vector<float*>>& ptrs, std::vector<clap_audio_buffer_t>& bufs) {
		for (uint32_t c : ch)
		{
			ptrs.emplace_back();
			for (uint32_t k = 0; k < c; ++k)
			{
				data.emplace_back(block, 0.f);
				ptrs.back().push_back(data.back().data());
			}
		}
		for (size_t b = 0; b < ch.size(); ++b)
			bufs.push_back({ptrs[b].data(), nullptr, ch[b], 0, 0});
	};
	inData.reserve(64);
	outData.reserve(64);
	makeBufs(inCh, inData, inPtrs, inBufs);
	makeBufs(outCh, outData, outPtrs, outBufs);

	EventList in, out;
	double energy = 0;
	bool finite = true;
	uint32_t outEvents = 0;
	uint32_t midiOut = 0;
	int64_t steady = 0;
	double phase = 0;
	double peak = 0;
	int firstHotBlock = -1;
	for (int blk = 0; blk < 400; ++blk)
	{
		for (auto& ch : inData)
			for (uint32_t s = 0; s < block; ++s)
				ch[s] = float(0.3 * std::sin(phase + s * 2 * M_PI * 440 / sr));
		phase += block * 2 * M_PI * 440 / sr;

		in.clear();
		out.clear();
		if (blk == 1)
		{
			in.push(makeNote(CLAP_EVENT_NOTE_ON, 10, 60, 0.9));
			in.push(makeNote(CLAP_EVENT_NOTE_ON, 20, 64, 0.8));
		}
		if (blk == 50)
		{
			// CLAP note expressions on the held note (addressed by key, no note id)
			for (auto [id, val] : {std::pair<int, double>{CLAP_NOTE_EXPRESSION_TUNING, 2.0},
			                       {CLAP_NOTE_EXPRESSION_VOLUME, 0.5},
			                       {CLAP_NOTE_EXPRESSION_PAN, 0.2},
			                       {CLAP_NOTE_EXPRESSION_PRESSURE, 0.7}})
			{
				clap_event_note_expression_t ne{};
				ne.header = {sizeof(ne), 7, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
				ne.expression_id = id;
				ne.note_id = -1;
				ne.port_index = 0;
				ne.channel = 0;
				ne.key = 60;
				ne.value = val;
				in.push(ne);
			}
		}
		if (blk == 100 && testParam != CLAP_INVALID_ID)
			in.push(makeParam(5, testParam, testValue(0.75)));
		if (blk == 150)
		{
			// MIDI dialect: note on 67 and a pitch bend
			clap_event_midi_t m{};
			m.header = {sizeof(m), 3, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
			m.port_index = 0;
			m.data[0] = 0x90;
			m.data[1] = 67;
			m.data[2] = 100;
			in.push(m);
			m.header.time = 50;
			m.data[0] = 0xE0;
			m.data[1] = 0;
			m.data[2] = 80;
			in.push(m);
		}
		if (blk == 300)
		{
			in.push(makeNote(CLAP_EVENT_NOTE_OFF, 0, 60, 0.0));
			in.push(makeNote(CLAP_EVENT_NOTE_OFF, 0, 64, 0.0));
			in.push(makeNote(CLAP_EVENT_NOTE_OFF, 0, 67, 0.0));
		}

		clap_event_transport_t tr{};
		tr.header = {sizeof(tr), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
		tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
		           CLAP_TRANSPORT_HAS_TIME_SIGNATURE | CLAP_TRANSPORT_IS_PLAYING;
		tr.tempo = 120;
		tr.song_pos_beats = clap_beattime(double(steady) / sr * 2.0 * CLAP_BEATTIME_FACTOR);
		tr.tsig_num = 4;
		tr.tsig_denom = 4;

		clap_process_t proc{};
		proc.steady_time = steady;
		proc.frames_count = block;
		proc.transport = &tr;
		proc.audio_inputs = inBufs.data();
		proc.audio_inputs_count = uint32_t(inBufs.size());
		proc.audio_outputs = outBufs.data();
		proc.audio_outputs_count = uint32_t(outBufs.size());
		proc.in_events = &in.in;
		proc.out_events = &out.out;
		const auto status = p->process(p, &proc);
		CHECK(status != CLAP_PROCESS_ERROR, "process returned error");
		steady += block;
		outEvents += uint32_t(out.events.size());
		for (auto& raw : out.events)
		{
			auto* h = reinterpret_cast<const clap_event_header_t*>(raw.data());
			if (h->type == CLAP_EVENT_MIDI && midiOut++ < 3)
			{
				auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
				std::printf("    midi out @%u: %02X %02X %02X\n", h->time, m->data[0], m->data[1], m->data[2]);
			}
		}

		for (auto& ch : outData)
			for (float s : ch)
			{
				if (!std::isfinite(s))
					finite = false;
				energy += double(s) * s;
				peak = std::max(peak, double(std::fabs(s)));
				if (std::fabs(s) > 4.0 && firstHotBlock < 0)
					firstHotBlock = blk;
			}
		pumpMainThread(p);
	}
	CHECK(finite, "non-finite output");
	if (firstHotBlock >= 0)
		std::printf("    WARNING: output exceeds +12 dBFS from block %d (peak %g)\n", firstHotBlock, peak);
	std::printf("    processed %d blocks, output RMS %.4f, %u output events\n", 400,
	            std::sqrt(energy / std::max<size_t>(1, outData.size() * 400 * block)), outEvents);
	if (!outData.empty() && energy == 0)
		std::printf("    note: output is silent (may be expected, e.g. a generator in its off state)\n");

	if (testParam != CLAP_INVALID_ID)
	{
		double v = 0;
		params->get_value(p, testParam, &v);
		CHECK(std::fabs(v - testValue(0.75)) < 1e-4,
		      "automation did not reach the controller (%g)", v);
	}

	// flush while active (but between process calls)
	if (testParam != CLAP_INVALID_ID)
	{
		EventList fi, fo;
		fi.push(makeParam(0, testParam, testValue(0.6)));
		params->flush(p, &fi.in, &fo.out);
		pumpMainThread(p);
	}

	p->stop_processing(p);
	p->deactivate(p);

	// --- state round trip
	Blob saved;
	CHECK(state->save(p, &saved.os), "state save failed");
	std::printf("    state: %zu bytes\n", saved.data.size());
	if (testParam != CLAP_INVALID_ID)
	{
		double before = 0, changed = 0, after = 0;
		params->get_value(p, testParam, &before);
		EventList fi, fo;
		fi.push(makeParam(0, testParam, testInfo.min_value));
		params->flush(p, &fi.in, &fo.out);
		pumpMainThread(p);
		params->get_value(p, testParam, &changed);
		CHECK(state->load(p, &saved.is), "state load failed");
		params->get_value(p, testParam, &after);
		std::printf("    state round trip: %g -> %g -> %g\n", before, changed, after);
		CHECK(std::fabs(before - after) < 1e-4, "state did not restore the parameter");
	}

	// re-activate to make sure the instance survives a second cycle
	CHECK(p->activate(p, 44100, 1, 1024), "re-activate failed");
	p->deactivate(p);

	if (gui)
	{
		const char* api = nullptr;
		bool floating = false;
		if (gui->get_preferred_api(p, &api, &floating))
			std::printf("    gui: preferred api %s\n", api);
	}

	p->destroy(p);
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: clap-test-host <plugin.clap> [plugin-id]\n");
		return 2;
	}
	std::string path = argv[1];
	std::string binary = path;
#if defined(__APPLE__)
	{
		std::string name = path.substr(path.find_last_of('/') + 1);
		name = name.substr(0, name.rfind('.'));
		binary = path + "/Contents/MacOS/" + name;
	}
#endif
#if defined(_WIN32)
	HMODULE lib = LoadLibraryA(binary.c_str());
	auto entry = lib ? reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry")) : nullptr;
#else
	void* lib = dlopen(binary.c_str(), RTLD_LOCAL | RTLD_NOW);
	if (!lib)
		std::fprintf(stderr, "%s\n", dlerror());
	auto entry = lib ? static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry")) : nullptr;
#endif
	if (!entry)
	{
		std::fprintf(stderr, "cannot load clap_entry from %s\n", path.c_str());
		return 1;
	}
	if (!entry->init(path.c_str()))
	{
		std::fprintf(stderr, "entry init failed\n");
		return 1;
	}
	auto factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	const uint32_t n = factory ? factory->get_plugin_count(factory) : 0;
	std::printf("%s: %u plugin(s)\n", path.c_str(), n);
	int tested = 0;
	for (uint32_t i = 0; i < n; ++i)
	{
		auto d = factory->get_plugin_descriptor(factory, i);
		if (argc > 2 && std::strcmp(d->id, argv[2]) != 0)
			continue;
		testPlugin(factory, d);
		++tested;
	}
	entry->deinit();
	std::printf("\n%d plugin(s) tested, %d failure(s)\n", tested, gFailures);
	return (gFailures == 0 && tested > 0) ? 0 : 1;
}
