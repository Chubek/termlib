/** @file test_passes.cpp @brief Differential semantics, SSA and legality tests for every pass. */
#include "PassInterpreter.hpp"
#include "Jiterati-SIMD.hpp"
#include <array>

using namespace pass_test;

struct ScalarCase {
    char const* pass;
    unsigned minimum_width;
    bool boolean;
    Opcode expected;
    std::function<Value(Function&, Block&, Type, Value, Value, Value)> build;
};

static void scalar_tests() {
    std::vector<ScalarCase> cases = {
        {"algebraic-simplification", 1, false, Opcode::Copy, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.add(x, constant(f,t,0)); }},
        {"reassociation", 1, false, Opcode::Add, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.add(b.add(x,constant(f,t,3)),constant(f,t,5)); }},
        {"commutative-canonicalization", 1, false, Opcode::Mul, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.mul(constant(f,t,3),x); }},
        {"comparison-canonicalization", 1, false, Opcode::ICmpSLt, [](auto&, auto& b, auto, auto x, auto y, auto) { return b.icmp_sgt(x,y); }},
        {"select-simplification", 1, false, Opcode::Copy, [](auto& f, auto& b, auto, auto x, auto y, auto) { return b.select(f.const_bool(true),x,y); }},
        {"redundant-cast-elimination", 1, false, Opcode::Copy, [](auto&, auto& b, auto t, auto x, auto, auto) { return b.trunc(t,b.zext(Type::i64(),x)); }},
        {"double-negation-elimination", 1, false, Opcode::Copy, [](auto&, auto& b, auto, auto x, auto, auto) { return b.neg(b.neg(x)); }},
        {"double-not-elimination", 1, false, Opcode::Copy, [](auto&, auto& b, auto, auto x, auto, auto) { return b.bitwise_not(b.bitwise_not(x)); }},
        {"bitwise-idempotence", 1, false, Opcode::Copy, [](auto&, auto& b, auto, auto x, auto, auto) { return b.bitwise_xor(x,x); }},
        {"bitwise-absorption", 1, false, Opcode::Copy, [](auto&, auto& b, auto, auto x, auto y, auto) { return b.bitwise_and(x,b.bitwise_or(x,y)); }},
        {"bitwise-complement-folding", 1, false, Opcode::Copy, [](auto&, auto& b, auto, auto x, auto, auto) { return b.bitwise_or(x,b.bitwise_not(x)); }},
        {"shift-simplification", 1, false, Opcode::Copy, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.shl(x,constant(f,t,0)); }},
        {"shift-combination", 8, false, Opcode::LShr, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.lshr(b.lshr(x,constant(f,t,1)),constant(f,t,2)); }},
        {"unsigned-division-reduction", 8, false, Opcode::LShr, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.udiv(x,constant(f,t,8)); }},
        {"unsigned-remainder-reduction", 8, false, Opcode::And, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.urem(x,constant(f,t,8)); }},
        {"multiply-by-negative-one", 1, false, Opcode::Neg, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.mul(x,constant(f,t,UINT64_MAX)); }},
        {"subtraction-to-addition", 1, false, Opcode::Add, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.sub(x,constant(f,t,7)); }},
        {"negation-propagation", 1, false, Opcode::Sub, [](auto&, auto& b, auto, auto x, auto y, auto) { return b.add(x,b.neg(y)); }},
        {"boolean-comparison-simplification", 1, true, Opcode::Copy, [](auto& f, auto& b, auto, auto x, auto, auto) { return b.icmp_eq(x,f.const_bool(true)); }},
        {"boolean-select-simplification", 1, true, Opcode::Not, [](auto& f, auto& b, auto, auto x, auto, auto) { return b.select(x,f.const_bool(false),f.const_bool(true)); }},
        {"distributive-factoring", 1, false, Opcode::Mul, [](auto&, auto& b, auto, auto x, auto y, auto z) { return b.add(b.mul(x,y),b.mul(z,x)); }},
        {"mask-simplification", 1, false, Opcode::And, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.bitwise_and(constant(f,t,15),b.bitwise_and(x,constant(f,t,7))); }},
        {"strength-reduction", 8, false, Opcode::Shl, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.mul(constant(f,t,8),x); }},
        {"peephole-optimization", 1, false, Opcode::Copy, [](auto& f, auto& b, auto t, auto x, auto, auto) { return b.add(x,constant(f,t,0)); }},
        {"copy-propagation", 1, false, Opcode::Add, [](auto& f, auto& b, auto t, auto x, auto y, auto) {
            auto c = b.build_instruction(Opcode::Copy,t,{x}); return b.add(c,y);
        }},
    };
    std::array<Type,5> types = {Type::i1(),Type::i8(),Type::i16(),Type::i32(),Type::i64()};
    for (auto const& test : cases) for (auto type : types) {
        if (width(type) < test.minimum_width || (test.boolean && type != Type::i1())) continue;
        Module m; auto* fn = m.create_function("test",type,{type,type,type}); auto* b = fn->create_block("entry");
        auto value = test.build(*fn,*b,type,b->arg(0),b->arg(1),b->arg(2)); b->ret(value);
        auto* result = fn->instruction_by_id(value.vreg_index()); result->set_name("result");
        std::vector<std::vector<uint64_t>> inputs;
        uint64_t random = 0xd1b54a32d192ed03ull;
        std::array<uint64_t,6> edge = {0,1,limit(type),uint64_t(1)<<(width(type)-1),limit(type)/2,2};
        for (auto a : edge) for (auto c : edge) inputs.push_back({a,c,a^c});
        for (unsigned i = 0; i < 32; ++i) {
            random ^= random<<13; random ^= random>>7; random ^= random<<17;
            inputs.push_back({random,random*17,random>>3});
        }
        std::vector<Execution> before;
        for (auto const& input : inputs) { before.push_back(execute(*fn,input)); CHECK(!before.back().trapped); }
        auto previous = fn->to_string(); run(*fn,test.pass);
        CHECK(result->opcode() == test.expected); CHECK(result->parent() == b); CHECK(result->name() == "result");
        CHECK(previous != fn->to_string());
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            if (!(before[i] == execute(*fn,inputs[i]))) {
                std::fprintf(stderr,"differential failure: %s %s\n",test.pass,type.to_string().c_str()); std::exit(1);
            }
        }
    }
}

