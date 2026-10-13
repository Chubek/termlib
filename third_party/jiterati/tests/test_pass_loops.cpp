/** @file test_pass_loops.cpp @brief Loop legality, live-out and bounded schedule tests. */
#include "PassInterpreter.hpp"
using namespace pass_test;

struct Loop {
    Block* header;
    Value phi, next, value;
};
static Loop build_loop(Function& fn, Block* preheader, Block* exit, unsigned count, bool memory = false, std::string name = "loop") {
    auto* loop = fn.create_block(std::move(name));
    // Redirect an existing preheader branch when chaining adjacent loops.
    if (preheader->instructions().empty()) preheader->br(loop);
    else preheader->set_successors({loop});
    auto phi = loop->phi(Type::i32(),{{preheader,fn.const_i32(0)},{loop,Value::none()}});
    auto value = loop->add(loop->arg(0),phi);
    if (memory) loop->store(loop->arg(1),value);
    auto next = loop->add(phi,fn.const_i32(1));
    loop->cond_br(loop->icmp_slt(next,fn.const_i32(static_cast<int32_t>(count))),loop,exit);
    fn.instruction_by_id(phi.vreg_index())->set_operand(3,next);
    return {loop,phi,next,value};
}

static void simple_tests() {
    for (auto name : {"loop-unrolling","loop-peeling","polyhedral-optimization"}) for (bool memory : {false,true}) {
        Module m; auto* fn = m.create_function("loop",Type::i32(),{Type::i32(),Type::ptr()});
        auto* pre = fn->create_block("entry"); auto* exit = fn->create_block("exit"); auto loop = build_loop(*fn,pre,exit,4,memory);
        // Add an exit phi to ensure the exiting edge is renamed after expansion.
        exit->ret(exit->phi(Type::i32(),{{loop.header,loop.value}}));
        verify(*fn); auto before = execute(*fn,{37,0x1000}); CHECK(!before.trapped);
        run(*fn,name); CHECK(before == execute(*fn,{37,0x1000}));
        if (std::string_view(name) == "loop-peeling") {
            CHECK(fn->blocks().size() == 3); CHECK(pre->instructions().size() > 1);
            auto* phi = fn->instruction_by_id(loop.phi.vreg_index());
            CHECK(execute(*fn,{0,0x1000}).returned == 3); CHECK(phi->opcode() == Opcode::Phi);
        } else CHECK(fn->blocks().size() == 2);
    }
    // The expansion limit rejects a larger finite domain without changing IR.
    Module m; auto* fn = m.create_function("large",Type::i32(),{Type::i32()});
    auto* pre = fn->create_block("entry"); auto* exit = fn->create_block("exit"); auto loop = build_loop(*fn,pre,exit,64);
    exit->ret(loop.value); auto text = fn->to_string(); run(*fn,"loop-unrolling"); CHECK(text == fn->to_string());
    CHECK(execute(*fn,{12}).returned == 75);
    // A trapping invariant must remain inside the loop. A transitive copy of a
    // parameter may be hoisted only after its SSA operand is resolved.
    Module licm; auto* lf = licm.create_function("licm",Type::i32(),{Type::i32()});
    auto* lp = lf->create_block("entry"); auto* lx = lf->create_block("exit"); auto l = build_loop(*lf,lp,lx,3);
    auto* term = l.header->instructions().back(); l.header->instructions().pop_back();
    auto alias = l.header->build_instruction(Opcode::Copy,Type::i32(),{l.header->arg(0)});
    auto invariant = l.header->mul(alias,lf->const_i32(5)); auto trap = l.header->sdiv(l.header->arg(0),lf->const_i32(0));
    l.header->instructions().push_back(term); lx->ret(l.value);
    auto before = execute(*lf,{8}); run(*lf,"loop-invariant-code-motion"); CHECK(before == execute(*lf,{8}));
    CHECK(lf->instruction_by_id(invariant.vreg_index())->parent() == lp);
    CHECK(lf->instruction_by_id(trap.vreg_index())->parent() == l.header);
}

