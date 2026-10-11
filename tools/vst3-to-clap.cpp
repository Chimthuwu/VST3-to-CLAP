// vst3-to-clap - command line converter
//
// Produces a standalone .clap for a given .vst3 by copying the generic wrapper binary and
// writing the VST3 location into its embedded configuration block.
//
//   vst3-to-clap MyPlugin.vst3                      -> ./MyPlugin.clap (all classes)
//   vst3-to-clap MyPlugin.vst3 -o ~/.clap/My.clap
//   vst3-to-clap MyPlugin.vst3 --list               (show the classes inside)
//   vst3-to-clap MyPlugin.vst3 --class "My Synth"   (wrap only one class)
//   vst3-to-clap MyPlugin.vst3 --copy-vst3          (copy the .vst3 next to the .clap and
//                                                    reference it relatively)

#include "../src/embed.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/module.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

static fs::path executablePath(const char* argv0)
{
#if defined(_WIN32)
	wchar_t buf[MAX_PATH * 4];
	DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
	if (n > 0)
		return fs::path(std::wstring(buf, n));
#elif defined(__APPLE__)
	char buf[4096];
	uint32_t size = sizeof(buf);
	if (_NSGetExecutablePath(buf, &size) == 0)
		return fs::canonical(buf);
#else
	std::error_code ec;
	auto p = fs::read_symlink("/proc/self/exe", ec);
	if (!ec)
		return p;
#endif
	return fs::absolute(argv0);
}

static void usage()
{
	std::cerr <<
	    R"(vst3-to-clap - wrap a VST3 plug-in as a CLAP plug-in

usage: vst3-to-clap <plugin.vst3> [options]

options:
  -o, --output <path>     output .clap path (default: ./<name>.clap)
  -c, --class <uid|name>  wrap only this class (default: every audio class in the module)
  -l, --list              list the audio classes in the module and exit
      --copy-vst3         copy the .vst3 next to the output and reference it relatively
      --relative          store the .vst3 location relative to the output .clap
      --template <path>   wrapper binary to use (default: vst3-to-clap-wrapper.clap next
                          to this executable)
  -h, --help              show this help
)";
}

static std::string cidString(const VST3::UID& uid)
{
	Steinberg::char8 buf[64] = {};
	Steinberg::FUID::fromTUID(uid.data()).toString(buf);
	return buf;
}

static int listClasses(const std::string& vst3, const std::string& filter, bool& filterMatched)
{
	std::string err;
	auto module = VST3::Hosting::Module::create(vst3, err);
	if (!module)
	{
		std::cerr << "error: cannot load " << vst3 << ": " << err << "\n";
		return -1;
	}
	int count = 0;
	filterMatched = filter.empty();
	for (auto& ci : module->getFactory().classInfos())
	{
		if (ci.category() != kVstAudioEffectClass)
			continue;
		const std::string cid = cidString(ci.ID());
		const bool match = !filter.empty() && (filter == cid || filter == ci.name());
		filterMatched |= match;
		if (filter.empty() || match)
		{
			std::cout << "  " << cid << "  " << ci.name() << "  [" << ci.subCategoriesString()
			          << "]  clap id: vst3." << cid << "\n";
			++count;
		}
	}
	return count;
}

static fs::path findBundleBinary(const fs::path& bundle)
{
	fs::path macos = bundle / "Contents" / "MacOS";
	std::error_code ec;
	for (auto& e : fs::directory_iterator(macos, ec))
		if (e.is_regular_file())
			return e.path();
	return {};
}

static bool patchBinary(const fs::path& file, const std::string& vst3Path, const std::string& filter)
{
	std::vector<char> data;
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return false;
		data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	const std::string marker = V3C_EMBED_MARKER;
	auto first = std::search(data.begin(), data.end(), marker.begin(), marker.end());
	if (first == data.end())
	{
		std::cerr << "error: " << file << " is not a vst3-to-clap wrapper (marker not found)\n";
		return false;
	}
	// a universal (fat) macOS binary contains one block per architecture: patch them all
	std::string payload = vst3Path;
	payload.push_back('\0');
	payload += filter;
	payload.push_back('\0');
	const size_t capacity = V3C_EMBED_TOTAL_SIZE - marker.size();
	if (payload.size() > capacity)
	{
		std::cerr << "error: path too long for the embedded configuration block\n";
		return false;
	}
	int patched = 0;
	for (auto it = first; it != data.end();
	     it = std::search(it + 1, data.end(), marker.begin(), marker.end()))
	{
		auto dst = it + marker.size();
		if (size_t(data.end() - dst) < capacity)
			break;
		std::fill(dst, dst + capacity, '\0');
		std::copy(payload.begin(), payload.end(), dst);
		++patched;
	}
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	out.write(data.data(), std::streamsize(data.size()));
	return patched > 0 && bool(out);
}

