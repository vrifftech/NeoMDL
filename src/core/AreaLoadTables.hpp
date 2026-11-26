#pragma once
#include "core/ResourceContext.hpp"
#include "core/Engine2DA.hpp"
#include <twoda/TwoDAFile.hpp>
#include <algorithm>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace neomdl::detail {
// Existing AreaLoad table reader, shared by object/creature visual mapping.
// Scoped loading belongs to the host loader.
// Engine column lookup and source-cell semantics belong to Engine2DA.
inline void areaTableRequire(bool value,const std::string& message) {
    if(!value)throw std::runtime_error("Area load: "+message);
}
struct AreaLoadTables {
    ResourceResolver resolver;
    std::map<std::string,neo2da::TwoDAFile> files;
    const neo2da::TwoDAFile& table(const std::string& file) {
        const auto& job=resourceLoadJob();job.check();
        auto it=files.find(file);
        if(it==files.end()) {
            const auto r=resolver(file,0x7e1);areaTableRequire(bool(r),"missing "+file+".2da");
            job.update(0,0,"Decoding "+file+".2da");
            neo2da::TwoDAFile t;
            try {t.loadBytes(r->bytes);}
            catch(const neo2da::TwoDAError& error) {
                std::ostringstream context;
                context<<file<<".2da from "<<r->sourceDescription<<"\nBytes: "<<r->bytes.size()<<"\nHeader:";
                for(std::size_t n=0;n<std::min<std::size_t>(32,r->bytes.size());++n)
                    context<<' '<<std::hex<<std::setfill('0')<<std::setw(2)<<unsigned(r->bytes[n]);
                throw neo2da::TwoDAError(context.str()+"\n"+error.what(),error.helpContext());
            }
            job.check();it=files.emplace(file,std::move(t)).first;
        }
        return it->second;
    }
    std::optional<std::string> lookup(const neo2da::TwoDAFile& t,std::uint32_t row,const std::string& column) {
        std::string value;
        if(!engine2da::getString(t,static_cast<int>(row),column,value))return {};
        return value;
    }
    // Appearance constructors and object model loaders consume the string
    // destination even when GetCExoStringEntry reports failure.
    std::string stringValue(const std::string& file,std::uint32_t row,const std::string& column) {
        std::string value;engine2da::getString(table(file),static_cast<int>(row),column,value);return value;
    }

    std::optional<std::string> lookup(const std::string& file,std::uint32_t row,const std::string& column) {
        return lookup(table(file),row,column);
    }
    std::string cell(const std::string& file,std::uint32_t row,const std::string& column,bool required=false) {
        const auto& t=table(file);
        const auto value=lookup(t,row,column).value_or(std::string{});
        areaTableRequire(!required||!value.empty(),"missing "+file+".2da "+column+" at row "+std::to_string(row));
        return value;
    }
};
} // namespace neomdl::detail
