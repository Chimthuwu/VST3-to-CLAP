// X11 GUI smoke test for the vst3-to-clap wrapper (Linux only).
//
//   clap-gui-test <plugin.clap> <plugin-id> [seconds] [screenshot-delay-cmd]
//
// Creates an X11 window, embeds the plugin editor through clap.gui, and runs a host
// event loop that serves clap.timer-support and clap.posix-fd-support for N seconds.
#include <clap/clap.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <dlfcn.h>
#include <poll.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

struct Timer
{
	uint32_t period;
	Clock::time_point next;
};
static std::map<clap_id, Timer> gTimers;
static std::map<int, clap_posix_fd_flags_t> gFds;
static clap_id gNextTimer = 1;
static int gResizeRequests = 0;
static uint32_t gReqW = 0, gReqH = 0;
static bool gCallback = false;
static bool gFlushRequested = false;

static const clap_host_params_t kParams = {
    [](const clap_host_t*, clap_param_rescan_flags) {},
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t*) { gFlushRequested = true; }};

struct OutEvents
{
	int begins = 0, values = 0, ends = 0;
	double lastValue = -1;
	clap_id lastId = CLAP_INVALID_ID;
	clap_output_events_t out{this, [](const clap_output_events_t* o, const clap_event_header_t* e) {
		auto* self = static_cast<OutEvents*>(o->ctx);
		if (e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN)
			++self->begins;
		else if (e->type == CLAP_EVENT_PARAM_GESTURE_END)
			++self->ends;
		else if (e->type == CLAP_EVENT_PARAM_VALUE)
		{
			++self->values;
			auto* v = reinterpret_cast<const clap_event_param_value_t*>(e);
			self->lastValue = v->value;
			self->lastId = v->param_id;
		}
		return true;
	}};
} gOut;

static const clap_host_timer_support_t kTimer = {
    [](const clap_host_t*, uint32_t period, clap_id* id) {
	    *id = gNextTimer++;
	    gTimers[*id] = {period, Clock::now() + std::chrono::milliseconds(period)};
	    return true;
    },
    [](const clap_host_t*, clap_id id) { return gTimers.erase(id) > 0; }};
static const clap_host_posix_fd_support_t kFd = {
    [](const clap_host_t*, int fd, clap_posix_fd_flags_t f) {
	    gFds[fd] = f;
	    return true;
    },
    [](const clap_host_t*, int fd, clap_posix_fd_flags_t f) {
	    gFds[fd] = f;
	    return true;
    },
    [](const clap_host_t*, int fd) { return gFds.erase(fd) > 0; }};
static const clap_host_gui_t kGui = {
    [](const clap_host_t*) {},
    [](const clap_host_t*, uint32_t w, uint32_t h) {
	    ++gResizeRequests;
	    gReqW = w;
	    gReqH = h;
	    return true;
    },
    [](const clap_host_t*) { return true; },
    [](const clap_host_t*) { return true; },
    [](const clap_host_t*, bool) {}};
static const clap_host_log_t kLog = {[](const clap_host_t*, clap_log_severity, const char* m) {
	std::printf("[plugin] %s\n", m);
}};

