/** @file Jiterati-Pass.hpp
 *  @brief Executable pass pipelines and function-local analysis results.
 */
#ifndef JITERATI_PASS_HPP_INCLUDED
#define JITERATI_PASS_HPP_INCLUDED

#include "Jiterati.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace jiterati {

/** Results of the most recent function processed by an analysis pass.
 *  Register colors are virtual, target-independent; -1 denotes a spill.
 *  Results must be recomputed after modifying their input function.
 */
struct PassResult {
    std::vector<Block*> postorder;
    std::unordered_map<Block*, std::unordered_set<Block*>> dominators;
    std::unordered_map<Block*, std::unordered_set<uint32_t>> live_in, live_out;
    std::unordered_set<uint32_t> escaping_allocas;
    std::unordered_map<uint32_t, int> registers;
};

/** Base interface for registered analyses and transformations. */
class Pass {
public:
    virtual ~Pass() = default;
    virtual std::string name() const = 0;
    virtual bool is_analysis() const { return false; }
    virtual bool is_transform() const { return false; }
    /** Analysis output, or nullptr for transformations. */
    virtual PassResult const* result() const { return nullptr; }
};

class FunctionPass : public virtual Pass {
public:
    virtual void run(Function& fn) = 0;
};

class ModulePass : public virtual Pass {
public:
    virtual void run(Module& m) = 0;
};

class AnalysisPass : public virtual Pass {
public:
    virtual bool is_analysis() const final { return true; }
};

class TransformPass : public virtual Pass {
public:
    virtual bool is_transform() const final { return true; }
};

class PassManager {
public:
    void add(std::unique_ptr<Pass> pass);
    void run(Module& m);
private:
    std::vector<std::unique_ptr<Pass>> passes_;
};

class AnalysisManager {
public:
    // Placeholder for analysis result caching.
};

/** Built-in pass descriptor. Names are canonical kebab-case JPL tokens. */
struct PassInfo {
    std::string_view name;
    std::string_view class_name;
    std::string_view description;
    bool analysis;
};

/** Return the deterministic built-in pass catalog. */
std::vector<PassInfo> const& available_passes();
/** Create a built-in pass; accepts canonical names, class names and legacy aliases.
 *  @throws std::invalid_argument for unknown names.
 */
std::unique_ptr<Pass> create_pass(std::string_view name);
/** Parse semicolon/whitespace-delimited pass names into an executable pipeline.
 *  @throws std::invalid_argument for unknown names.
 */
PassManager parse_jpl_pipeline(std::string_view source);

inline void PassManager::add(std::unique_ptr<Pass> pass) {
    passes_.push_back(std::move(pass));
}

inline void PassManager::run(Module& m) {
    for (auto& p : passes_) {
        if (auto* mp = dynamic_cast<ModulePass*>(p.get())) {
            mp->run(m);
        } else if (auto* fp = dynamic_cast<FunctionPass*>(p.get())) {
            for (auto& fn : m.functions()) {
                fp->run(*fn);
            }
        }
    }
}

} // namespace jiterati

#endif // JITERATI_PASS_HPP_INCLUDED
