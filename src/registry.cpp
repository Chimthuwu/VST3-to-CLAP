// vst3-to-clap - discovery of VST3 classes and the CLAP descriptors that expose them
#include "registry.h"
#include "embed.h"
#include "runloop.h"

#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/moduleinfo/moduleinfoparser.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

//------------------------------------------------------------------------
// The embedded configuration block. The `vst3-to-clap` converter finds the marker in a
// copy of this binary and writes "<vst3 path>\0<class filter>\0" right after it.
// It is initialised with a non-zero marker so it lives in .data (i.e. in the file).
//------------------------------------------------------------------------
extern "C" {
#if defined(__GNUC__) || defined(__clang__)
__attribute__((used))
#endif
volatile char v3c_embedded_config[V3C_EMBED_TOTAL_SIZE] = V3C_EMBED_MARKER;
}

namespace v3c {

EmbeddedConfig readEmbeddedConfig()
{
	EmbeddedConfig cfg;
	constexpr size_t markerLen = sizeof(V3C_EMBED_MARKER) - 1;
	std::string fields[2];
	size_t pos = markerLen;
	for (int f = 0; f < 2 && pos < V3C_EMBED_TOTAL_SIZE; ++f)
	{
		while (pos < V3C_EMBED_TOTAL_SIZE)
		{
			char c = v3c_embedded_config[pos++];
			if (c == 0)
				break;
			fields[f].push_back(c);
		}
	}
	cfg.vst3Path = fields[0];
	cfg.classFilter = fields[1];
	cfg.valid = !cfg.vst3Path.empty();
	return cfg;
}

//------------------------------------------------------------------------
std::string clapIdForClass(const std::string& classIdHex)
{
	return "vst3." + classIdHex;
}

static std::string canonicalCid(const VST3::UID& uid)
{
	Steinberg::char8 buf[64] = {};
	Steinberg::FUID::fromTUID(uid.data()).toString(buf);
	return buf;
}

static std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

static bool hasVst3Extension(const fs::path& p)
{
	return lower(p.extension().string()) == ".vst3";
}

void findVst3Modules(const std::string& root, std::vector<std::string>& out)
{
	std::error_code ec;
	fs::path p(root);
	if (!fs::exists(p, ec))
		return;
	if (hasVst3Extension(p))
	{
		out.push_back(fs::absolute(p, ec).string());
		return;
	}
	if (!fs::is_directory(p, ec))
		return;
	for (auto it = fs::recursive_directory_iterator(
	         p, fs::directory_options::follow_directory_symlink |
	                fs::directory_options::skip_permission_denied,
	         ec);
	     it != fs::recursive_directory_iterator(); it.increment(ec))
	{
		if (ec)
			break;
		if (hasVst3Extension(it->path()))
		{
			out.push_back(it->path().string());
			if (it->is_directory(ec))
				it.disable_recursion_pending(); // do not descend into bundles
		}
	}
}

//------------------------------------------------------------------------
void PluginEntry::finalize()
{
	featurePtrs.clear();
	for (auto& f : featureStrings)
		featurePtrs.push_back(f.c_str());
	featurePtrs.push_back(nullptr);

	descriptor.clap_version = CLAP_VERSION_INIT;
	descriptor.id = clapId.c_str();
	descriptor.name = name.c_str();
	descriptor.vendor = vendor.c_str();
	descriptor.url = url.c_str();
	descriptor.manual_url = "";
	descriptor.support_url = url.c_str();
	descriptor.version = version.c_str();
	descriptor.description = description.c_str();
	descriptor.features = featurePtrs.data();
}

static std::vector<std::string> mapFeatures(const std::vector<std::string>& subCategories)
{
	// VST3 sub categories look like "Fx|Delay|Stereo" (already split into tokens here).
	std::vector<std::string> out;
	std::set<std::string> seen;
	auto add = [&](const std::string& f) {
		if (!f.empty() && seen.insert(f).second)
			out.push_back(f);
	};

	bool instrument = false, fx = false;
	for (const auto& raw : subCategories)
	{
		const std::string s = lower(raw);
		if (s == "instrument")
			instrument = true;
		else if (s == "fx")
			fx = true;
	}
	if (instrument)
		add(CLAP_PLUGIN_FEATURE_INSTRUMENT);
	if (fx || !instrument)
		add(CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);

	static const std::map<std::string, const char*> table = {
	    {"synth", CLAP_PLUGIN_FEATURE_SYNTHESIZER},
	    {"sampler", CLAP_PLUGIN_FEATURE_SAMPLER},
	    {"drum", CLAP_PLUGIN_FEATURE_DRUM},
	    {"piano", CLAP_PLUGIN_FEATURE_SYNTHESIZER},
	    {"external", CLAP_PLUGIN_FEATURE_INSTRUMENT},
	    {"analyzer", CLAP_PLUGIN_FEATURE_ANALYZER},
	    {"delay", CLAP_PLUGIN_FEATURE_DELAY},
	    {"distortion", CLAP_PLUGIN_FEATURE_DISTORTION},
	    {"dynamics", CLAP_PLUGIN_FEATURE_COMPRESSOR},
	    {"eq", CLAP_PLUGIN_FEATURE_EQUALIZER},
	    {"filter", CLAP_PLUGIN_FEATURE_FILTER},
	    {"modulation", CLAP_PLUGIN_FEATURE_CHORUS},
	    {"pitch shift", CLAP_PLUGIN_FEATURE_PITCH_SHIFTER},
	    {"restoration", CLAP_PLUGIN_FEATURE_RESTORATION},
	    {"reverb", CLAP_PLUGIN_FEATURE_REVERB},
	    {"tools", CLAP_PLUGIN_FEATURE_UTILITY},
	    {"generator", CLAP_PLUGIN_FEATURE_SYNTHESIZER},
	    {"spatial", CLAP_PLUGIN_FEATURE_UTILITY},
	    {"mastering", CLAP_PLUGIN_FEATURE_MASTERING},
	    {"mono", CLAP_PLUGIN_FEATURE_MONO},
	    {"stereo", CLAP_PLUGIN_FEATURE_STEREO},
	    {"surround", CLAP_PLUGIN_FEATURE_SURROUND},
	    {"ambisonics", CLAP_PLUGIN_FEATURE_AMBISONIC},
	};
	for (const auto& raw : subCategories)
	{
		auto it = table.find(lower(raw));
		if (it != table.end())
			add(it->second);
	}
	add("vst3-wrapper");
	return out;
}

static std::vector<std::string> splitCategories(const std::vector<std::string>& in)
{
	std::vector<std::string> out;
	for (const auto& s : in)
	{
		std::stringstream ss(s);
		std::string tok;
		while (std::getline(ss, tok, '|'))
			if (!tok.empty())
				out.push_back(tok);
	}
	return out;
}

static bool matchesFilter(const std::string& filter, const std::string& cidHex,
                          const std::string& name)
{
	if (filter.empty())
		return true;
	return lower(filter) == lower(cidHex) || lower(filter) == lower(name);
}

//------------------------------------------------------------------------
Registry& Registry::instance()
{
	static Registry r;
	return r;
}

const PluginEntry* Registry::at(uint32_t index) const
{
	return index < entries.size() ? entries[index].get() : nullptr;
}

const PluginEntry* Registry::find(const char* clapId) const
{
	if (!clapId)
		return nullptr;
	for (auto& e : entries)
		if (e->clapId == clapId)
			return e.get();
	return nullptr;
}

void Registry::addEntry(const std::string& path, const std::string& cidHex,
                        const std::string& name, const std::string& vendor,
                        const std::string& version, const std::string& url,
                        const std::vector<std::string>& subCategories)
{
	const std::string clapId = clapIdForClass(cidHex);
	for (auto& e : entries)
		if (e->clapId == clapId)
			return; // same plugin found twice (e.g. user + system folder) - first one wins

	auto uid = VST3::UID::fromString(cidHex, false);
	if (!uid)
		return;

	auto e = std::make_unique<PluginEntry>();
	e->modulePath = path;
	e->classIdHex = cidHex;
	e->classId = *uid;
	e->clapId = clapId;
	e->name = name;
	e->vendor = vendor;
	e->version = version.empty() ? "1.0.0" : version;
	e->url = url;
	e->description = "VST3 plug-in wrapped as CLAP (" + fs::path(path).filename().string() + ")";
	e->featureStrings = mapFeatures(splitCategories(subCategories));
	e->finalize();
	entries.push_back(std::move(e));
}

bool Registry::scanModuleInfoJson(const std::string& path, const std::string& classFilter)
{
	auto infoPath = VST3::Hosting::Module::getModuleInfoPath(path);
	if (!infoPath)
		return false;
	std::ifstream f(*infoPath, std::ios::binary);
	if (!f)
		return false;
	std::stringstream ss;
	ss << f.rdbuf();
	const std::string json = ss.str();
	auto info = Steinberg::ModuleInfoLib::parseJson(json, nullptr);
	if (!info)
		return false;

	bool any = false;
	for (const auto& c : info->classes)
	{
		if (c.category != kVstAudioEffectClass)
			continue;
		auto uid = VST3::UID::fromString(c.cid);
		if (!uid)
			continue;
		const std::string cidHex = canonicalCid(*uid);
		if (!matchesFilter(classFilter, cidHex, c.name))
			continue;
		addEntry(path, cidHex, c.name, c.vendor.empty() ? info->factoryInfo.vendor : c.vendor,
		         c.version, info->factoryInfo.url, c.subCategories);
		any = true;
	}
	return any;
}

void Registry::scanModule(const std::string& path, const std::string& classFilter)
{
	// Fast path: moduleinfo.json lets us enumerate without loading the binary.
	if (scanModuleInfoJson(path, classFilter))
		return;

	std::string error;
	auto module = acquireModule(path, error);
	if (!module)
		return;
	const auto& factory = module->getFactory();
	const auto factoryInfo = factory.info();
	for (const auto& ci : factory.classInfos())
	{
		if (ci.category() != kVstAudioEffectClass)
			continue;
		const std::string cidHex = canonicalCid(ci.ID());
		if (!matchesFilter(classFilter, cidHex, ci.name()))
			continue;
		addEntry(path, cidHex, ci.name(), ci.vendor().empty() ? factoryInfo.vendor() : ci.vendor(),
		         ci.version(), factoryInfo.url(), ci.subCategories());
	}
	// `module` goes out of scope here; it is unloaded again unless an instance holds it.
}

static std::vector<std::string> splitPathList(const std::string& s)
{
#if defined(_WIN32)
	const char sep = ';';
#else
	const char sep = ':';
#endif
	std::vector<std::string> out;
	std::string cur;
	for (char c : s)
	{
		if (c == sep)
		{
			if (!cur.empty())
				out.push_back(cur);
			cur.clear();
		}
		else
			cur.push_back(c);
	}
	if (!cur.empty())
		out.push_back(cur);
	return out;
}

bool Registry::init(const char* clapPath)
{
	if (initCount++ > 0)
		return true;

	std::vector<std::string> modules;
	std::string classFilter;

	const EmbeddedConfig cfg = readEmbeddedConfig();
	if (cfg.valid)
	{
		// Converted plug-in: wrap exactly one VST3 (optionally one class of it).
		fs::path p = fs::u8path(cfg.vst3Path);
		if (p.is_relative() && clapPath)
		{
			fs::path base = fs::u8path(clapPath);
			p = base.parent_path() / p;
		}
		modules.push_back(p.lexically_normal().u8string());
		classFilter = cfg.classFilter;
	}
	else
	{
		// Shell mode: wrap every VST3 found on VST3_CLAP_PATH, or in the standard folders.
		const char* env = std::getenv("VST3_CLAP_PATH");
		if (env && *env)
		{
			for (auto& dir : splitPathList(env))
				findVst3Modules(dir, modules);
		}
		else
		{
			for (auto& p : VST3::Hosting::Module::getModulePaths())
				modules.push_back(p);
		}
	}

	std::sort(modules.begin(), modules.end());
	modules.erase(std::unique(modules.begin(), modules.end()), modules.end());
	for (auto& m : modules)
		scanModule(m, classFilter);
	return true;
}

void Registry::deinit()
{
	if (initCount == 0 || --initCount > 0)
		return;
	entries.clear();
}

//------------------------------------------------------------------------
namespace {
class ModuleHostContext : public Steinberg::Vst::HostApplication
{
public:
	Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
	{
		Steinberg::Vst::StringConvert::convert(std::string("VST3-to-CLAP"), name);
		return Steinberg::kResultOk;
	}
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override
	{
#if SMTG_OS_LINUX
		if (Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Linux::IRunLoop::iid))
		{
			*obj = static_cast<Steinberg::Linux::IRunLoop*>(&SharedRunLoop::instance());
			return Steinberg::kResultOk;
		}
#endif
		return HostApplication::queryInterface(iid, obj);
	}
};
} // namespace

