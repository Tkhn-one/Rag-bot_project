#include "rag/infrastructure/adapters.hpp"
#include "local_file.hpp"

#include <miniz.h>
#include <pugixml.hpp>

#include <unordered_map>
#include <unordered_set>

namespace rag {
namespace {

constexpr std::size_t mebibyte = 1024 * 1024;
constexpr std::size_t maxArchiveBytes = 64 * mebibyte;
constexpr std::size_t maxUncompressedBytes = 128 * mebibyte;
constexpr std::size_t maxXmlPartBytes = 8 * mebibyte;
constexpr std::size_t maxXmlBytes = 32 * mebibyte;
constexpr std::size_t maxSections = 100'000;
constexpr std::size_t maxPartNameBytes = 512;
constexpr mz_uint maxEntries = 4096;
constexpr int maxXmlDepth = 64;

struct XmlName {
    std::string_view local;
    std::string_view ns;
};

XmlName expandedName(pugi::xml_node node, std::string_view qualified, bool attribute = false) {
    const auto colon = qualified.find(':');
    if (colon == qualified.npos && attribute) {
        return {qualified, {}};
    }
    const auto declaration = colon == qualified.npos ? std::string("xmlns")
                             : "xmlns:" + std::string(qualified.substr(0, colon));
    const auto local = colon == qualified.npos ? qualified : qualified.substr(colon + 1);
    for (auto scope = node; scope; scope = scope.parent()) {
        if (const auto ns = scope.attribute(declaration.c_str())) {
            return {local, ns.value()};
        }
    }
    return {local, {}};
}

bool isWordNamespace(std::string_view ns) {
    return ns == "http://schemas.openxmlformats.org/wordprocessingml/2006/main" ||
           ns == "http://purl.oclc.org/ooxml/wordprocessingml/main";
}

std::string_view wordName(pugi::xml_node node) {
    const auto name = expandedName(node, node.name());
    return isWordNamespace(name.ns) ? name.local : std::string_view{};
}

pugi::xml_node wordChild(pugi::xml_node parent, std::string_view name) {
    for (const auto child : parent.children()) {
        if (wordName(child) == name) {
            return child;
        }
    }
    return {};
}

std::string_view wordAttribute(pugi::xml_node node, std::string_view local) {
    for (const auto attribute : node.attributes()) {
        const auto name = expandedName(node, attribute.name(), true);
        if (name.local == local && isWordNamespace(name.ns)) {
            return attribute.value();
        }
    }
    return {};
}

bool packageName(pugi::xml_node node, std::string_view local, std::string_view part) {
    const auto name = expandedName(node, node.name());
    if (name.local != local) {
        return false;
    }
    constexpr std::string_view transitional = "http://schemas.openxmlformats.org/package/2006/";
    constexpr std::string_view strict = "http://purl.oclc.org/ooxml/package/";
    return (name.ns.starts_with(transitional) && name.ns.substr(transitional.size()) == part) ||
           (name.ns.starts_with(strict) && name.ns.substr(strict.size()) == part);
}

class XmlStructure final : public pugi::xml_tree_walker {
public:
    bool for_each(pugi::xml_node& node) override {
        if (depth() >= maxXmlDepth) {
            error = AppError::DocxLimitExceeded;
            return false;
        }
        return node.type() != pugi::node_doctype;
    }

    AppError error = AppError::DocxCorrupt;
};

class Package {
public:
    explicit Package(std::string_view bytes)
        : opened_(mz_zip_reader_init_mem(&archive_, bytes.data(), bytes.size(), 0) != 0) {}

    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    ~Package() {
        if (opened_) {
            mz_zip_reader_end(&archive_);
        }
    }

    std::optional<AppError> inspect() {
        if (!opened_) {
            return AppError::DocxCorrupt;
        }
        const auto count = mz_zip_reader_get_num_files(&archive_);
        if (count > maxEntries) {
            return AppError::DocxLimitExceeded;
        }

        mz_uint64 bytesLeft = maxUncompressedBytes;
        for (mz_uint index = 0; index < count; ++index) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive_, index, &stat)) {
                return AppError::DocxCorrupt;
            }
            if (stat.m_uncomp_size > bytesLeft) {
                return AppError::DocxLimitExceeded;
            }
            bytesLeft -= stat.m_uncomp_size;
            if (stat.m_is_directory) {
                continue;
            }