static void folding_tests() {
    for (auto type : {Type::i1(),Type::i8(),Type::i16(),Type::i32(),Type::i64()}) {
        for (auto op : {Opcode::Add,Opcode::Sub,Opcode::Mul,Opcode::Neg,Opcode::Not,Opcode::And,Opcode::Or,Opcode::Xor,
                       Opcode::SDiv,Opcode::SRem,Opcode::UDiv,Opcode::URem,Opcode::Shl,Opcode::LShr,Opcode::AShr,
                       Opcode::ICmpEq,Opcode::ICmpNe,Opcode::ICmpSLt,Opcode::ICmpSLe,Opcode::ICmpSGt,Opcode::ICmpSGe,
                       Opcode::ICmpULt,Opcode::ICmpULe,Opcode::ICmpUGt,Opcode::ICmpUGe}) {
            for (auto a : {uint64_t(0),uint64_t(1),limit(type),uint64_t(1)<<(width(type)-1)})
                for (auto c : {uint64_t(0),uint64_t(1),uint64_t(width(type)-1),uint64_t(width(type)),limit(type)}) {
                    Module m; auto* fn = m.create_function("fold",type,{}); auto* b = fn->create_block("entry");
                    bool compare = op >= Opcode::ICmpEq && op <= Opcode::ICmpUGe;
                    std::vector<Value> operands{constant(*fn,type,a)};
                    if (op != Opcode::Neg && op != Opcode::Not) operands.push_back(constant(*fn,type,c));
                    auto v = b->build_instruction(op,compare?Type::i1():type,operands); b->ret(v);
                    auto before = execute(*fn,{}); run(*fn,"constant-folding"); CHECK(before == execute(*fn,{}));
                    if (before.trapped) CHECK(fn->instruction_by_id(v.vreg_index())->opcode() == op);
                    else CHECK(fn->instruction_by_id(v.vreg_index())->opcode() == Opcode::Copy);
                }
        }
        for (auto op : {Opcode::Trunc,Opcode::ZExt,Opcode::SExt}) {
            Module m; auto* fn = m.create_function("cast",Type::i64(),{}); auto* b = fn->create_block("entry");
            auto v = b->build_instruction(op,Type::i64(),{constant(*fn,type,limit(type))}); b->ret(v);
            auto before = execute(*fn,{}); run(*fn,"constant-folding"); CHECK(before == execute(*fn,{}));
        }
    }
    Module m; auto* fn = m.create_function("propagate",Type::i32(),{}); auto* b = fn->create_block("entry");
    auto sum = b->add(fn->const_i32(2),fn->const_i32(3)); auto product = b->mul(sum,fn->const_i32(7)); b->ret(product);
    run(*fn,"constant-propagation"); CHECK(execute(*fn,{}).returned == 35);
    CHECK(fn->instruction_by_id(product.vreg_index())->opcode() == Opcode::Copy);
}

