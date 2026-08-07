#include "source/source_manager.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include "base/utf8.h"

namespace manta {

SourceFile::SourceFile(FileId id, std::string path, std::string text)
    : id_(id), path_(std::move(path)), text_(std::move(text)) {
    // Spec 1.4: a byte-order mark is permitted and ignored.
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB &&
        static_cast<unsigned char>(text_[2]) == 0xBF) {
        text_.erase(0, 3);
        hadBom_ = true;
    }

    lineStarts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\r') hasCrlf_ = true;
        if (text_[i] == '\n') lineStarts_.push_back(static_cast<std::uint32_t>(i + 1));
    }
    // A trailing newline produces a final empty line start; drop it so that
    // lineCount() matches what an editor shows.
    if (lineStarts_.size() > 1 && lineStarts_.back() == text_.size()) lineStarts_.pop_back();
}

LineCol SourceFile::lineCol(std::uint32_t offset) const {
    offset = std::min<std::uint32_t>(offset, static_cast<std::uint32_t>(text_.size()));
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    auto line = static_cast<std::uint32_t>(it - lineStarts_.begin());  // 1-based
    std::uint32_t start = lineStarts_[line - 1];

    // Columns count codepoints, not bytes: a comment or string containing
    // multi-byte UTF-8 (spec 2.1 permits it) must not skew the column of the
    // tokens after it.
    std::uint32_t col = 1;
    for (std::uint32_t i = start; i < offset;) {
        i += static_cast<std::uint32_t>(utf8SequenceLength(static_cast<unsigned char>(text_[i])));
        ++col;
    }
    return LineCol{line, col};
}

std::string_view SourceFile::lineText(std::uint32_t line) const {
    if (line == 0 || line > lineStarts_.size()) return {};
    std::uint32_t start = lineStarts_[line - 1];
    std::uint32_t stop = line < lineStarts_.size() ? lineStarts_[line]
                                                   : static_cast<std::uint32_t>(text_.size());
    std::string_view sv(text_.data() + start, stop - start);
    while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) sv.remove_suffix(1);
    return sv;
}

std::expected<const SourceFile*, IoError> SourceManager::load(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::unexpected(IoError{path, "cannot open file"});

    std::string text;
    char buf[64 * 1024];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return std::unexpected(IoError{path, "read error"});

    auto id = static_cast<FileId>(files_.size());
    files_.push_back(std::make_unique<SourceFile>(id, path, std::move(text)));
    return files_.back().get();
}

const SourceFile* SourceManager::addVirtual(std::string name, std::string text) {
    auto id = static_cast<FileId>(files_.size());
    files_.push_back(std::make_unique<SourceFile>(id, std::move(name), std::move(text)));
    return files_.back().get();
}

LineCol SourceManager::lineCol(Span s) const {
    const SourceFile* f = get(s.file);
    return f ? f->lineCol(s.offset) : LineCol{};
}

std::string_view SourceManager::pathOf(FileId id) const {
    const SourceFile* f = get(id);
    return f ? std::string_view(f->path()) : std::string_view{"<unknown>"};
}

}  // namespace manta
