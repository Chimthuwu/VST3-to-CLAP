// vst3-to-clap - discovery of VST3 classes and the CLAP descriptors that expose them
#pragma once

#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

#include "public.sdk/source/vst/hosting/module.h"

namespace v3c {

// One VST3 audio processor class exposed as one CLAP plugin.
struct PluginEntry
{
	std::string modulePath; // path of the .vst3 bundle / file
	std::string classIdHex; // 32 hex chars, VST3 class UID
	VST3::UID classId;

	// storage for the strings the descriptor points into
	std::string clapId, name, vendor, url, version, description;
	std::vector<std::string> featureStrings;
	std::vector<const char*> featurePtrs;

	clap_plugin_descriptor_t descriptor{};

	void finalize(); // fills `descriptor` from the strings above
};

// Configuration baked into a converted .clap by the `vst3-to-clap` tool.
struct EmbeddedConfig
{
	std::string vst3Path; // absolute, or relative to the .clap file
	std::string classFilter; // optional: class UID (hex) or class name; empty = all classes
	bool valid = false;
};
EmbeddedConfig readEmbeddedConfig();

class Registry
{
public:
	static Registry& instance();

	// Called from clap_entry.init(). `clapPath` is the path of the .clap file/bundle.
	bool init(const char* clapPath);
	void deinit();

	uint32_t count() const { return static_cast<uint32_t>(entries.size()); }
	const PluginEntry* at(uint32_t index) const;
	const PluginEntry* find(const char* clapId) const;

	// Shared, ref-counted module loading. Modules stay loaded while any instance holds them.
	VST3::Hosting::Module::Ptr acquireModule(const std::string& path, std::string& error);

private:
	void scanModule(const std::string& path, const std::string& classFilter);
	bool scanModuleInfoJson(const std::string& path, const std::string& classFilter);
	void addEntry(const std::string& path, const std::string& cidHex, const std::string& name,
	              const std::string& vendor, const std::string& version, const std::string& url,
	              const std::vector<std::string>& subCategories);

	std::vector<std::unique_ptr<PluginEntry>> entries;
	int initCount = 0;
};

// Collect .vst3 bundles/files beneath `root` (or `root` itself if it is one).
void findVst3Modules(const std::string& root, std::vector<std::string>& out);

std::string clapIdForClass(const std::string& classIdHex);

// IHostApplication passed to every module via IPluginFactory3::setHostContext()
Steinberg::FUnknown* moduleHostContext();

} // namespace v3c