static void cfg_tests() {
    for (auto pass : {"phi-simplification","control-flow-optimization","unreachable-block-elimination","partial-redundancy-elimination","common-subexpression-elimination"}) {
        Module m; auto* fn = m.create_function("diamond",Type::i32(),{Type::i1(),Type::i32()});
        auto* entry = fn->create_block("entry"); auto* left = fn->create_block("left");
        auto* right = fn->create_block("right"); auto* join = fn->create_block("join");
        auto x = entry->arg(1); auto one = fn->const_i32(1);
        bool fold = std::string_view(pass) == "control-flow-optimization";
        entry->cond_br(fold ? fn->const_bool(false) : entry->arg(0),left,right);
        auto a = left->add(x,one); left->br(join);
        auto c = right->add(x,one); right->br(join);
        auto common = join->phi(Type::i32(),{{left,x},{right,x}});
        auto repeated = join->add(x,one); auto v = join->add(common,join->add(repeated,join->phi(Type::i32(),{{left,a},{right,c}})));
        join->ret(v);
        auto* orphan = fn->create_block("orphan"); orphan->ret(fn->const_i32(99));
        std::vector<Execution> before; for (uint64_t cond : {0,1}) before.push_back(execute(*fn,{cond,123}));
        run(*fn,pass); for (uint64_t cond : {0,1}) CHECK(before[cond] == execute(*fn,{cond,123}));
        if (std::string_view(pass) == "phi-simplification") CHECK(fn->instruction_by_id(common.vreg_index())->opcode() == Opcode::Copy);
        if (fold) CHECK(fn->blocks().size() == 1);
        if (std::string_view(pass) == "unreachable-block-elimination") CHECK(fn->blocks().size() == 4);
        if (std::string_view(pass) == "partial-redundancy-elimination") CHECK(fn->instruction_by_id(repeated.vreg_index())->opcode() == Opcode::Copy);
    }
    // Partial availability requires edge insertion, including resolution of a
    // copy defined in the join rather than incorrectly moving its SSA use.
    Module m; auto* fn = m.create_function("pre",Type::i32(),{Type::i1(),Type::i32()});
    auto* e = fn->create_block("entry"); auto* a = fn->create_block("a"); auto* b = fn->create_block("b"); auto* j = fn->create_block("join");
    e->cond_br(e->arg(0),a,b); auto one = fn->const_i32(1); a->add(e->arg(1),one); a->br(j); b->br(j);
    auto alias = j->build_instruction(Opcode::Copy,Type::i32(),{e->arg(1)}); auto v = j->add(alias,one); j->ret(v);
    auto before = execute(*fn,{0,55}); run(*fn,"partial-redundancy-elimination"); CHECK(before == execute(*fn,{0,55}));
    CHECK(fn->instruction_by_id(v.vreg_index())->opcode() == Opcode::Copy); CHECK(b->instructions().size() == 2);
    // CSE must not reuse a value from a nondominating sibling.
    run(*fn,"common-subexpression-elimination"); CHECK(before == execute(*fn,{0,55}));
    Module thread; auto* tf = thread.create_function("thread",Type::i32(),{});
    auto* start = tf->create_block("entry"); auto* trampoline = tf->create_block("trampoline"); auto* end = tf->create_block("exit");
    start->br(trampoline); trampoline->br(end); end->ret(tf->const_i32(8)); run(*tf,"jump-threading");
    CHECK(start->successors()[0] == end); CHECK(execute(*tf,{}).returned == 8);
    // Duplicate conditional successors retain their true/false edge positions.
    Module duplicate; auto* df = duplicate.create_function("duplicate",Type::i32(),{Type::i1()});
    auto* de = df->create_block("entry"); auto* dx = df->create_block("exit"); de->cond_br(de->arg(0),dx,dx); dx->ret(df->const_i32(9));
    CHECK(de->successors().size() == 2); run(*df,"control-flow-optimization"); CHECK(df->blocks().size() == 1);
}

