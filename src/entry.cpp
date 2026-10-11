// vst3-to-clap - CLAP entry point and plugin factory
#include <clap/clap.h>

#include <cstring>

#include "plugin.h"
#include "registry.h"

namespace {

uint32_t factoryGetCount(const clap_plugin_factory_t*)
{
	return v3c::Registry::instance().count();
}

const clap_plugin_descriptor_t* factoryGetDescriptor(const clap_plugin_factory_t*, uint32_t index)
{
	auto* e = v3c::Registry::instance().at(index);
	return e ? &e->descriptor : nullptr;
}

const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                   const char* pluginId)
{
	if (!host || !clap_version_is_compatible(host->clap_version))
		return nullptr;
	auto* e = v3c::Registry::instance().find(pluginId);
	if (!e)
		return nullptr;
	auto* w = new v3c::WrapperPlugin(host, e);
	return w->clapPlugin();
}

const clap_plugin_factory_t kFactory = {factoryGetCount, factoryGetDescriptor, factoryCreate};

bool entryInit(const char* pluginPath)
{
	return v3c::Registry::instance().init(pluginPath);
}

void entryDeinit()
{
	v3c::Registry::instance().deinit();
}

const void* entryGetFactory(const char* factoryId)
{
	if (factoryId && std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
		return &kFactory;
	return nullptr;
}

} // namespace

extern "C" {
#if defined(_WIN32)
__declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((visibility("default")))
#endif
extern const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory};
}