            const auto length = mz_zip_reader_get_filename(&archive_, index, nullptr, 0);
            if (length < 2) {
                return AppError::DocxCorrupt;
            }
            if (length > maxPartNameBytes + 1) {
                return AppError::DocxLimitExceeded;
            }
            std::string name(length, '\0');
            if (mz_zip_reader_get_filename(&archive_, index, name.data(), length) != length) {
                return AppError::DocxCorrupt;
            }
            name.pop_back();
            if (name.find('\0') != name.npos ||
                !entries_.emplace(std::move(name), Entry{index, stat.m_uncomp_size}).second) {
                return AppError::DocxCorrupt;
            }
        }
        return std::nullopt;
    }

    bool contains(const std::string& name) const { return entries_.contains(name); }

    std::optional<AppError> readXml(const std::string& name, pugi::xml_document& xml) {
        const auto entry = entries_.find(name);
        if (entry == entries_.end() || entry->second.bytes == 0) {
            return AppError::DocxCorrupt;
        }
        if (entry->second.bytes > maxXmlPartBytes || entry->second.bytes > xmlBytesLeft_) {
            return AppError::DocxLimitExceeded;
        }
        std::vector<char> buffer(static_cast<std::size_t>(entry->second.bytes));
        if (!mz_zip_reader_extract_to_mem(&archive_, entry->second.index, buffer.data(), buffer.size(), 0) ||
            !xml.load_buffer(buffer.data(), buffer.size(),
                             pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_doctype) ||
            !xml.document_element()) {
            return AppError::DocxCorrupt;
        }
        xmlBytesLeft_ -= buffer.size();
        const auto root = xml.document_element();
        for (auto sibling = root.next_sibling(); sibling; sibling = sibling.next_sibling()) {
            if (sibling.type() == pugi::node_element) {
                return AppError::DocxCorrupt;
            }
        }
        XmlStructure limits;
        if (!xml.traverse(limits)) {
            return limits.error;
        }
        return std::nullopt;
    }

private:
    struct Entry {
        mz_uint index;
        mz_uint64 bytes;
    };

    mz_zip_archive archive_{};
    const bool opened_;
    std::unordered_map<std::string, Entry> entries_;
    std::size_t xmlBytesLeft_ = maxXmlBytes;
};

std::optional<std::string> resolvePart(std::string_view source, std::string_view target) {
    if (target.empty() || target.find_first_of("\\:?#") != target.npos ||
        std::ranges::any_of(target, [](unsigned char ch) { return ch < 32 || ch == 127; })) {
        return std::nullopt;
    }
    std::string_view parent;
    if (target.starts_with('/')) {
        target.remove_prefix(1);
    } else if (const auto slash = source.rfind('/'); slash != source.npos) {
        parent = source.substr(0, slash + 1);
    }
    const auto path = std::string(parent) + std::string(target);
    std::string resolved;
    std::string_view remaining(path);
    while (true) {
        const auto separator = remaining.find('/');
        const auto segment = remaining.substr(0, separator);
        if (segment.empty()) {
            return std::nullopt;
        }
        if (segment == "..") {
            if (resolved.empty()) {
                return std::nullopt;
            }
            const auto slash = resolved.rfind('/');
            resolved.resize(slash == resolved.npos ? 0 : slash);
        } else if (segment != ".") {
            if (!resolved.empty()) {
                resolved += '/';
            }
            resolved += segment;
        }
        if (separator == remaining.npos) {
            break;
        }
        remaining.remove_prefix(separator + 1);
    }
    if (resolved.empty() || resolved.size() > maxPartNameBytes) {
        return std::nullopt;
    }
    return resolved;
}

std::string relationshipsPath(std::string_view part) {
    const auto slash = part.rfind('/');
    const auto start = slash == part.npos ? 0 : slash + 1;
    return std::string(part.substr(0, start)) + "_rels/" + std::string(part.substr(start)) + ".rels";
}

