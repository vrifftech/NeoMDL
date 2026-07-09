#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <cstdio>
#include <cstdlib>

namespace neomdl::engine2da {
namespace {
int columnIndex(const neo2da::TwoDAFile& table,std::string_view name) {
    const auto lower=[](unsigned char c){return c>='A'&&c<='Z'?c+('a'-'A'):c;};
    for(std::size_t i=0;i<table.columnCount();++i) {
        const auto& label=table.columnLabel(i);
        if(label.size()!=name.size())continue;
        std::size_t c=0;
        while(c<label.size()&&lower(label[c])==lower(name[c]))++c;
        if(c==label.size())return static_cast<int>(i);
    }
    return -1;
}
const std::string* source(const neo2da::TwoDAFile& table,int row,int column) {
    if(row<0||column<0||static_cast<std::size_t>(row)>=table.rowCount()||
       static_cast<std::size_t>(column)>=table.columnCount())return nullptr;
    return &table.sourceCell(static_cast<std::size_t>(row),static_cast<std::size_t>(column));
}
}
bool getInt(const neo2da::TwoDAFile& table,int row,int column,int& value) {
    const auto* cell=source(table,row,column);
    if(!cell){value=std::atoi(table.defaultValue().c_str());return false;}
    if(table.nativeFormatName()=="Binary 2DA V2.b") {
        value=0;std::sscanf(cell->c_str(),"%i",&value);
    } else if(cell->size()>=3&&(*cell)[0]=='0'&&((*cell)[1]=='x'||(*cell)[1]=='X')) {
        // The text hexadecimal branch does not initialize the destination.
        // An unsuccessful conversion retains the caller's incoming value.
        unsigned parsed=static_cast<unsigned>(value);
        std::sscanf(cell->c_str(),"%x",&parsed);value=static_cast<int>(parsed);
    } else value=std::atoi(cell->c_str());
    return !cell->empty();
}
bool getInt(const neo2da::TwoDAFile& table,int row,std::string_view column,int& value) {
    return getInt(table,row,columnIndex(table,column),value);
}
bool getFloat(const neo2da::TwoDAFile& table,int row,int column,float& value) {
    const auto* cell=source(table,row,column);
    if(!cell){value=static_cast<float>(std::atof(table.defaultValue().c_str()));return false;}
    if(table.nativeFormatName()=="Binary 2DA V2.b") {
        value=0.0f;std::sscanf(cell->c_str(),"%f",&value);
    } else value=static_cast<float>(std::atof(cell->c_str()));
    return !cell->empty();
}
bool getFloat(const neo2da::TwoDAFile& table,int row,std::string_view column,float& value) {
    return getFloat(table,row,columnIndex(table,column),value);
}
bool getString(const neo2da::TwoDAFile& table,int row,int column,std::string& value) {
    const auto* cell=source(table,row,column);
    value=cell?*cell:table.defaultValue();
    return cell&&!cell->empty();
}
bool getString(const neo2da::TwoDAFile& table,int row,std::string_view column,std::string& value) {
    return getString(table,row,columnIndex(table,column),value);
}
} // namespace neomdl::engine2da
