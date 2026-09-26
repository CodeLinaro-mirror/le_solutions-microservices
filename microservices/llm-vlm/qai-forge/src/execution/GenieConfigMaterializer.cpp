// Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "qai_forge/execution/GenieConfigMaterializer.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::ordered_json;

namespace qai_forge {
namespace execution {
namespace {

bool isSafeRelativePath(const fs::path& path) {
    const fs::path normalized = path.lexically_normal();
    if (normalized.empty() || normalized.is_absolute() ||
        normalized.has_root_name()) {
        return false;
    }

    const auto first = normalized.begin();
    return first == normalized.end() || *first != "..";
}

fs::path canonicalExisting(const fs::path& path, const std::string& label) {
    std::error_code ec;
    const fs::path result = fs::weakly_canonical(path, ec);
    if (ec || !fs::exists(result)) {
        throw std::runtime_error(
            "GenIE materialization: missing " + label + ": " + path.string());
    }
    return result;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool isJsonPath(const fs::path& path) {
    return lower(path.extension().string()) == ".json";
}

bool isExtensionKey(const std::string& key) {
    const std::string normalized = lower(key);
    return normalized == "extensions" || normalized == "extension";
}

bool isBackendKey(const std::string& key) {
    return lower(key) == "backend";
}

// tokenizer/vocab assets are never copied - the source model bundle path is
// used directly. Any ".json" reference with one of these filenames must be
// treated as an opaque asset path, not a nested config to materialize.
bool isTokenizerAssetFilename(const fs::path& path) {
    const std::string filename = lower(path.filename().string());
    return filename == "tokenizer.json" || filename == "vocab.json";
}

std::string sanitizePathComponent(std::string value) {
    for (char& c : value) {
        const bool valid =
            std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
            c == '_' || c == '.';
        if (!valid) c = '_';
    }
    if (value.empty()) value = "runtime";
    return value;
}

std::vector<std::string> stringLeaves(const json& value) {
    std::vector<std::string> result;
    if (value.is_string()) {
        result.push_back(value.get<std::string>());
    } else if (value.is_array()) {
        for (const auto& item : value) {
            auto leaves = stringLeaves(item);
            result.insert(result.end(), leaves.begin(), leaves.end());
        }
    } else if (value.is_object()) {
        for (const auto& [key, item] : value.items()) {
            (void)key;
            auto leaves = stringLeaves(item);
            result.insert(result.end(), leaves.begin(), leaves.end());
        }
    }
    return result;
}

class MaterializationContext {
public:
    MaterializationContext(fs::path source_root,
                           fs::path staging_root,
                           fs::path published_root,
                           int selected_device_id)
        : source_root_(std::move(source_root)),
          staging_root_(std::move(staging_root)),
          published_root_(std::move(published_root)),
          selected_device_id_(selected_device_id) {}

    fs::path materializeJson(const fs::path& source_path) {
        const fs::path source = canonicalExisting(source_path, "JSON reference");
        ensureInsideSourceRoot(source);

        if (active_.count(source.string()) != 0) {
            throw std::runtime_error(
                "GenIE materialization: cyclic JSON reference: " +
                source.string());
        }

        auto existing = destinations_.find(source.string());
        if (existing != destinations_.end()) return existing->second;

        const fs::path relative =
            fs::relative(source, source_root_).lexically_normal();
        if (!isSafeRelativePath(relative)) {
            throw std::runtime_error(
                "GenIE materialization: unsafe JSON relative path: " +
                source.string());
        }

        const fs::path destination = staging_root_ / relative;
        auto owner = destination_sources_.find(destination.string());
        if (owner != destination_sources_.end() &&
            owner->second != source.string()) {
            throw std::runtime_error(
                "GenIE materialization: destination collision between '" +
                owner->second + "' and '" + source.string() + "'");
        }

        destinations_[source.string()] = destination;
        destination_sources_[destination.string()] = source.string();

        json data;
        try {
            std::ifstream input(source);
            if (!input) {
                throw std::runtime_error("cannot open file");
            }
            input >> data;
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "GenIE materialization: failed to parse '" + source.string() +
                "': " + error.what());
        }

        active_.insert(source.string());

        const bool is_root = source == root_source_;
        rewriteJson(data, source.parent_path(), destination.parent_path(), is_root);

        active_.erase(source.string());

        fs::create_directories(destination.parent_path());
        writeJsonAtomically(data, destination);
        return destination;
    }

    void setRootSource(fs::path root_source) {
        root_source_ = canonicalExisting(root_source, "root config");
    }

    fs::path rootSource() const { return root_source_; }

    fs::path resolveJsonReference(const std::string& value,
                                  const fs::path& current_dir) const {
        fs::path reference(value);
        fs::path resolved;

        if (reference.is_absolute() || reference.has_root_name()) {
            resolved = reference;
        } else {
            resolved = current_dir / reference;
        }

        resolved = canonicalExisting(resolved, "JSON reference");
        ensureInsideSourceRoot(resolved);
        if (!isJsonPath(resolved)) {
            throw std::runtime_error(
                "GenIE materialization: expected JSON reference, got: " +
                value);
        }
        return resolved;
    }

    // Maps a path rooted at staging_root_ (as returned by materializeJson())
    // to its eventual location under published_root_. materialize() writes
    // every file under a "<name>.staging" directory and only renames the
    // whole tree to "<name>" (published_root_) after all JSON content has
    // been generated — so absolute paths embedded *inside* that JSON content
    // must reference the post-rename (published) location, not the
    // still-staging one, or the closed-source GenIE SDK will try to open a
    // path that no longer exists once the rename has happened.
    fs::path publishedPathFor(const fs::path& staging_path) const {
        const fs::path relative =
            fs::relative(staging_path, staging_root_).lexically_normal();
        return published_root_ / relative;
    }

private:
    void ensureInsideSourceRoot(const fs::path& path) const {
        const fs::path relative =
            fs::relative(path, source_root_).lexically_normal();
        if (!isSafeRelativePath(relative)) {
            throw std::runtime_error(
                "GenIE materialization: reference escapes source tree: " +
                path.string());
        }
    }

    bool explicitExtensionReference(const json& value) const {
        bool found = false;
        collectExtensionReferences(value, found);
        return found;
    }

    void collectExtensionReferences(const json& value, bool& found) const {
        if (!value.is_object()) return;
        for (const auto& [key, item] : value.items()) {
            if (isExtensionKey(key)) {
                found = true;
                return;
            }
            if (item.is_object()) collectExtensionReferences(item, found);
        }
    }

    fs::path conventionalExtension(const fs::path& current_dir) const {
        const fs::path local = current_dir / "htp_backend_ext_config.json";
        if (fs::is_regular_file(local)) return local;

        const fs::path root = source_root_ / "htp_backend_ext_config.json";
        if (fs::is_regular_file(root)) return root;

        return {};
    }

    void patchExtension(json& extension) const {
        if (!extension.is_object() || !extension.contains("devices") ||
            !extension["devices"].is_array() ||
            extension["devices"].empty() ||
            !extension["devices"][0].is_object()) {
            throw std::runtime_error(
                "GenIE materialization: extension must contain a non-empty "
                "devices object array");
        }

        auto& device = extension["devices"][0];
        if (device.contains("device_id") &&
            !(device["device_id"].is_number_integer() ||
              device["device_id"].is_number_unsigned())) {
            throw std::runtime_error(
                "GenIE materialization: devices[0].device_id is not numeric");
        }
        device["device_id"] = selected_device_id_;
    }

    void rewriteJson(json& value,
                     const fs::path& current_dir,
                     const fs::path& destination_dir,
                     bool is_root) {
        if (value.is_array()) {
            for (auto& item : value) {
                rewriteJson(item, current_dir, destination_dir, false);
            }
            return;
        }

        if (value.is_string()) {
            const std::string reference = value.get<std::string>();
            if (reference.size() >= 5 &&
                lower(reference).substr(reference.size() - 5) == ".json" &&
                !isTokenizerAssetFilename(fs::path(reference))) {
                std::error_code ec;
                const fs::path candidate =
                    fs::path(reference).is_absolute()
                        ? fs::path(reference)
                        : current_dir / fs::path(reference);
                if (fs::is_regular_file(candidate, ec) && !ec) {
                    const fs::path referenced_source =
                        resolveJsonReference(reference, current_dir);
                    const fs::path referenced_destination =
                        materializeJson(referenced_source);
                    // Absolute path pointing at the eventual *published*
                    // location (not the staging one materializeJson()
                    // returns) — see publishedPathFor()'s comment. This
                    // string is embedded directly into the JSON payload
                    // handed to the closed-source GenIE SDK, which resolves
                    // any relative path against the worker process's
                    // current working directory, so it must be absolute and
                    // must survive the staging→published rename.
                    value = publishedPathFor(referenced_destination).string();
                }
            }
            return;
        }

        if (!value.is_object()) return;

        for (auto& [key, item] : value.items()) {
            if (isExtensionKey(key)) {
                const auto refs = stringLeaves(item);
                if (refs.empty()) {
                    throw std::runtime_error(
                        "GenIE materialization: backend.extensions must "
                        "contain a JSON path");
                }

                for (const auto& ref : refs) {
                    const fs::path extension_source =
                        resolveJsonReference(ref, current_dir);
                    const fs::path extension_destination =
                        materializeJson(extension_source);

                    std::ifstream extension_input(extension_source);
                    json extension;
                    try {
                        extension_input >> extension;
                    } catch (const std::exception& error) {
                        throw std::runtime_error(
                            "GenIE materialization: failed to parse extension '" +
                            extension_source.string() + "': " + error.what());
                    }
                    patchExtension(extension);

                    writeJsonAtomically(extension, extension_destination);
                    replaceReference(item, ref,
                                     publishedPathFor(extension_destination),
                                     destination_dir);
                }
                continue;
            }

            if (item.is_object() && isBackendKey(key) &&
                !explicitExtensionReference(item)) {
                const fs::path fallback = conventionalExtension(current_dir);
                if (!fallback.empty()) {
                    const fs::path extension_source =
                        canonicalExisting(fallback, "conventional extension");
                    const fs::path extension_destination =
                        materializeJson(extension_source);

                    std::ifstream extension_input(extension_source);
                    json extension;
                    extension_input >> extension;
                    patchExtension(extension);
                    writeJsonAtomically(extension, extension_destination);

                    // Absolute published path — same rationale as the
                    // plain-reference branch above (must survive the
                    // staging→published rename and be resolvable
                    // independent of the worker's CWD).
                    item["extensions"] =
                        publishedPathFor(extension_destination).string();
                }
            }

            rewriteJson(item, current_dir, destination_dir, false);
        }

        (void)is_root;
    }

    void replaceReference(json& value,
                          const std::string& original,
                          const fs::path& destination,
                          const fs::path& reference_base) {
        (void)reference_base;
        if (value.is_string()) {
            if (value.get<std::string>() == original) {
                // Absolute path — see the rewriteJson() plain-reference
                // branch above for why a relative path is unsafe here (the
                // GenIE SDK resolves it against the worker's CWD, not this
                // JSON file's destination directory).
                value = destination.string();
            }
            return;
        }
        if (value.is_array()) {
            for (auto& item : value) {
                replaceReference(item, original, destination, reference_base);
            }
            return;
        }
        if (value.is_object()) {
            for (auto& [key, item] : value.items()) {
                (void)key;
                replaceReference(item, original, destination, reference_base);
            }
        }
    }

    void writeJsonAtomically(const json& data, const fs::path& destination) const {
        fs::create_directories(destination.parent_path());
        const fs::path temporary = destination.string() + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "GenIE materialization: cannot write " + temporary.string());
        }
        output << data.dump(4);
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "GenIE materialization: failed writing " + temporary.string());
        }
        output.close();