int main(int argc, char** argv)
{
	std::string input, output, filter, templatePath;
	bool list = false, copyVst3 = false, relative = false;

	for (int i = 1; i < argc; ++i)
	{
		std::string a = argv[i];
		auto next = [&]() -> std::string {
			if (i + 1 >= argc)
			{
				std::cerr << "error: " << a << " needs a value\n";
				std::exit(2);
			}
			return argv[++i];
		};
		if (a == "-h" || a == "--help")
		{
			usage();
			return 0;
		}
		else if (a == "-o" || a == "--output")
			output = next();
		else if (a == "-c" || a == "--class")
			filter = next();
		else if (a == "-l" || a == "--list")
			list = true;
		else if (a == "--copy-vst3")
			copyVst3 = true;
		else if (a == "--relative")
			relative = true;
		else if (a == "--template")
			templatePath = next();
		else if (!a.empty() && a[0] == '-')
		{
			std::cerr << "error: unknown option " << a << "\n";
			usage();
			return 2;
		}
		else if (input.empty())
			input = a;
		else
		{
			std::cerr << "error: unexpected argument " << a << "\n";
			return 2;
		}
	}
	if (input.empty())
	{
		usage();
		return 2;
	}

	std::error_code ec;
	fs::path in = fs::absolute(input, ec);
	while (!in.empty() && !in.has_filename())
		in = in.parent_path(); // strip a trailing separator
	if (!fs::exists(in))
	{
		std::cerr << "error: " << in << " does not exist\n";
		return 1;
	}

	// The class filter is matched in upper case UID form or by exact name.
	if (filter.size() == 32 && std::all_of(filter.begin(), filter.end(), ::isxdigit))
		std::transform(filter.begin(), filter.end(), filter.begin(), ::toupper);

	std::cout << "Audio classes in " << in.filename().string() << ":\n";
	bool filterMatched = false;
	const int n = listClasses(in.string(), filter, filterMatched);
	if (n < 0)
		return 1;
	if (n == 0)
	{
		std::cerr << (filter.empty() ? "error: no audio processor classes found\n"
		                             : "error: no class matches '" + filter + "'\n");
		return 1;
	}
	if (list)
		return 0;

	fs::path tmpl = templatePath.empty()
	                    ? executablePath(argv[0]).parent_path() / "vst3-to-clap-wrapper.clap"
	                    : fs::path(templatePath);
	if (!fs::exists(tmpl))
	{
		std::cerr << "error: wrapper template not found at " << tmpl
		          << " (use --template <path>)\n";
		return 1;
	}

	fs::path out = output.empty() ? fs::current_path() / (in.stem().string() + ".clap")
	                              : fs::absolute(output);
	fs::create_directories(out.parent_path(), ec);

	// where the wrapper will look for the VST3
	fs::path vst3Target = in;
	if (copyVst3)
	{
		vst3Target = out.parent_path() / in.filename();
		if (fs::equivalent(vst3Target, in, ec))
			; // already there
		else
		{
			fs::remove_all(vst3Target, ec);
			fs::copy(in, vst3Target, fs::copy_options::recursive | fs::copy_options::copy_symlinks,
			         ec);
			if (ec)
			{
				std::cerr << "error: copying " << in << ": " << ec.message() << "\n";
				return 1;
			}
		}
		relative = true;
	}
	std::string embedded = vst3Target.u8string();
	if (relative)
		embedded = fs::relative(vst3Target, out.parent_path()).u8string();

	// copy the template (a file on Linux/Windows, a bundle directory on macOS)
	fs::remove_all(out, ec);
	fs::path binary;
	if (fs::is_directory(tmpl))
	{
		fs::copy(tmpl, out, fs::copy_options::recursive | fs::copy_options::copy_symlinks, ec);
		binary = findBundleBinary(out);
		// give each converted bundle its own identifier and name
		fs::path plist = out / "Contents" / "Info.plist";
		std::ifstream pin(plist);
		if (pin)
		{
			std::string text((std::istreambuf_iterator<char>(pin)), std::istreambuf_iterator<char>());
			pin.close();
			std::string slug;
			for (char c : out.stem().string())
				slug.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '-');
			auto replaceAll = [&](const std::string& from, const std::string& to) {
				for (size_t pos = 0; (pos = text.find(from, pos)) != std::string::npos; pos += to.size())
					text.replace(pos, from.size(), to);
			};
			replaceAll("org.vst3-to-clap.wrapper", "org.vst3-to-clap." + slug);
			replaceAll("<string>vst3-to-clap-wrapper</string>\n\t<key>CFBundlePackageType",
			           "<string>" + out.stem().string() + "</string>\n\t<key>CFBundlePackageType");
			std::ofstream pout(plist, std::ios::trunc);
			pout << text;
		}
	}
	else
	{
		fs::copy_file(tmpl, out, fs::copy_options::overwrite_existing, ec);
		binary = out;
	}
	if (ec || binary.empty())
	{
		std::cerr << "error: cannot write " << out << (ec ? ": " + ec.message() : "") << "\n";
		return 1;
	}
	if (!patchBinary(binary, embedded, filter))
		return 1;

#if defined(__APPLE__)
	// patching invalidates the signature; re-sign ad hoc so the host will load it
	std::string cmd = "codesign --force --sign - \"" + out.string() + "\" >/dev/null 2>&1";
	if (std::system(cmd.c_str()) != 0)
		std::cerr << "warning: ad-hoc codesign failed; sign " << out << " manually\n";
#endif

	std::cout << "Wrote " << out.string() << "  ->  " << embedded << "\n";
	return 0;
}