static void memory_tests() {
    for (auto pass : {"load-forwarding","redundant-load-elimination","dead-store-elimination","store-of-load-elimination","stack-promotion"}) {
        Module m; auto* fn = m.create_function("memory",Type::i32(),{Type::i32()}); auto* b = fn->create_block("entry");
        auto pointer = b->alloca(Type::i32()); b->store(pointer,fn->const_i32(7)); b->store(pointer,b->arg(0));
        auto first = b->load(Type::i32(),pointer); auto second = b->load(Type::i32(),pointer); b->store(pointer,second); b->ret(b->add(first,second));
        auto before = execute(*fn,{42}); CHECK(!before.trapped); auto count = fn->instr_storage().size();
        run(*fn,pass); CHECK(before == execute(*fn,{42}));
        auto p = std::string_view(pass);
        if (p == "load-forwarding" || p == "stack-promotion") CHECK(fn->instruction_by_id(first.vreg_index())->opcode() == Opcode::Copy);
        if (p == "redundant-load-elimination") CHECK(fn->instruction_by_id(second.vreg_index())->opcode() == Opcode::Copy);
        if (p == "dead-store-elimination" || p == "store-of-load-elimination" || p == "stack-promotion") CHECK(fn->instr_storage().size() < count);
    }
    // Unknown pointer parameters may alias; a different-address store is a clobber.
    for (auto pass : {"load-forwarding","redundant-load-elimination","dead-store-elimination"}) {
        Module m; auto* fn = m.create_function("alias",Type::i32(),{Type::ptr(),Type::ptr()}); auto* b = fn->create_block("entry");
        b->store(b->arg(0),fn->const_i32(4)); auto old = b->load(Type::i32(),b->arg(0));
        b->store(b->arg(1),fn->const_i32(9)); auto fresh = b->load(Type::i32(),b->arg(0)); b->ret(b->add(old,fresh));
        auto alias = execute(*fn,{0x1000,0x1000}), distinct = execute(*fn,{0x1000,0x2000});
        run(*fn,pass); CHECK(alias == execute(*fn,{0x1000,0x1000})); CHECK(distinct == execute(*fn,{0x1000,0x2000}));
    }
    // Reads and potential arithmetic traps prevent dead-store motion.
    Module m; auto* fn = m.create_function("barrier",Type::i32(),{Type::ptr()}); auto* b = fn->create_block("entry");
    b->store(b->arg(0),fn->const_i32(4)); b->sdiv(fn->const_i32(1),fn->const_i32(0)); b->store(b->arg(0),fn->const_i32(9)); b->ret(fn->const_i32(0));
    auto before = execute(*fn,{0x1000}); run(*fn,"dead-store-elimination"); CHECK(before == execute(*fn,{0x1000}));
}

