#include "GetRepo.hpp"
#include "../Utils.hpp"
#include "constants.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include <regex>
#include <sstream>
#include <iomanip>

using namespace rapidjson;

std::vector<std::unique_ptr<Package>> GetRepo::loadPackages()
{
	std::vector<std::unique_ptr<Package>> result;
	std::string directoryUrl = this->url + "/repo.json";

	// fetch current repository json
	std::string response;
	bool success = downloadFileToMemory(directoryUrl, &response);

#ifdef NETWORK_MOCK
	mockPopulatePackages(&response);
#endif

	// attempt fallback to http in case of https repos failure
	if (!success && (this->url.rfind("https", 0) == 0))
	{
		printf("--> Attempting http fallback for https repo \"%s\" after loading failure\n", this->name.c_str());

		// update repo url
		this->url.replace(0, 5, "http");
		directoryUrl = this->url + "/repo.json";

		// retry fetch
		success = downloadFileToMemory(directoryUrl, &response);
	}

	if (!success)
	{
		printf("--> Could not update repository metadata for \"%s\" repo!\n", this->name.c_str());
		this->loaded = false;
		return {};
	}

	if (libget_status_callback != nullptr)
		libget_status_callback(STATUS_UPDATING_STATUS, 1, 1);

	// extract out packages, append to package list
	Document doc;
	ParseResult ok = doc.Parse(response.c_str());

	if (!ok)
	{
		printf("--> JSON parse error in repo.json for %s: %s (offset %zu)\n",
			this->url.c_str(), GetParseError_En(ok.Code()), ok.Offset());
		printf("--> Response preview (first 200 chars): %.200s\n", response.c_str());
		this->loaded = false;
		return {};
	}

	if (!doc.IsObject() || !doc.HasMember("packages"))
	{
		printf("--> Invalid format in downloaded repo.json for %s\n", this->url.c_str());
		this->loaded = false;
		return {};
	}

	const Value& packages_doc = doc["packages"];

	// for every repo
	auto total = packages_doc.Size();
	for (int i = 0; i < (int)total; i++)
	{
		if (networking_callback != nullptr) {
			double progress = (double)(i + 1) / (double)total;
			networking_callback(nullptr, progress);
		}

		auto package = std::make_unique<Package>(GET);

		// TODO: use arrays and loops for parsing this info, and also check the type first

		auto& cur = packages_doc[i];

		// mostly essential attributes
		if (cur.HasMember("name"))
		{
			package->pkg_name = cur["name"].GetString();
		}
		else
		{
			printf("Missing name for package on repo, skipping\n");
			continue;
		}
		if (cur.HasMember("title"))
			package->title = cur["title"].GetString();
		else
			package->title = package->pkg_name;

		if (cur.HasMember("author"))
			package->author = cur["author"].GetString();
		if (cur.HasMember("description"))
			package->short_desc = cur["description"].GetString();
		if (cur.HasMember("details"))
			package->long_desc = std::regex_replace(cur["details"].GetString(), std::regex("\\\\n"), "\n");
		if (cur.HasMember("version"))
			package->version = cur["version"].GetString();

		// more information and details
		if (cur.HasMember("license"))
			package->license = cur["license"].GetString();
		if (cur.HasMember("changelog"))
			package->changelog = std::regex_replace(cur["changelog"].GetString(), std::regex("\\\\n"), "\n");
		if (cur.HasMember("url"))
		{
			package->url = cur["url"].GetString();
			package->sourceUrl = package->url;
		}
		if (cur.HasMember("file") && cur["file"].IsString())
			package->file = cur["file"].GetString();
		if (cur.HasMember("direct_url") && cur["direct_url"].IsString())
			package->directUrl = cur["direct_url"].GetString();
		else if (!package->file.empty())
			package->directUrl = package->url;
		if (cur.HasMember("path") && cur["path"].IsString())
			package->path = cur["path"].GetString();
		if (cur.HasMember("requiredDeletion") && cur["requiredDeletion"].IsArray())
		{
			for (auto& v : cur["requiredDeletion"].GetArray())
				if (v.IsString())
					package->requiredDeletion.push_back(v.GetString());
		}
		if (cur.HasMember("userDeletion") && cur["userDeletion"].IsArray())
		{
			for (auto& v : cur["userDeletion"].GetArray())
				if (v.IsString())
					package->userDeletion.push_back(v.GetString());
		}
		// hpak direct-download packages: pull per-release metadata when the
		// repo entry doesn't carry the full set (skipped otherwise to save
		// rate limits). Keeps version/title/etc. current for status checks.
		if (!package->file.empty())
		{
			bool hasFull = !package->title.empty() && !package->author.empty() &&
			               !package->version.empty() && !package->short_desc.empty() &&
			               !package->long_desc.empty() && !package->license.empty() &&
			               !package->changelog.empty();
			if (!hasFull)
			{
				std::string base = !package->directUrl.empty() ? package->directUrl : package->url;
				while (!base.empty() && base.back() == '/') base.pop_back();
				std::string content;
				if (!base.empty() && downloadFileToMemory(base + "/package.json", &content))
				{
					Document pj;
					if (!pj.Parse(content.c_str()).HasParseError() && pj.IsObject())
					{
						auto getStr = [&](const char* key) -> std::string {
							auto it = pj.FindMember(key);
							if (it != pj.MemberEnd() && it->value.IsString())
								return it->value.GetString();
							return "";
						};
						auto setIf = [&](std::string& field, const char* key) {
							std::string v = getStr(key);
							if (!v.empty()) field = v;
						};
						std::string t = getStr("title");
						if (t.empty()) t = getStr("name");
						if (!t.empty()) package->title = t;
						setIf(package->author, "author");
						setIf(package->version, "version");
						setIf(package->short_desc, "description");
						if (package->short_desc.empty()) setIf(package->short_desc, "short_desc");
						setIf(package->long_desc, "details");
						if (package->long_desc.empty()) setIf(package->long_desc, "long_desc");
						setIf(package->license, "license");
						setIf(package->changelog, "changelog");
						if (pj.HasMember("requiredDeletion") && pj["requiredDeletion"].IsArray())
						{
							for (auto& v : pj["requiredDeletion"].GetArray())
								if (v.IsString())
									package->requiredDeletion.push_back(v.GetString());
						}
						if (pj.HasMember("userDeletion") && pj["userDeletion"].IsArray())
						{
							for (auto& v : pj["userDeletion"].GetArray())
								if (v.IsString())
									package->userDeletion.push_back(v.GetString());
						}
						if (pj.HasMember("dependencies") && pj["dependencies"].IsObject())
						{
							for (auto it = pj["dependencies"].MemberBegin(); it != pj["dependencies"].MemberEnd(); ++it)
							{
								if (!it->name.IsString())
									continue;
								std::string vc = "*";
								if (it->value.IsString())
									vc = it->value.GetString();
								package->dependencies.push_back({it->name.GetString(), vc});
							}
						}
						if (pj.HasMember("depends") && pj["depends"].IsArray())
						{
							for (auto& v : pj["depends"].GetArray())
								if (v.IsString())
									package->dependencies.push_back({v.GetString(), "*"});
						}
					}
				}
			}
		}
		if (cur.HasMember("updated"))
		{
			package->updated = cur["updated"].GetString();
			struct tm tm {};

			auto res = my_strptime(package->updated.c_str(), "%d/%m/%Y", &tm);
			if (res)
			{
				// make sure that all the time-related fields are set
				tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
				package->updated_timestamp = (int)mktime(&tm);

				// also update the label to a simpler y-m-d format
				std::stringstream ss;
				ss << std::put_time(&tm, "%Y-%m-%d");
				package->updated = ss.str();
			}
		}
		// hpak datetime field: "DD/MM/YYYY HH:MM" (24hr). Overrides "updated" when present.
		if (cur.HasMember("UpdateDateTime") && cur["UpdateDateTime"].IsString())
		{
			std::string dt = cur["UpdateDateTime"].GetString();
			struct tm tm {};

			auto res = my_strptime(dt.c_str(), "%d/%m/%Y %H:%M", &tm);
			if (res)
			{
				tm.tm_sec = 0;
				package->updated_timestamp = (int)mktime(&tm);

				std::stringstream ss;
				ss << std::put_time(&tm, "%Y-%m-%d %H:%M");
				package->updated = ss.str();
			}
		}

		// even more details
		if (cur.HasMember("app_dls"))
			package->downloads += cur["app_dls"].GetInt();
		if (cur.HasMember("web_dls"))
			package->downloads += cur["web_dls"].GetInt();
		if (cur.HasMember("extracted"))
			// convert KB to bytes
			package->extracted_size += cur["extracted"].GetInt() * 1000;
		if (cur.HasMember("filesize"))
			package->download_size += cur["filesize"].GetInt() * 1000; // ibid

		if (cur.HasMember("category"))
			package->category = cur["category"].GetString();
		if (cur.HasMember("binary"))
			package->binary = cur["binary"].GetString();
		if (cur.HasMember("screens"))
			package->screens = cur["screens"].GetInt();

		const Value* depsVal = nullptr;
		if (cur.HasMember("depends"))
			depsVal = &cur["depends"];
		else if (cur.HasMember("dependencies"))
			depsVal = &cur["dependencies"];
		if (depsVal != nullptr)
		{
			if (depsVal->IsArray())
			{
				for (SizeType j = 0; j < depsVal->Size(); j++)
				{
					const auto& dep = (*depsVal)[j];
					if (dep.IsString())
						package->dependencies.push_back({dep.GetString(), "*"});
				}
			}
			else if (depsVal->IsObject())
			{
				for (auto it = depsVal->MemberBegin(); it != depsVal->MemberEnd(); ++it)
				{
					if (!it->name.IsString())
						continue;
					std::string versionConstraint = "*";
					if (it->value.IsString())
						versionConstraint = it->value.GetString();
					package->dependencies.push_back({it->name.GetString(), versionConstraint});
				}
			}
		}

		result.push_back(std::move(package));
	}
	return result;
}

std::string GetRepo::getType() const
{
	return "get";
}

std::string GetRepo::getZipUrl(const Package& package) const
{
	// Get packages are in the /zips folder under the package name
	return this->url + "/zips/" + package.getPackageName() + ".zip";
}

std::string GetRepo::getIconUrl(const Package& package) const
{
	// Get icons are also just in the /packages folder
	return this->url + "/packages/" + package.getPackageName() + "/icon.png";
}