static clap_host_t gHost = {
    CLAP_VERSION_INIT, nullptr, "clap-gui-test", "vst3-to-clap", "", "1.0",
    [](const clap_host_t*, const char* id) -> const void* {
	    if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT))
		    return &kTimer;
	    if (!strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT))
		    return &kFd;
	    if (!strcmp(id, CLAP_EXT_GUI))
		    return &kGui;
	    if (!strcmp(id, CLAP_EXT_LOG))
		    return &kLog;
	    if (!strcmp(id, CLAP_EXT_PARAMS))
		    return &kParams;
	    return nullptr;
    },
    [](const clap_host_t*) {}, [](const clap_host_t*) {},
    [](const clap_host_t*) { gCallback = true; }};

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		std::fprintf(stderr, "usage: clap-gui-test <plugin.clap> <plugin-id> [seconds] [cmd]\n");
		return 2;
	}
	const double seconds = argc > 3 ? std::atof(argv[3]) : 2.0;
	const char* cmd = argc > 4 ? argv[4] : nullptr;

	Display* dpy = XOpenDisplay(nullptr);
	if (!dpy)
	{
		std::fprintf(stderr, "no X display\n");
		return 1;
	}

	void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	auto entry = lib ? static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry")) : nullptr;
	if (!entry || !entry->init(argv[1]))
	{
		std::fprintf(stderr, "cannot load %s\n", argv[1]);
		return 1;
	}
	auto factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
	const clap_plugin_t* p = factory->create_plugin(factory, &gHost, argv[2]);
	if (!p || !p->init(p))
	{
		std::fprintf(stderr, "cannot create %s\n", argv[2]);
		return 1;
	}
	auto gui = static_cast<const clap_plugin_gui_t*>(p->get_extension(p, CLAP_EXT_GUI));
	auto timerExt = static_cast<const clap_plugin_timer_support_t*>(p->get_extension(p, CLAP_EXT_TIMER_SUPPORT));
	auto fdExt = static_cast<const clap_plugin_posix_fd_support_t*>(p->get_extension(p, CLAP_EXT_POSIX_FD_SUPPORT));
	if (!gui || !gui->is_api_supported(p, CLAP_WINDOW_API_X11, false))
	{
		std::fprintf(stderr, "no x11 gui\n");
		return 1;
	}
	if (!gui->create(p, CLAP_WINDOW_API_X11, false))
	{
		std::printf("gui create: plugin has no editor\n");
		p->destroy(p);
		entry->deinit();
		return 3;
	}
	uint32_t w = 0, h = 0;
	gui->get_size(p, &w, &h);
	std::printf("editor size %ux%u, resizable %d\n", w, h, int(gui->can_resize(p)));

	Window win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, w ? w : 400, h ? h : 300, 0,
	                                 0, 0x202020);
	XStoreName(dpy, win, "clap-gui-test");
	XMapWindow(dpy, win);
	XSync(dpy, False);

	clap_window_t cw{CLAP_WINDOW_API_X11, {}};
	cw.x11 = win;
	if (!gui->set_parent(p, &cw))
	{
		std::fprintf(stderr, "set_parent failed\n");
		return 1;
	}
	gui->show(p);
	XSync(dpy, False);

	// Minimal XEmbed embedder side (CLAP requires XEmbed on X11): tell every child
	// window it has been embedded, which is when e.g. VSTGUI maps itself.
	{
		Atom xembed = XInternAtom(dpy, "_XEMBED", False);
		Window root, parent, *children = nullptr;
		unsigned int n = 0;
		if (XQueryTree(dpy, win, &root, &parent, &children, &n))
		{
			for (unsigned int i = 0; i < n; ++i)
			{
				XEvent ev{};
				ev.xclient.type = ClientMessage;
				ev.xclient.window = children[i];
				ev.xclient.message_type = xembed;
				ev.xclient.format = 32;
				ev.xclient.data.l[0] = CurrentTime;
				ev.xclient.data.l[1] = 0; // XEMBED_EMBEDDED_NOTIFY
				ev.xclient.data.l[3] = long(win);
				ev.xclient.data.l[4] = 0; // protocol version
				XSendEvent(dpy, children[i], False, NoEventMask, &ev);
			}
			std::printf("xembed: notified %u child window(s)\n", n);
			if (children)
				XFree(children);
		}
		XSync(dpy, False);
	}
	std::printf("embedded; timers=%zu fds=%zu\n", gTimers.size(), gFds.size());

	auto params = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
	// optional mouse drag inside the editor: CLAP_GUI_TEST_DRAG="x1,y1,x2,y2"
	int drag[4] = {-1, -1, -1, -1};
	if (const char* d = std::getenv("CLAP_GUI_TEST_DRAG"))
		std::sscanf(d, "%d,%d,%d,%d", &drag[0], &drag[1], &drag[2], &drag[3]);
	int dragStep = 0;

	// optional audio processing while the editor is open: CLAP_GUI_TEST_PROCESS=1
	const bool doProcess = std::getenv("CLAP_GUI_TEST_PROCESS") != nullptr;
	auto audioPorts = static_cast<const clap_plugin_audio_ports_t*>(p->get_extension(p, CLAP_EXT_AUDIO_PORTS));
	const uint32_t block = 256;
	std::vector<std::vector<float>> chans;
	std::vector<std::vector<float*>> ptrs;
	std::vector<clap_audio_buffer_t> ins, outs;
	int64_t steady = 0;
	if (doProcess && audioPorts)
	{
		chans.reserve(64);
		for (int dir = 1; dir >= 0; --dir)
			for (uint32_t i = 0, n = audioPorts->count(p, dir); i < n; ++i)
			{
				clap_audio_port_info_t info{};
				audioPorts->get(p, i, dir, &info);
				ptrs.emplace_back();
				for (uint32_t c = 0; c < info.channel_count; ++c)
				{
					chans.emplace_back(block, 0.f);
					ptrs.back().push_back(chans.back().data());
				}
				(dir ? ins : outs).push_back({nullptr, nullptr, info.channel_count, 0, 0});
			}
		size_t k = 0;
		for (auto& b : ins)
			b.data32 = ptrs[k++].data();
		for (auto& b : outs)
			b.data32 = ptrs[k++].data();
		if (!p->activate(p, 48000, 1, block) || !p->start_processing(p))
			std::fprintf(stderr, "activate failed\n");
	}
	auto dragAt = Clock::now() + std::chrono::milliseconds(500);

	const auto end = Clock::now() + std::chrono::milliseconds(int(seconds * 1000));
	bool ranCmd = false;
	uint64_t timerFires = 0, fdFires = 0;
	while (Clock::now() < end)
	{
		std::vector<pollfd> pfds;
		for (auto& kv : gFds)
			pfds.push_back({kv.first, short(POLLIN), 0});
		pfds.push_back({ConnectionNumber(dpy), short(POLLIN), 0});
		poll(pfds.data(), nfds_t(pfds.size()), 5);
		for (size_t i = 0; i + 1 < pfds.size(); ++i)
			if (pfds[i].revents && fdExt)
			{
				fdExt->on_fd(p, pfds[i].fd, CLAP_POSIX_FD_READ);
				++fdFires;
			}
		while (XPending(dpy))
		{
			XEvent ev;
			XNextEvent(dpy, &ev);
		}
		const auto now = Clock::now();
		std::vector<clap_id> due;
		for (auto& kv : gTimers)
			if (now >= kv.second.next)
			{
				due.push_back(kv.first);
				kv.second.next = now + std::chrono::milliseconds(kv.second.period);
			}
		for (auto id : due)
			if (timerExt)
			{
				timerExt->on_timer(p, id);
				++timerFires;
			}
		if (gCallback)
		{
			gCallback = false;
			p->on_main_thread(p);
		}
		if (drag[0] >= 0 && dragStep < 12 && Clock::now() >= dragAt)
		{
			// press, move in 10 steps, release
			if (dragStep == 0)
			{
				XTestFakeMotionEvent(dpy, -1, drag[0], drag[1], 0);
				XTestFakeButtonEvent(dpy, 1, True, 0);
			}
			else if (dragStep <= 10)
			{
				int x = drag[0] + (drag[2] - drag[0]) * dragStep / 10;
				int y = drag[1] + (drag[3] - drag[1]) * dragStep / 10;
				XTestFakeMotionEvent(dpy, -1, x, y, 0);
			}
			else
				XTestFakeButtonEvent(dpy, 1, False, 0);
			XFlush(dpy);
			++dragStep;
			dragAt = Clock::now() + std::chrono::milliseconds(30);
		}
		if (doProcess)
		{
			struct List
			{
				std::vector<clap_event_note_t> notes;
			} list;
			if ((steady / block) % 200 == 10)
			{
				clap_event_note_t n{{sizeof(clap_event_note_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, -1, 0, 0, 60, 0.8};
				list.notes.push_back(n);
			}
			if ((steady / block) % 200 == 150)
			{
				clap_event_note_t n{{sizeof(clap_event_note_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0}, -1, 0, 0, 60, 0.0};
				list.notes.push_back(n);
			}
			clap_input_events_t in{&list, [](const clap_input_events_t* l) {
				                       return uint32_t(static_cast<const List*>(l->ctx)->notes.size());
			                       },
			                       [](const clap_input_events_t* l, uint32_t i) {
				                       return &static_cast<const List*>(l->ctx)->notes[i].header;
			                       }};
			clap_event_transport_t tr{};
			tr.header = {sizeof(tr), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
			tr.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE |
			           CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE |
			           CLAP_TRANSPORT_IS_PLAYING;
			tr.tempo = 120;
			tr.song_pos_beats = clap_beattime(double(steady) / 48000.0 * 2.0 * CLAP_BEATTIME_FACTOR);
			tr.song_pos_seconds = clap_sectime(double(steady) / 48000.0 * CLAP_SECTIME_FACTOR);
			tr.bar_start = clap_beattime((int64_t(tr.song_pos_beats / CLAP_BEATTIME_FACTOR) / 4) * 4 * CLAP_BEATTIME_FACTOR);
			tr.tsig_num = 4;
			tr.tsig_denom = 4;
			clap_process_t proc{};
			proc.steady_time = steady;
			proc.frames_count = block;
			proc.transport = &tr;
			proc.audio_inputs = ins.data();
			proc.audio_inputs_count = uint32_t(ins.size());
			proc.audio_outputs = outs.data();
			proc.audio_outputs_count = uint32_t(outs.size());
			proc.in_events = &in;
			proc.out_events = &gOut.out;
			p->process(p, &proc);
			steady += block;
		}
		if (gFlushRequested && params && !doProcess)
		{
			gFlushRequested = false;
			params->flush(p, nullptr, &gOut.out);
		}
		if (gResizeRequests && gReqW)
		{
			XResizeWindow(dpy, win, gReqW, gReqH);
			gReqW = 0;
		}
		if (cmd && !ranCmd && Clock::now() > end - std::chrono::milliseconds(int(seconds * 300)))
		{
			XSync(dpy, False);
			ranCmd = true;
			if (std::system(cmd) != 0)
				std::fprintf(stderr, "command failed\n");
		}
	}
	if (drag[0] >= 0)
	{
		char text[128] = {};
		if (gOut.lastId != CLAP_INVALID_ID)
			params->value_to_text(p, gOut.lastId, gOut.lastValue, text, sizeof(text));
		double v = -1;
		if (gOut.lastId != CLAP_INVALID_ID)
			params->get_value(p, gOut.lastId, &v);
		std::printf("GUI edits seen by host: %d begin, %d values, %d end; param %u = %g (\"%s\"), get_value %g\n",
		            gOut.begins, gOut.values, gOut.ends, gOut.lastId, gOut.lastValue, text, v);
	}
	// optional: scroll at (x,y) and take numbered screenshots: CLAP_GUI_TEST_SCROLL="x,y,pages,cmd%d"
	if (const char* sc = std::getenv("CLAP_GUI_TEST_SCROLL"))
	{
		int sx = 0, sy = 0, pages = 0, used = 0;
		if (std::sscanf(sc, "%d,%d,%d,%n", &sx, &sy, &pages, &used) == 3)
		{
			std::string tmpl = sc + used;
			for (int pg = 1; pg <= pages; ++pg)
			{
				XTestFakeMotionEvent(dpy, -1, sx, sy, 0);
				for (int k = 0; k < 6; ++k)
				{
					XTestFakeButtonEvent(dpy, 5, True, 0);
					XTestFakeButtonEvent(dpy, 5, False, 0);
				}
				XFlush(dpy);
				const auto until = Clock::now() + std::chrono::milliseconds(400);
				while (Clock::now() < until)
				{
					poll(nullptr, 0, 5);
					for (auto& kv : gFds)
						if (fdExt)
							fdExt->on_fd(p, kv.first, CLAP_POSIX_FD_READ);
					while (XPending(dpy))
					{
						XEvent ev;
						XNextEvent(dpy, &ev);
					}
					for (auto& kv : gTimers)
						if (timerExt)
							timerExt->on_timer(p, kv.first);
				}
				XSync(dpy, False);
				char cmdbuf[1024];
				std::snprintf(cmdbuf, sizeof(cmdbuf), tmpl.c_str(), pg);
				if (std::system(cmdbuf) != 0)
					std::fprintf(stderr, "command failed\n");
			}
		}
	}
	if (doProcess)
	{
		p->stop_processing(p);
		p->deactivate(p);
		std::printf("processed %lld samples\n", (long long)steady);
	}
	std::printf("timer callbacks %llu, fd callbacks %llu, resize requests %d\n",
	            (unsigned long long)timerFires, (unsigned long long)fdFires, gResizeRequests);

	// resize through the host side too
	if (gui->can_resize(p))
	{
		uint32_t nw = w + 100, nh = h + 50;
		gui->adjust_size(p, &nw, &nh);
		std::printf("set_size %ux%u -> %d\n", nw, nh, int(gui->set_size(p, nw, nh)));
	}

	gui->destroy(p);
	XDestroyWindow(dpy, win);
	XCloseDisplay(dpy);
	p->destroy(p);
	entry->deinit();
	std::printf("gui test done\n");
	return 0;
}
