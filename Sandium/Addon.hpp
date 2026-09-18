#pragma once

#include <memory>
#include <sol/sol.hpp>
#include <string>
#include <vector>

#include "api/Logging.hpp"

class LuaManager;

class Addon
{
public:
    Addon() = delete;
    Addon(const std::string &folderPath, int asType = -1);
    ~Addon() noexcept;

private:
    std::string _folderPath;
    int _asType;

public:
    bool IsLoaded() const;
    void Load();
    void Unload();
private:
    void LoadAsSR();
    void LoadAsSandium();

    bool _isLoaded;

    std::string _id;
public:
    const std::string &ID() const;

    std::string _name;
    std::string _description;
    std::string _logDecoration;
public:
    const std::string &GetName() const;
    const std::string &GetDescription() const;
    const std::string &GetLogDecoration() const;
    const std::string &GetFolderPath() const;

private:
    std::vector<std::string> _requires;
    std::vector<std::string> _conflicts;
    bool _disabled = false;
    bool _builtin = false;
public:
    bool CheckDependencies();
    bool IsDisabled() const;
    void SetDisabled(bool disabled); // persists to addon.json
    bool IsBuiltin() const;
    void SetBuiltin(bool builtin = true); // built-ins (sub_rosa, sandium) cannot be disabled

private:
    std::unique_ptr<api::Logger> _logger;
public:
    api::Logger *GetLogger() const;

private:
    std::unique_ptr<sol::thread> _addonThread;
public:
    bool PrepareLua(LuaManager *manager, bool ignoreClient);
};

void DiscoverAddons();
const std::vector<std::unique_ptr<Addon>> &GetAddons();
void RegisterAddon(std::unique_ptr<Addon> addon);
Addon *FindAddon(const std::string &addonID);