Steinberg::FUnknown* moduleHostContext()
{
	static ModuleHostContext ctx;
	return static_cast<Steinberg::Vst::IHostApplication*>(&ctx);
}

// VST3 modules may keep pointers to our host objects (e.g. the Linux run loop handed over
// via setHostContext) in their own static data, and some modules can never be unloaded
// (anything linking GTK/glib, for instance). If the host later dlclose()s this .clap, those
// pointers would dangle and crash at process exit. Once a VST3 module has been loaded we
// therefore keep this library resident.
static void pinSelf()
{
	static bool pinned = false;
	if (pinned)
		return;
	pinned = true;
#if defined(_WIN32)
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
	                   reinterpret_cast<LPCWSTR>(&pinSelf), &self);
#else
	Dl_info info{};
	if (dladdr(reinterpret_cast<void*>(&pinSelf), &info) && info.dli_fname)
		dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE);
#endif
}

static std::mutex gModuleMutex;
static std::map<std::string, std::weak_ptr<VST3::Hosting::Module>> gModules;

VST3::Hosting::Module::Ptr Registry::acquireModule(const std::string& path, std::string& error)
{
	std::lock_guard<std::mutex> lock(gModuleMutex);
	auto it = gModules.find(path);
	if (it != gModules.end())
		if (auto m = it->second.lock())
			return m;
	auto m = VST3::Hosting::Module::create(path, error);
	if (m)
	{
		pinSelf();
		gModules[path] = m;
		// module-wide host context (IPluginFactory3); on Linux it carries the run loop
		m->getFactory().setHostContext(moduleHostContext());
	}
	return m;
}

} // namespace v3c