static void fusion_tests() {
    Module m; auto* fn = m.create_function("fusion",Type::i32(),{Type::i32()});
    auto* pre = fn->create_block("entry"); auto* exit = fn->create_block("exit");
    auto first = build_loop(*fn,pre,exit,3,false,"first");
    // A second counted loop receives the first loop's exit edge as its entry.
    auto* second = fn->create_block("second");
    first.header->set_successors({first.header,second});
    auto phi = second->phi(Type::i32(),{{first.header,fn->const_i32(0)},{second,Value::none()}});
    auto product = second->mul(second->arg(0),second->add(phi,fn->const_i32(1)));
    auto next = second->add(phi,fn->const_i32(1));
    second->cond_br(second->icmp_slt(next,fn->const_i32(3)),second,exit);
    fn->instruction_by_id(phi.vreg_index())->set_operand(3,next);
    exit->ret(exit->add(first.value,product));
    verify(*fn); auto before = execute(*fn,{19}); CHECK(!before.trapped);
    run(*fn,"loop-fusion"); CHECK(before == execute(*fn,{19})); CHECK(fn->blocks().size() == 2);
    // Unequal domains remain separate.
    Module mismatch; auto* mf = mismatch.create_function("mismatch",Type::i32(),{Type::i32()});
    auto* mp = mf->create_block("entry"); auto* mx = mf->create_block("exit"); auto a = build_loop(*mf,mp,mx,3,false,"a");
    auto* mb = mf->create_block("b"); a.header->set_successors({a.header,mb});
    auto bp = mb->phi(Type::i32(),{{a.header,mf->const_i32(0)},{mb,Value::none()}});
    auto bn = mb->add(bp,mf->const_i32(1)); mb->cond_br(mb->icmp_slt(bn,mf->const_i32(4)),mb,mx);
    mf->instruction_by_id(bp.vreg_index())->set_operand(3,bn); mx->ret(bn);
    auto text = mf->to_string(); run(*mf,"loop-fusion"); CHECK(text == mf->to_string());
}

static void nest_tests() {
    for (auto pass : {"loop-interchange","polyhedral-optimization"}) for (bool memory : {false,true}) {
        Module m; auto* fn = m.create_function("nest",Type::i32(),{Type::i32(),Type::ptr()});
        auto* pre = fn->create_block("entry"); auto* outer = fn->create_block("outer");
        auto* inner = fn->create_block("inner"); auto* latch = fn->create_block("latch"); auto* exit = fn->create_block("exit");
        pre->br(outer);
        auto i = outer->phi(Type::i32(),{{pre,fn->const_i32(0)},{latch,Value::none()}}); outer->br(inner);
        auto j = inner->phi(Type::i32(),{{outer,fn->const_i32(0)},{inner,Value::none()}});
        auto output = inner->add(inner->mul(i,fn->const_i32(10)),inner->add(j,inner->arg(0)));
        if (memory) inner->store(inner->arg(1),output);
        auto jnext = inner->add(j,fn->const_i32(1)); inner->cond_br(inner->icmp_slt(jnext,fn->const_i32(4)),inner,latch);
        auto inext = latch->add(i,fn->const_i32(1)); latch->cond_br(latch->icmp_slt(inext,fn->const_i32(3)),outer,exit);
        fn->instruction_by_id(i.vreg_index())->set_operand(3,inext); fn->instruction_by_id(j.vreg_index())->set_operand(3,jnext);
        exit->ret(exit->add(output,inext)); verify(*fn);
        auto before = execute(*fn,{7,0x1000}); CHECK(!before.trapped); CHECK(before.returned == 33);
        run(*fn,pass); CHECK(before == execute(*fn,{7,0x1000}));
        if (!memory) CHECK(fn->blocks().size() == 2);
        else if (std::string_view(pass) == "loop-interchange") CHECK(fn->blocks().size() == 5);
    }
}

int main() { simple_tests(); fusion_tests(); nest_tests(); std::puts("test_pass_loops passed"); }