static void dead_and_schedule_tests() {
    for (auto pass : {"dead-code-removal","dead-code-elimination"}) {
        Module m; auto* fn = m.create_function("dead",Type::i32(),{Type::i32()}); auto* b = fn->create_block("entry");
        b->mul(b->add(b->arg(0),fn->const_i32(3)),fn->const_i32(7));
        auto division = b->sdiv(b->arg(0),fn->const_i32(0)); b->ret(fn->const_i32(5));
        auto before = execute(*fn,{19}); run(*fn,pass); CHECK(before == execute(*fn,{19}));
        CHECK(fn->instruction_by_id(division.vreg_index()) != nullptr); CHECK(b->instructions().size() == 2);
    }
    Module m; auto* fn = m.create_function("schedule",Type::i64(),{Type::i64(),Type::i64()}); auto* b = fn->create_block("entry");
    auto short_chain = b->add(b->arg(0),fn->const_i64(1));
    auto long_chain = b->mul(b->arg(1),fn->const_i64(3)); auto next = b->add(long_chain,fn->const_i64(4));
    b->ret(b->add(short_chain,next)); auto before = execute(*fn,{7,9}); run(*fn,"instruction-scheduling");
    CHECK(before == execute(*fn,{7,9})); CHECK(b->instructions()[0]->id() == long_chain.vreg_index());
    // Mark/sweep deletes a dead phi cycle that reference-count DCE cannot remove.
    Module cycle; auto* cf = cycle.create_function("cycle",Type::i32(),{}); auto* ce = cf->create_block("entry"); auto* loop = cf->create_block("loop");
    ce->ret(cf->const_i32(0)); auto phi = loop->phi(Type::i32(),{{loop,cf->const_i32(1)}}); loop->br(loop);
    cf->instruction_by_id(phi.vreg_index())->set_operand(1,phi); run(*cf,"dead-code-elimination"); CHECK(loop->instructions().size() == 1);
}

static void call_and_simd_tests() {
    Module m; auto* callee = m.create_function("callee",Type::i32(),{Type::i32()}); auto* body = callee->create_block("entry");
    auto value = body->add(body->arg(0),callee->const_i32(7)); body->ret(value);
    auto* fn = m.create_function("caller",Type::i32(),{Type::i32()}); auto* b = fn->create_block("entry");
    auto call = b->call(callee,{b->arg(0)}); b->ret(call);
    run(*fn,"tail-call-optimization"); CHECK(fn->instruction_by_id(call.vreg_index())->is_tail_call());
    auto before = execute(*fn,{17}); run(*fn,"function-inlining"); CHECK(before == execute(*fn,{17}));
    CHECK(fn->instruction_by_id(call.vreg_index()) == nullptr);
    Module simd; auto* sf = simd.create_function("simd",Type::i32(),{Type::i32()}); auto* sb = sf->create_block("entry");
    std::vector<Value> lanes;
    auto lane_count = std::max<std::size_t>(2,jiterati::simd::policy_width(jiterati::simd::VectorPolicy::Auto,4));
    for (std::size_t i = 0; i < lane_count; ++i) lanes.push_back(sb->add(sb->arg(0),sf->const_i32(static_cast<int32_t>(i))));
    sb->ret(lanes.back()); auto text = sf->to_string(); auto expected = execute(*sf,{19}); run(*sf,"autovectorize");
    CHECK(text == sf->to_string()); CHECK(expected == execute(*sf,{19}));
    if (lane_count > 1 && jiterati::simd::policy_width(jiterati::simd::VectorPolicy::Auto,4) > 1)
        CHECK(sf->instruction_by_id(lanes[0].vreg_index())->vector_width() == lane_count);
}

