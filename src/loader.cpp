// unit 加载
#include "loader.hpp"

#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "common.hpp"
#include "parser.hpp"

namespace {

std::string dirname_of(const std::string& p) {
    auto pos = p.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : p.substr(0, pos);
}

bool file_exists(const std::string& p) {
    std::ifstream f(p);
    return static_cast<bool>(f);
}

}  // namespace

std::vector<std::unique_ptr<Unit>> load_units(const std::string& entry_path,
                                              const std::vector<std::string>& search_dirs,
                                              std::string& entry_name) {
    std::vector<std::string> dirs{dirname_of(entry_path)};
    for (auto& d : search_dirs) dirs.push_back(d);

    std::map<std::string, std::unique_ptr<Unit>> owned;
    std::set<std::string> in_progress;
    std::vector<std::string> post_order;

    std::function<std::string(const std::string&)> find_file = [&](const std::string& un) {
        for (auto& d : dirs) {
            std::string p = d + "/" + un + ".mu";
            if (file_exists(p)) return p;
        }
        throw MuError("找不到 unit `" + un + "` 对应的 " + un + ".mu");
    };

    std::function<Unit*(const std::string&)> visit = [&](const std::string& path) -> Unit* {
        auto unit = parse_file(path);
        std::string name = unit->name;
        auto it = owned.find(name);
        if (it != owned.end()) return it->second.get();
        if (in_progress.count(name)) {
            throw MuError("unit 循环依赖：" + name);
        }
        in_progress.insert(name);
        Unit* raw = unit.get();
        owned[name] = std::move(unit);
        for (auto& use : raw->uses) {
            Unit* dep = visit(find_file(use->unit_name));
            if (dep->name != use->unit_name) {
                throw MuError(dep->filename + " 声明的 unit 是 `" + dep->name +
                                  "`，但被 `use " + use->unit_name + ";` 引用",
                              use->line, use->col, raw->filename);
            }
        }
        in_progress.erase(name);
        post_order.push_back(name);
        return raw;
    };

    Unit* entry_unit = visit(entry_path);
    entry_name = entry_unit->name;

    std::vector<std::unique_ptr<Unit>> result;
    result.reserve(post_order.size());
    for (auto& name : post_order) result.push_back(std::move(owned[name]));
    return result;
}