        std::error_code ec;
        fs::rename(temporary, destination, ec);
        if (ec) {
            fs::remove(temporary);
            throw std::runtime_error(
                "GenIE materialization: cannot publish " +
                destination.string() + ": " + ec.message());
        }
    }

    fs::path source_root_;
    fs::path staging_root_;
    fs::path published_root_;
    fs::path root_source_;
    int selected_device_id_;

    std::set<std::string> active_;
    std::unordered_map<std::string, fs::path> destinations_;
    std::unordered_map<std::string, std::string> destination_sources_;
};

}  // namespace

GenieConfigMaterializer::GenieConfigMaterializer()
    : options_{} {}

GenieConfigMaterializer::GenieConfigMaterializer(Options options)
    : options_(std::move(options)) {}

MaterializedLaunchSpec GenieConfigMaterializer::materialize(
    const fs::path& config_file,
    const fs::path& sampler_file,
    const RuntimeId& runtime_id,
    int selected_device_id,
    RuntimeArtifacts::GenerationToken generation) const {
    const fs::path root_config = canonicalExisting(config_file, "root config");
    const fs::path requested_source_root =
        options_.source_root.empty()
            ? root_config.parent_path()
            : options_.source_root;
    const fs::path source_root =
        canonicalExisting(requested_source_root, "source root");
    const fs::path root_relative =
        fs::relative(root_config, source_root).lexically_normal();
    if (!isSafeRelativePath(root_relative)) {
        throw std::runtime_error(
            "GenIE materialization: root config is outside source root: " +
            root_config.string());
    }

    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::string name =
        sanitizePathComponent(runtime_id.key()) + "-" + std::to_string(stamp);

    fs::create_directories(options_.artifact_parent);
    const fs::path staging =
        options_.artifact_parent / (name + ".staging");
    const fs::path published = options_.artifact_parent / name;

    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::remove_all(published, ec);
    fs::create_directories(staging);

    try {
        MaterializationContext context(source_root, staging, published,
                                       selected_device_id);
        context.setRootSource(root_config);
        const fs::path materialized_config =
            context.materializeJson(root_config);
        const fs::path materialized_config_relative =
            fs::relative(materialized_config, staging).lexically_normal();

        fs::path materialized_sampler;
        if (!sampler_file.empty()) {
            const fs::path sampler = canonicalExisting(sampler_file, "sampler config");
            context.materializeJson(sampler);
            const fs::path relative =
                fs::relative(sampler, source_root).lexically_normal();
            if (!isSafeRelativePath(relative)) {
                throw std::runtime_error(
                    "GenIE materialization: sampler is outside source tree");
            }
            materialized_sampler = staging / relative;
        }
        const fs::path materialized_sampler_relative =
            materialized_sampler.empty()
                ? fs::path{}
                : fs::relative(materialized_sampler, staging).lexically_normal();

        fs::rename(staging, published, ec);
        if (ec) {
            throw std::runtime_error(
                "GenIE materialization: failed to publish artifact tree: " +
                ec.message());
        }

        auto artifacts = RuntimeArtifacts::create(published, std::move(generation));

        MaterializedLaunchSpec result;
        result.runtime_id = runtime_id;
        result.provider = options_.provider;
        result.config_file = published / materialized_config_relative;
        if (!materialized_sampler_relative.empty()) {
            result.sampler_config_file = published / materialized_sampler_relative;
        }
        result.native_options = {
            {"device_id", selected_device_id}
        };
        result.artifacts = std::move(artifacts);
        return result;
    } catch (...) {
        std::error_code cleanup_ec;
        fs::remove_all(staging, cleanup_ec);
        fs::remove_all(published, cleanup_ec);
        throw;
    }
}

}  // namespace execution
}  // namespace qai_forge
