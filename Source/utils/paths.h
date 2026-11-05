#pragma once

#include <string>

namespace devilution {

namespace paths {

const std::string &BasePath();
const std::string &PrefPath();
const std::string &ConfigPath();
const std::string &AssetsPath();

/** The base path SDL reports (the executable's folder, or an app bundle's resources), even after SetBasePath chose another. */
const std::string &DefaultBasePath();

void SetBasePath(const std::string &path);
void SetPrefPath(const std::string &path);
void SetConfigPath(const std::string &path);
void SetAssetsPath(const std::string &path);

} // namespace paths

} // namespace devilution
