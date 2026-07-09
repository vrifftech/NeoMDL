#pragma once

#include <string>
#include <string_view>

namespace neo2da { class TwoDAFile; }

namespace neomdl::engine2da {
// Engine C2DA readers mutate output independently of their success result.
// A caller that checks success before assigning a field must keep doing so.
// Format-level empty/default provenance belongs to TwoDAFile::sourceCell.
bool getInt(const neo2da::TwoDAFile&,int row,int column,int& value);
bool getInt(const neo2da::TwoDAFile&,int row,std::string_view column,int& value);
bool getFloat(const neo2da::TwoDAFile&,int row,int column,float& value);
bool getFloat(const neo2da::TwoDAFile&,int row,std::string_view column,float& value);
bool getString(const neo2da::TwoDAFile&,int row,int column,std::string& value);
bool getString(const neo2da::TwoDAFile&,int row,std::string_view column,std::string& value);
} // namespace neomdl::engine2da