static void analysis_tests() {
    Module m; auto* fn = m.create_function("analysis",Type::i32(),{Type::i1(),Type::i32(),Type::i32()});
    auto* e = fn->create_block("entry"); auto* a = fn->create_block("a"); auto* b = fn->create_block("b"); auto* j = fn->create_block("join");
    auto safe = e->alloca(Type::i32()); auto escaped = e->alloca(Type::i32());
    e->store(safe,e->arg(1)); e->ptrtoint(Type::i64(),escaped); e->cond_br(e->arg(0),a,b);
    a->br(j); b->br(j); j->ret(j->phi(Type::i32(),{{a,e->arg(1)},{b,e->arg(2)}}));
    auto text = fn->to_string();
    for (auto name : {"cfg-analysis","dominator-analysis","liveness-analysis","escape-analysis","register-allocation"}) {
        auto pass = create_pass(name); dynamic_cast<FunctionPass&>(*pass).run(*fn); CHECK(pass->result()); CHECK(text == fn->to_string());
        auto const& result = *pass->result(); auto n = std::string_view(name);
        if (n == "cfg-analysis") CHECK(result.postorder.back() == e && result.postorder.size() == 4);
        if (n == "dominator-analysis") CHECK(result.dominators.at(j).count(e) && !result.dominators.at(j).count(a));
        if (n == "liveness-analysis") {
            CHECK(result.live_out.at(a).count(1)); CHECK(!result.live_out.at(a).count(2));
            CHECK(result.live_out.at(b).count(2)); CHECK(!result.live_out.at(b).count(1));
        }
        if (n == "escape-analysis") {
            CHECK(!result.escaping_allocas.count(safe.vreg_index())); CHECK(result.escaping_allocas.count(escaped.vreg_index()));
        }
        if (n == "register-allocation") {
            CHECK(result.registers.at(1) >= 0 && result.registers.at(2) >= 0);
            CHECK(result.registers.at(1) != result.registers.at(2));
        }
    }
    verify(*fn);
}

static void integration_tests() {
    Module m;
    for (auto name : {"first","second"}) {
        auto* fn = m.create_function(name,Type::i32(),{Type::i32()}); auto* b = fn->create_block("entry");
        auto literal_sum = b->add(fn->const_i32(2),fn->const_i32(3));
        b->ret(b->mul(b->arg(0),literal_sum));
    }
    auto pipeline = parse_jpl_pipeline("cfg; ConstantPropagation; strength-reduction; dce"); pipeline.run(m);
    for (auto& fn : m.functions()) { verify(*fn); CHECK(execute(*fn,{7}).returned == 35); CHECK(fn->entry_block()->instructions().size() == 2); }
    // Dominating reuse is a positive CSE case, separate from sibling legality.
    Module common; auto* cf = common.create_function("common",Type::i32(),{Type::i32()}); auto* cb = cf->create_block("entry");
    auto one = cf->const_i32(1); auto a = cb->add(cb->arg(0),one); auto b = cb->add(one,cb->arg(0)); cb->ret(cb->add(a,b));
    auto before = execute(*cf,{19}); run(*cf,"common-subexpression-elimination"); CHECK(before == execute(*cf,{19}));
    CHECK(cf->instruction_by_id(b.vreg_index())->opcode() == Opcode::Copy);
    // Signed i1 division by bit-pattern one is division by -1 and may trap.
    Module boolean; auto* bf = boolean.create_function("signed_bool",Type::i1(),{Type::i1()}); auto* bb = bf->create_block("entry");
    auto division = bb->sdiv(bb->arg(0),bf->const_bool(true)); bb->ret(division);
    auto original = execute(*bf,{1}); CHECK(original.trapped); run(*bf,"algebraic-simplification"); CHECK(original == execute(*bf,{1}));
    // Truncation followed by extension is lossy and cannot be canceled.
    Module loss; auto* lf = loss.create_function("loss",Type::i64(),{Type::i64()}); auto* lb = lf->create_block("entry");
    auto extended = lb->zext(Type::i64(),lb->trunc(Type::i8(),lb->arg(0))); lb->ret(extended);
    auto expected = execute(*lf,{0x100}); run(*lf,"redundant-cast-elimination"); CHECK(expected == execute(*lf,{0x100}));
    CHECK(lf->instruction_by_id(extended.vreg_index())->opcode() == Opcode::ZExt);
}