std::string_view relationshipKind(pugi::xml_node relation) {
    const std::string_view type = relation.attribute("Type").value();
    for (const std::string_view base : {
             "http://schemas.openxmlformats.org/officeDocument/2006/relationships/",
             "http://purl.oclc.org/ooxml/officeDocument/relationships/"}) {
        if (type.starts_with(base)) {
            return type.substr(base.size());
        }
    }
    return {};
}

bool external(pugi::xml_node relation) {
    return std::string_view(relation.attribute("TargetMode").value()) == "External";
}

bool unsupportedElement(pugi::xml_node node, std::string_view word) {
    if (word == "drawing" || word == "pict" || word == "object" || word == "altChunk" ||
        word == "subDoc" || word == "txbxContent") {
        return true;
    }
    const auto name = expandedName(node, node.name());
    return name.local == "AlternateContent" &&
           name.ns == "http://schemas.openxmlformats.org/markup-compatibility/2006";
}

template <typename Callback>
void visibleChildren(pugi::xml_node parent, const Callback& visit) {
    for (const auto child : parent.children()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        const auto name = wordName(child);
        if (name == "sdt" || name == "sdtContent" || name == "customXml" ||
            name == "ins" || name == "moveTo") {
            visibleChildren(child, visit);
        } else if (name != "del" && name != "moveFrom") {
            visit(child, name);
        }
    }
}

class PartText {
public:
    PartText(const std::string& name, ExtractedDocument& result) : name_(name), result_(result) {}

    void blocks(pugi::xml_node container, std::optional<TableCell> cell = {}) {
        visibleChildren(container, [&](pugi::xml_node child, std::string_view name) {
            if (name == "p") {
                if (result_.sections.size() >= maxSections) {
                    exceeded_ = true;
                    return;
                }
                auto position = cell ? DocxPosition{name_, *cell} : DocxPosition{name_, ++paragraph_};
                std::string text;
                inlineText(child, text);
                if (text.find_first_not_of(" \t\r\n\f\v") != text.npos) {
                    result_.sections.push_back({std::move(text), {}, std::move(position)});
                }
            } else if (name == "tbl") {
                table(child);
            } else if (unsupportedElement(child, name)) {
                result_.incomplete = true;
            }
        });
    }

    bool exceeded() const { return exceeded_; }

private:
    void table(pugi::xml_node node) {
        const auto tableIndex = ++table_;
        std::size_t row = 0;
        visibleChildren(node, [&](pugi::xml_node rowNode, std::string_view rowName) {
            if (rowName != "tr" || wordChild(wordChild(rowNode, "trPr"), "del")) {
                return;
            }
            ++row;
            std::size_t cell = 0;
            visibleChildren(rowNode, [&](pugi::xml_node cellNode, std::string_view cellName) {
                if (cellName == "tc" && !wordChild(wordChild(cellNode, "tcPr"), "cellDel")) {
                    blocks(cellNode, TableCell{tableIndex, row, ++cell});
                }
            });
        });
    }

    void inlineText(pugi::xml_node node, std::string& text) {
        visibleChildren(node, [&](pugi::xml_node child, std::string_view name) {
            if (name == "t") {
                text += child.child_value();
            } else if (name == "tab" || name == "ptab") {
                text += '\t';
            } else if (name == "br" || name == "cr") {
                text += '\n';
            } else if (name == "noBreakHyphen") {
                text += "\xe2\x80\x91";
            } else if (name == "r" || name == "hyperlink" || name == "smartTag" || name == "fldSimple" ||
                       name == "dir" || name == "bdo") {
                inlineText(child, text);
            } else if (unsupportedElement(child, name)) {
                result_.incomplete = true;
            }
        });
    }

    const std::string& name_;
    ExtractedDocument& result_;
    std::size_t paragraph_ = 0;
    std::size_t table_ = 0;
    bool exceeded_ = false;
};

