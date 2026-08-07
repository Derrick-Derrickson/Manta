// Loading and addressing source text.
//
// Spec 1.4: files are UTF-8, a BOM is permitted and ignored, line endings may be
// LF or CRLF. The BOM is stripped at load so that every offset the rest of the
// compiler sees indexes the real text; CRLF is *kept* in the buffer, because the
// annotator rewrites source in place by byte range (spec 13.7) and must not
// disturb the line endings of lines it does not touch. Only the formatter
// normalises to LF, and it does so by rewriting the whole file.
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "source/span.h"

namespace manta {

class SourceFile {
public:
    SourceFile(FileId id, std::string path, std::string text);

    [[nodiscard]] FileId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] std::size_t size() const noexcept { return text_.size(); }

    // True if the original bytes began with a UTF-8 BOM (stripped from text()).
    [[nodiscard]] bool hadBom() const noexcept { return hadBom_; }
    [[nodiscard]] bool hasCrlf() const noexcept { return hasCrlf_; }

    [[nodiscard]] LineCol lineCol(std::uint32_t offset) const;

    // The line containing an offset, without its terminator. Used for the caret
    // display in text diagnostics.
    [[nodiscard]] std::string_view lineText(std::uint32_t line) const;
    [[nodiscard]] std::uint32_t lineCount() const noexcept {
        return static_cast<std::uint32_t>(lineStarts_.size());
    }

private:
    FileId id_;
    std::string path_;
    std::string text_;
    std::vector<std::uint32_t> lineStarts_;
    bool hadBom_ = false;
    bool hasCrlf_ = false;
};

struct IoError {
    std::string path;
    std::string message;
};

class SourceManager {
public:
    // Reads a file from disk. Binary mode: no platform newline translation, so
    // Linux and Windows see identical bytes (spec 15.8).
    std::expected<const SourceFile*, IoError> load(const std::string& path);

    // Registers text that did not come from a file (tests, stdin).
    const SourceFile* addVirtual(std::string name, std::string text);

    [[nodiscard]] const SourceFile* get(FileId id) const {
        return id < files_.size() ? files_[id].get() : nullptr;
    }

    [[nodiscard]] std::size_t count() const noexcept { return files_.size(); }

    // Convenience for diagnostic rendering.
    [[nodiscard]] LineCol lineCol(Span s) const;
    [[nodiscard]] std::string_view pathOf(FileId id) const;

private:
    std::vector<std::unique_ptr<SourceFile>> files_;
};

}  // namespace manta