static void corpus_tests() {
    // Deterministic randomized DAGs exercise interaction between passes, with
    // modular overflow, boolean widths, traps, selects and constant subgraphs.
    for (auto type : {Type::i1(),Type::i8(),Type::i16(),Type::i32(),Type::i64()}) for (unsigned seed = 1; seed <= 8; ++seed) {
        uint64_t state = seed*0x9e3779b97f4a7c15ull;
        auto random = [&] { state ^= state<<13; state ^= state>>7; state ^= state<<17; return state; };
        Module m; auto* fn = m.create_function("corpus",type,{type,type}); auto* b = fn->create_block("entry");
        std::vector<Value> pool{b->arg(0),b->arg(1),constant(*fn,type,0),constant(*fn,type,1),constant(*fn,type,3),constant(*fn,type,limit(type))};
        for (unsigned n = 0; n < 48; ++n) {
            auto a = pool[random()%pool.size()], c = pool[random()%pool.size()]; Value v;
            switch (random()%16) {
                case 0: v = b->add(a,c); break;
                case 1: v = b->sub(a,c); break;
                case 2: v = b->mul(a,c); break;
                case 3: v = b->bitwise_and(a,c); break;
                case 4: v = b->bitwise_or(a,c); break;
                case 5: v = b->bitwise_xor(a,c); break;
                case 6: v = b->neg(a); break;
                case 7: v = b->bitwise_not(a); break;
                case 8: v = b->shl(a,constant(*fn,type,random()%width(type))); break;
                case 9: v = b->lshr(a,constant(*fn,type,random()%width(type))); break;
                case 10: v = b->ashr(a,constant(*fn,type,random()%width(type))); break;
                case 11: v = b->select(b->icmp_slt(a,c),a,c); break;
                case 12: v = b->sdiv(a,constant(*fn,type,3)); break;
                case 13: v = b->urem(a,constant(*fn,type,3)); break;
                case 14: v = b->build_instruction(Opcode::Copy,type,{a}); break;
                default: v = b->add(a,a); break;
            }
            pool.push_back(v);
        }
        b->ret(pool.back());
        std::vector<std::vector<uint64_t>> inputs{{0,0},{1,1},{limit(type),1},{limit(type)/2,limit(type)}};
        for (unsigned i = 0; i < 12; ++i) inputs.push_back({random(),random()});
        std::vector<Execution> baseline;
        for (auto const& input : inputs) baseline.push_back(execute(*fn,input));
        for (auto const& info : available_passes()) {
            run(*fn,info.name);
            for (std::size_t i = 0; i < inputs.size(); ++i) if (!(baseline[i] == execute(*fn,inputs[i]))) {
                std::fprintf(stderr,"pipeline differential failure: %s %s seed=%u\n",std::string(info.name).c_str(),type.to_string().c_str(),seed); std::exit(1);
            }
        }
    }
}

int main() {
    CHECK(available_passes().size() == 55);
    std::set<std::string_view> names;
    for (auto const& info : available_passes()) {
        CHECK(names.insert(info.name).second); auto pass = create_pass(info.class_name);
        CHECK(pass->name() == info.name); CHECK(pass->is_analysis() == info.analysis); CHECK(pass->is_transform() != info.analysis);
    }
    bool rejected = false; try { parse_jpl_pipeline("constant-folding; misspelled"); } catch (std::invalid_argument const&) { rejected = true; } CHECK(rejected);
    scalar_tests(); folding_tests(); cfg_tests(); memory_tests(); dead_and_schedule_tests(); call_and_simd_tests(); analysis_tests();
    integration_tests(); corpus_tests();
    std::puts("test_passes passed");
}
