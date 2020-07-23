/*
 * This file is part of the Colobot: Gold Edition source code
 * Copyright (C) 2001-2020, Daniel Roux, EPSITEC SA & TerranovaTeam
 * http://epsitec.ch; http://colobot.info; http://github.com/colobot
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see http://gnu.org/licenses
 */

#include "app/modman.h"

#include "common/config.h"

#include "app/app.h"
#include "app/pathman.h"

#include "common/config_file.h"
#include "common/logger.h"

#include "common/resources/resourcemanager.h"

#include "level/parser/parser.h"

#include <algorithm>
#include <map>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

using namespace boost::filesystem;

CModManager::CModManager(CApplication* app, CPathManager* pathManager)
    : m_app{app},
      m_pathManager{pathManager}
{
}

void CModManager::Init()
{
    // Load names from the config file
    std::vector<std::string> savedModNames;
    GetConfigFile().GetArrayProperty("Mods", "Names", savedModNames);
    std::vector<bool> savedEnabled;
    GetConfigFile().GetArrayProperty("Mods", "Enabled", savedEnabled);

    // Transform the data into Mod structures
    m_mods.reserve(savedModNames.size());
    for (size_t i = 0; i < savedModNames.size(); ++i)
    {
        Mod mod{};
        mod.name = savedModNames[i];
        if (i < savedEnabled.size())
        {
            mod.enabled = savedEnabled[i];
        }
        mod.path = ""; // Find the path later
        m_mods.push_back(mod);
    }

    // Search the folders for mods
    auto rawPaths = m_pathManager->FindMods();
    std::map<std::string, std::string> modPaths;
    for (const auto& path : rawPaths)
    {
        auto modName = boost::filesystem::path(path).stem().string();
        modPaths.insert(std::make_pair(modName, path));
    }

    // Find paths for already saved mods
    auto it = m_mods.begin();
    while (it != m_mods.end())
    {
        auto& mod = *it;
        const auto pathsIt = modPaths.find(mod.name);
        if (pathsIt != modPaths.end())
        {
            mod.path = (*pathsIt).second;
            modPaths.erase(pathsIt);
            ++it;
        }
        else
        {
            GetLogger()->Warn("Could not find mod %s, removing it from the list\n", mod.name.c_str());
            it = m_mods.erase(it);
        }
    }

    // Add the remaining found mods to the end of the list
    for (const auto& newMod : modPaths)
    {
        Mod mod{};
        mod.name = newMod.first;
        mod.path = newMod.second;
        m_mods.push_back(mod);
    }

    SaveMods();

    // Load the metadata for each mod
    for (auto& mod : m_mods)
    {
        MountMod(mod, "/temp/mod");
        LoadModData(mod);
        UnmountMod(mod);
    }

    UpdatePaths();
}

void CModManager::ReloadMods()
{
    UnmountAllMods();
    m_mods.clear();

    Init();

    // Apply the configuration
    ReloadResources();
}

void CModManager::EnableMod(size_t i)
{
    m_mods[i].enabled = true;
}

void CModManager::DisableMod(size_t i)
{
    m_mods[i].enabled = false;
}

size_t CModManager::MoveUp(size_t i)
{
    if (i != 0)
    {
        std::swap(m_mods[i - 1], m_mods[i]);
        return i - 1;
    }
    else
    {
        return i;
    }
}

size_t CModManager::MoveDown(size_t i)
{
    if (i != m_mods.size() - 1)
    {
        std::swap(m_mods[i], m_mods[i + 1]);
        return i + 1;
    }
    else
    {
        return i;
    }
}

void CModManager::UpdatePaths()
{
    for (const auto& mod : m_mods)
    {
        if (mod.enabled)
        {
            MountMod(mod);
        }
    }
}

void CModManager::ReloadResources()
{
    m_app->ReloadResources();
}

