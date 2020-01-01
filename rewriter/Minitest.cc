#include "rewriter/Minitest.h"
#include "ast/Helpers.h"
#include "ast/ast.h"
#include "ast/treemap/treemap.h"
#include "core/Context.h"
#include "core/Names.h"
#include "core/core.h"
#include "core/errors/rewriter.h"
#include "rewriter/rewriter.h"

using namespace std;

namespace sorbet::rewriter {

namespace {
class ConstantMover {
    u4 classDepth = 0;
    vector<unique_ptr<ast::Expression>> movedConstants;

public:
    unique_ptr<ast::Expression> createConstAssign(ast::Assign &asgn) {
        auto loc = asgn.loc;
        auto unsafeNil = ast::MK::Unsafe(loc, ast::MK::Nil(loc));
        if (auto send = ast::cast_tree<ast::Send>(asgn.rhs.get())) {
            if (send->fun == core::Names::let() && send->args.size() == 2) {
                auto rhs = ast::MK::Let(loc, move(unsafeNil), send->args[1]->deepCopy());
                return ast::MK::Assign(asgn.loc, move(asgn.lhs), move(rhs));
            }
        }

        return ast::MK::Assign(asgn.loc, move(asgn.lhs), move(unsafeNil));
    }

    unique_ptr<ast::Expression> postTransformAssign(core::MutableContext ctx, unique_ptr<ast::Assign> asgn) {
        if (auto cnst = ast::cast_tree<ast::UnresolvedConstantLit>(asgn->lhs.get())) {
            if (ast::isa_tree<ast::UnresolvedConstantLit>(asgn->rhs.get())) {
                movedConstants.emplace_back(move(asgn));
                return ast::MK::EmptyTree();
            }
            auto name = ast::MK::Symbol(cnst->loc, cnst->cnst);

            // if the constant is already in a T.let, preserve it, otherwise decay it to unsafe
            movedConstants.emplace_back(createConstAssign(*asgn));

            auto module = ast::MK::Constant(asgn->loc, core::Symbols::Module());
            auto const_set = ctx.state.enterNameUTF8("const_set");
            return ast::MK::Send2(asgn->loc, move(module), const_set, move(name), move(asgn->rhs));
        }

        return asgn;
    }

    // classdefs define new constants, so we always move those if they're the "top-level" classdef (i.e. if we have
    // nested classdefs, we should only move the outermost one)
    unique_ptr<ast::ClassDef> preTransformClassDef(core::MutableContext ctx, unique_ptr<ast::ClassDef> classDef) {
        classDepth++;
        return classDef;
    }

    unique_ptr<ast::Expression> postTransformClassDef(core::MutableContext ctx, unique_ptr<ast::ClassDef> classDef) {
        classDepth--;
        if (classDepth == 0) {
            movedConstants.emplace_back(move(classDef));
            return ast::MK::EmptyTree();
        }
        return classDef;
    }

    // we move sends if they are other minitest `describe` blocks, as those end up being classes anyway: consequently,
    // we treat those the same way we treat classes
    unique_ptr<ast::Send> preTransformSend(core::MutableContext ctx, unique_ptr<ast::Send> send) {
        if (send->recv->isSelfReference() && send->args.size() == 1 && send->fun == core::Names::describe()) {
            classDepth++;
        }
        return send;
    }

    unique_ptr<ast::Expression> postTransformSend(core::MutableContext ctx, unique_ptr<ast::Send> send) {
        if (send->recv->isSelfReference() && send->args.size() == 1 && send->fun == core::Names::describe()) {
            classDepth--;
            if (classDepth == 0) {
                movedConstants.emplace_back(move(send));
                return ast::MK::EmptyTree();
            }
        }
        return send;
    }

    vector<unique_ptr<ast::Expression>> getMovedConstants() {
        return move(movedConstants);
    }