Result<std::string> mainDocument(Package& package) {
    pugi::xml_document relationships;
    if (const auto error = package.readXml("_rels/.rels", relationships)) {
        return *error;
    }
    const auto root = relationships.document_element();
    if (!packageName(root, "Relationships", "relationships")) {
        return AppError::DocxCorrupt;
    }
    std::optional<std::string> main;
    for (const auto relation : root.children()) {
        if (packageName(relation, "Relationship", "relationships") &&
            relationshipKind(relation) == "officeDocument") {
            if (main || external(relation)) {
                return AppError::DocxCorrupt;
            }
            main = resolvePart({}, relation.attribute("Target").value());
            if (!main) {
                return AppError::DocxCorrupt;
            }
        }
    }
    if (!main) {
        return AppError::DocxCorrupt;
    }
    return std::move(*main);
}

std::optional<AppError> collectPart(Package& package, const std::string& part, std::string_view kind,
                                   ExtractedDocument& result) {
    pugi::xml_document xml;
    if (const auto error = package.readXml(part, xml)) {
        return error;
    }
    const auto root = xml.document_element();
    if (wordName(root) != kind) {
        return AppError::DocxCorrupt;
    }
    PartText collector(part, result);
    if (kind == "document") {
        const auto body = wordChild(root, "body");
        if (!body) {
            return AppError::DocxCorrupt;
        }
        collector.blocks(body);
    } else if (kind == "footnotes") {
        for (const auto note : root.children()) {
            const auto type = wordAttribute(note, "type");
            if (wordName(note) == "footnote" && (type.empty() || type == "normal")) {
                collector.blocks(note);
            }
        }
    } else {
        collector.blocks(root);
    }
    return collector.exceeded() ? std::optional{AppError::DocxLimitExceeded} : std::nullopt;
}

std::optional<AppError> collectRelatedParts(Package& package, const std::string& main,
                                           ExtractedDocument& result) {
    const auto relationshipsName = relationshipsPath(main);
    if (!package.contains(relationshipsName)) {
        return std::nullopt;
    }
    pugi::xml_document relationships;
    if (const auto error = package.readXml(relationshipsName, relationships)) {
        return error;
    }
    const auto root = relationships.document_element();
    if (!packageName(root, "Relationships", "relationships")) {
        return AppError::DocxCorrupt;
    }
    std::unordered_set<std::string> visited;
    for (const auto relation : root.children()) {
        if (!packageName(relation, "Relationship", "relationships")) {
            continue;
        }
        const auto kind = relationshipKind(relation);
        const auto rootName = kind == "header" ? "hdr" : kind == "footer" ? "ftr" :
                              kind == "footnotes" ? "footnotes" : "";
        if (*rootName == '\0') {
            if (kind == "endnotes" || kind == "aFChunk" || kind == "oleObject" || kind == "package") {
                result.incomplete = true;
            }
            continue;
        }
        if (external(relation)) {
            result.incomplete = true;
            continue;
        }
        const auto part = resolvePart(main, relation.attribute("Target").value());
        if (!part) {
            return AppError::DocxCorrupt;
        }
        if (visited.insert(*part).second) {
            if (const auto error = collectPart(package, *part, rootName, result)) {
                return error;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

Result<ExtractedDocument> DocxReader::read(const std::filesystem::path& path) const {
    if (!hasExtension(path, L".docx")) {
        return AppError::UnsupportedFormat;
    }
    const auto file = readLocalFile(path, maxArchiveBytes);
    if (const auto* error = std::get_if<AppError>(&file)) {
        return *error;
    }
    Package package(std::get<std::string>(file));
    if (const auto error = package.inspect()) {
        return *error;
    }
    pugi::xml_document contentTypes;
    if (const auto error = package.readXml("[Content_Types].xml", contentTypes)) {
        return *error;
    }
    if (!packageName(contentTypes.document_element(), "Types", "content-types")) {
        return AppError::DocxCorrupt;
    }

    const auto main = mainDocument(package);
    if (const auto* error = std::get_if<AppError>(&main)) {
        return *error;
    }
    const auto& part = std::get<std::string>(main);
    ExtractedDocument result;
    if (const auto error = collectPart(package, part, "document", result)) {
        return *error;
    }
    if (const auto error = collectRelatedParts(package, part, result)) {
        return *error;
    }
    if (result.sections.empty()) {
        return AppError::DocxNoText;
    }
    return result;
}

}  // namespace rag