void CModManager::SaveMods()
{
    std::vector<std::string> savedNames;
    savedNames.reserve(m_mods.size());
    std::transform(m_mods.begin(), m_mods.end(), std::back_inserter(savedNames), [](const Mod& mod) { return mod.name; });
    GetConfigFile().SetArrayProperty("Mods", "Names", savedNames);

    std::vector<bool> savedEnabled;
    savedEnabled.reserve(m_mods.size());
    std::transform(m_mods.begin(), m_mods.end(), std::back_inserter(savedEnabled), [](const Mod& mod) { return mod.enabled; });
    GetConfigFile().SetArrayProperty("Mods", "Enabled", savedEnabled);

    GetConfigFile().Save();
}

size_t CModManager::CountMods() const
{
    return m_mods.size();
}

const Mod& CModManager::GetMod(size_t i) const
{
    return m_mods[i];
}

const std::vector<Mod>& CModManager::GetMods() const
{
    return m_mods;
}

void CModManager::LoadModData(Mod& mod)
{
    auto& data = mod.data;

    data.displayName = mod.name;

    try
    {
        CLevelParser levelParser("temp/mod/manifest.txt");
        if (levelParser.Exists())
        {
            levelParser.Load();

            CLevelParserLine* line = nullptr;

            // DisplayName
            line = levelParser.GetIfDefined("DisplayName");
            if (line != nullptr && line->GetParam("text")->IsDefined())
            {
                data.displayName = line->GetParam("text")->AsString();
            }

            // Author
            line = levelParser.GetIfDefined("Author");
            if (line != nullptr && line->GetParam("text")->IsDefined())
            {
                data.author = line->GetParam("text")->AsString();
            }

            // Version
            line = levelParser.GetIfDefined("Version");
            if (line != nullptr)
            {
                if (line->GetParam("text")->IsDefined())
                {
                    data.version = line->GetParam("text")->AsString();
                }
                else if (line->GetParam("major")->IsDefined() && line->GetParam("minor")->IsDefined() &&  line->GetParam("patch")->IsDefined())
                {
                    auto major = boost::lexical_cast<std::string>(line->GetParam("major")->AsInt());
                    auto minor = boost::lexical_cast<std::string>(line->GetParam("minor")->AsInt());
                    auto patch = boost::lexical_cast<std::string>(line->GetParam("patch")->AsInt());
                    data.version = boost::algorithm::join(std::vector<std::string>{ major, minor, patch }, ",");
                }
            }

            // Website
            line = levelParser.GetIfDefined("Website");
            if (line != nullptr && line->GetParam("text")->IsDefined())
            {
                data.website = line->GetParam("text")->AsString();
            }

            // Summary
            line = levelParser.GetIfDefined("Summary");
            if (line != nullptr && line->GetParam("text")->IsDefined())
            {
                data.summary = line->GetParam("text")->AsString();
            }
        }
        else
        {
            GetLogger()->Warn("No manifest file for mod %s\n", mod.name.c_str());
        }
    }
    catch (CLevelParserException& e)
    {
        GetLogger()->Warn("Failed parsing manifest for mod %s: %s\n", mod.name.c_str(), e.what());
    }

    // Changes
    data.changes = CResourceManager::ListDirectories("temp/mod");
}

void CModManager::MountMod(const Mod& mod, const std::string& mountPoint)
{
    GetLogger()->Debug("Mounting mod: '%s' at path %s\n", mod.path.c_str(), mountPoint.c_str());
    CResourceManager::AddLocation(mod.path, true, mountPoint);
}

void CModManager::UnmountMod(const Mod& mod)
{
    if (CResourceManager::LocationExists(mod.path))
    {
        GetLogger()->Debug("Unmounting mod: '%s'\n", mod.path.c_str());
        CResourceManager::RemoveLocation(mod.path);
    }
}

void CModManager::UnmountAllMods()
{
    for (const auto& mod : m_mods)
    {
        UnmountMod(mod);
    }
}