    unique_ptr<ast::Expression> addConstantsToExpression(core::Loc loc, unique_ptr<ast::Expression> expr) {
        auto consts = getMovedConstants();

        if (consts.empty()) {
            return expr;
        } else {
            ast::InsSeq::STATS_store stats;

            for (auto &m : consts) {
                stats.emplace_back(move(m));
            }

            return ast::MK::InsSeq(loc, std::move(stats), move(expr));
        }
    }
};

unique_ptr<ast::Expression> addSigVoid(unique_ptr<ast::Expression> expr) {
    return ast::MK::InsSeq1(expr->loc, ast::MK::SigVoid(expr->loc, ast::MK::Hash0(expr->loc)), std::move(expr));
}
} // namespace

unique_ptr<ast::Expression> recurse(core::MutableContext ctx, unique_ptr<ast::Expression> body);

unique_ptr<ast::Expression> prepareBody(core::MutableContext ctx, unique_ptr<ast::Expression> body) {
    body = recurse(ctx, std::move(body));

    auto bodySeq = ast::cast_tree<ast::InsSeq>(body.get());
    if (bodySeq) {
        for (auto &exp : bodySeq->stats) {
            exp = recurse(ctx, std::move(exp));
        }

        bodySeq->expr = recurse(ctx, std::move(bodySeq->expr));
    }
    return body;
}

string to_s(core::Context ctx, unique_ptr<ast::Expression> &arg) {
    auto argLit = ast::cast_tree<ast::Literal>(arg.get());
    string argString;
    if (argLit != nullptr) {
        if (argLit->isString(ctx)) {
            return argLit->asString(ctx).show(ctx);
        } else if (argLit->isSymbol(ctx)) {
            return argLit->asSymbol(ctx).show(ctx);
        }
    }
    auto argConstant = ast::cast_tree<ast::UnresolvedConstantLit>(arg.get());
    if (argConstant != nullptr) {
        return argConstant->cnst.show(ctx);
    }
    return arg->toString(ctx);
}

unique_ptr<ast::Expression> runSingle(core::MutableContext ctx, ast::Send *send) {
    if (send->block == nullptr) {
        return nullptr;
    }

    if (!send->recv->isSelfReference()) {
        return nullptr;
    }

    if (send->args.empty() && (send->fun == core::Names::before() || send->fun == core::Names::after())) {
        auto name = send->fun == core::Names::after() ? core::Names::afterAngles() : core::Names::initialize();
        ConstantMover constantMover;
        send->block->body = ast::TreeMap::apply(ctx, constantMover, move(send->block->body));
        auto method =
            addSigVoid(ast::MK::Method0(send->loc, send->loc, name, prepareBody(ctx, std::move(send->block->body)),
                                        ast::MethodDef::RewriterSynthesized));
        return constantMover.addConstantsToExpression(send->loc, move(method));
    }

    if (send->args.size() != 1) {
        return nullptr;
    }
    auto &arg = send->args[0];
    auto argString = to_s(ctx, arg);

    if (send->fun == core::Names::describe()) {
        ast::ClassDef::ANCESTORS_store ancestors;
        ancestors.emplace_back(ast::MK::Self(arg->loc));
        ast::ClassDef::RHS_store rhs;
        rhs.emplace_back(prepareBody(ctx, std::move(send->block->body)));
        auto name = ast::MK::UnresolvedConstant(arg->loc, ast::MK::EmptyTree(),
                                                ctx.state.enterNameConstant("<describe '" + argString + "'>"));
        return ast::MK::Class(send->loc, send->loc, std::move(name), std::move(ancestors), std::move(rhs));
    } else if (send->fun == core::Names::it()) {
        ConstantMover constantMover;
        send->block->body = ast::TreeMap::apply(ctx, constantMover, move(send->block->body));
        auto name = ctx.state.enterNameUTF8("<it '" + argString + "'>");
        auto method = addSigVoid(ast::MK::Method0(send->loc, send->loc, std::move(name),
                                                  prepareBody(ctx, std::move(send->block->body)),
                                                  ast::MethodDef::RewriterSynthesized));
        return constantMover.addConstantsToExpression(send->loc, move(method));
    }

    return nullptr;
}

unique_ptr<ast::Expression> recurse(core::MutableContext ctx, unique_ptr<ast::Expression> body) {
    auto bodySend = ast::cast_tree<ast::Send>(body.get());
    if (bodySend) {
        auto change = runSingle(ctx, bodySend);
        if (change) {
            return change;
        }
    }
    return body;
}

vector<unique_ptr<ast::Expression>> Minitest::run(core::MutableContext ctx, ast::Send *send) {
    vector<unique_ptr<ast::Expression>> stats;
    if (ctx.state.runningUnderAutogen) {
        return stats;
    }

    auto exp = runSingle(ctx, send);
    if (exp != nullptr) {
        stats.emplace_back(std::move(exp));
    }
    return stats;
}

}; // namespace sorbet::rewriter
