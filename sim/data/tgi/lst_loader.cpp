#include "data/tgi/lst_loader.hpp"

#include <algorithm>
#include <set>

#include "core/ascii.hpp"
#include "data/tgi/text_decode.hpp"
#include "vfs/path.hpp"

namespace corps::tgi {
namespace {

class LstLoader {
public:
    LstLoader(const vfs::Vfs& vfs, Diagnostics& diags) : vfs(vfs), diags(diags) {}

    LoadOrder run() {
        std::vector<const vfs::VfsFile*> roots = vfs.glob("index*.lst");
        sortWildcardMatches(roots);
        for (const vfs::VfsFile* root : roots) processList(*root);
        return std::move(order);
    }

private:
    void processList(const vfs::VfsFile& lst) {
        for (const std::string& open : stack) {
            if (open == lst.key) {
                diags.report(DiagCode::LstRecursion, lst.path, 0, "list includes itself (directly or indirectly); skipped");
                return;
            }
        }
        std::string bytes;
        if (!vfs.read(lst, bytes)) {
            diags.report(DiagCode::LstUnreadable, lst.path, 0, "cannot read list file");
            return;
        }
        stack.push_back(lst.key);
        order.lists.push_back(lst.path);

        const std::string text = decodeText(bytes).utf8;
        std::uint32_t lineNo = 0;
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find_first_of("\r\n", start);
            if (end == std::string::npos) end = text.size();
            ++lineNo;
            std::string_view line = std::string_view(text).substr(start, end - start);
            if (const std::size_t semi = line.find(';'); semi != std::string_view::npos) line = line.substr(0, semi);
            line = trimAscii(line);
            if (!line.empty()) processSpec(lst, lineNo, line);
            if (end == text.size()) break;
            start = end + ((text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ? 2 : 1);
        }
        stack.pop_back();
    }

    void processSpec(const vfs::VfsFile& lst, std::uint32_t lineNo, std::string_view spec) {
        const std::string norm = vfs::normalizePath(spec);
        const bool wildcard = norm.find('*') != std::string::npos;
        std::vector<const vfs::VfsFile*> matches = vfs.glob(norm);
        sortWildcardMatches(matches);
        if (matches.empty()) {
            if (wildcard) {
                order.emptyWildcards.push_back(lst.path + ":" + std::to_string(lineNo) + " " + norm);
            } else {
                diags.report(DiagCode::LstMissingFile, lst.path, lineNo, "listed file not found: " + norm);
            }
            return;
        }
        for (const vfs::VfsFile* m : matches) {
            if (iendsWith(m->key, ".lst")) {
                processList(*m);  // nested list is processed in place
                continue;
            }
            if (!loadedKeys.insert(m->key).second) {
                diags.report(DiagCode::LstDuplicate, lst.path, lineNo, "already loaded, skipped: " + m->path);
                continue;
            }
            order.files.push_back(LoadEntry{m, lst.path, lineNo});
        }
    }

    const vfs::Vfs& vfs;
    Diagnostics& diags;
    LoadOrder order;
    std::vector<std::string> stack;
    std::set<std::string> loadedKeys;
};

}  // namespace

void sortWildcardMatches(std::vector<const vfs::VfsFile*>& matches) {
    std::stable_sort(matches.begin(), matches.end(), [](const vfs::VfsFile* a, const vfs::VfsFile* b) {
        return vfs::upperOrdinalLess(a->path, b->path);
    });
}

LoadOrder buildLoadOrder(const vfs::Vfs& vfs, Diagnostics& diags) {
    LstLoader loader(vfs, diags);
    return loader.run();
}

}  // namespace corps::tgi
