/**
Copyright (c) 2011 Microsoft Corporation

Module Name:

    spacer_util.cpp

Abstract:

    Utility functions for SPACER.


Author:

    Krystof Hoder (t-khoder) 2011-8-19.
    Arie Gurfinkel
    Anvesh Komuravelli

Revision History:

    Modified by Anvesh Komuravelli

Notes:


--*/

#include <algorithm>
#include <sstream>

#include "ast/arith_decl_plugin.h"
#include "ast/array_decl_plugin.h"
#include "ast/ast.h"
#include "ast/ast_pp.h"
#include "ast/bv_decl_plugin.h"
#include "ast/datatype_decl_plugin.h"
#include "ast/expr_functors.h"
#include "ast/for_each_expr.h"
#include "ast/occurs.h"
#include "ast/rewriter/bool_rewriter.h"
#include "ast/rewriter/expr_replacer.h"
#include "ast/rewriter/expr_safe_replace.h"
#include "ast/rewriter/factor_equivs.h"
#include "ast/rewriter/rewriter.h"
#include "ast/rewriter/rewriter_def.h"
#include "ast/scoped_proof.h"
#include "util/util.h"

#include "model/model.h"
#include "model/model_evaluator.h"
#include "model/model_pp.h"
#include "model/model_smt2_pp.h"
#include "params/smt_params.h"

#include "qe/lite/qel.h"
#include "qe/mbp/mbp_plugin.h"
#include "qe/mbp/mbp_term_graph.h"
#include "qe/qe_mbp.h"

#include "tactic/arith/arith_bounds_tactic.h"
#include "tactic/arith/propagate_ineqs_tactic.h"
#include "tactic/core/propagate_values_tactic.h"
#include "tactic/tactical.h"

#include "muz/base/dl_util.h"
#include "muz/spacer/spacer_legacy_mev.h"
#include "muz/spacer/spacer_manager.h"
#include "muz/spacer/spacer_qe_project.h"
#include "muz/spacer/spacer_util.h"

namespace spacer {

class contains_def_pred : public i_expr_pred {
        array_util m_autil;
    public:
        contains_def_pred(ast_manager& m): m_autil(m) {}
        bool operator()(expr* e) override {
            return m_autil.is_default(e);
        }
};

bool contains_defaults(expr *fml, ast_manager &m) {
    contains_def_pred pred(m);
    check_pred check(pred, m, false);
    return check(fml);
}

bool is_clause(ast_manager &m, expr *n) {
    if (spacer::is_literal(m, n)) return true;
    if (m.is_or(n)) {
        for (expr *arg : *to_app(n)) {
            if (!spacer::is_literal(m, arg)) return false;
            return true;
        }
    }
    return false;
}

bool is_literal(ast_manager &m, expr *n) {
    return spacer::is_atom(m, n) ||
           (m.is_not(n) && spacer::is_atom(m, to_app(n)->get_arg(0)));
}

bool is_atom(ast_manager &m, expr *n) {
    if (is_quantifier(n) || !m.is_bool(n)) return false;
    if (is_var(n)) return true;
    SASSERT(is_app(n));
    if (to_app(n)->get_family_id() != m.get_basic_family_id()) { return true; }

    if ((m.is_eq(n) && !m.is_bool(to_app(n)->get_arg(0))) || m.is_true(n) ||
        m.is_false(n))
        return true;

    // x=y is atomic if x and y are Bool and atomic
    expr *e1, *e2;
    if (m.is_eq(n, e1, e2) && spacer::is_atom(m, e1) && spacer::is_atom(m, e2))
        return true;
    return false;
}

void subst_vars(ast_manager &m, app_ref_vector const &vars, model &mdl,
                expr_ref &fml) {
    model::scoped_model_completion _sc_(mdl, true);
    expr_safe_replace sub(m);
    for (app *v : vars) sub.insert(v, mdl(v));
    sub(fml);
}

void to_mbp_benchmark(std::ostream &out, expr *fml,
                      const app_ref_vector &vars) {
    ast_manager &m = vars.m();
    ast_pp_util pp(m);
    pp.collect(fml);
    pp.display_decls(out);

    out << "(define-fun mbp_benchmark_fml () Bool\n  ";
    out << mk_pp(fml, m) << ")\n\n";

    out << "(push 1)\n"
        << "(assert mbp_benchmark_fml)\n"
        << "(check-sat)\n"
        << "(mbp mbp_benchmark_fml (";
    for (auto v : vars) { out << mk_pp(v, m) << " "; }
    out << "))\n"
        << "(pop 1)\n"
        << "(exit)\n";
}

void qe_project_z3(ast_manager &m, app_ref_vector &vars, expr_ref &fml,
                   model &mdl, bool reduce_all_selects, bool use_native_mbp,
                   bool dont_sub) {
    params_ref p;
    p.set_bool("reduce_all_selects", reduce_all_selects);
    p.set_bool("dont_sub", dont_sub);
    TRACE(qe, tout << "qe-project-z3\n");

    qe::mbproj mbp(m, p);
    mbp.spacer(vars, mdl, fml);
}

/*
 * eliminate simple equalities using qe_lite
 * then, MBP for Booleans (substitute), reals (based on LW), ints (based on
 * Cooper), and arrays
 */
void qe_project_spacer(ast_manager &m, app_ref_vector &vars, expr_ref &fml,
                       model &mdl, bool reduce_all_selects, bool use_native_mbp,
                       bool dont_sub) {
    th_rewriter rw(m);
    TRACE(spacer_mbp, tout << "Before projection:\n"; tout << fml << "\n";
          tout << "Vars:" << vars << "\n";);

    {
        // Ensure that top-level AND of fml is flat
        expr_ref_vector flat(m);
        flatten_and(fml, flat);
        fml = mk_and(flat);
    }

    // uncomment for benchmarks
    // to_mbp_benchmark(verbose_stream(), fml, vars);

    app_ref_vector arith_vars(m);
    app_ref_vector array_vars(m);
    app_ref_vector other_vars(m);
    array_util arr_u(m);
    arith_util ari_u(m);
    expr_safe_replace bool_sub(m);
    expr_ref bval(m);

    while (true) {
        params_ref p;
        qel qe(m, p);
        qe(vars, fml);
        rw(fml);

        TRACE(spacer_mbp, tout << "After qe_lite:\n";
              tout << mk_pp(fml, m) << "\nVars:" << vars << "\n";);

        SASSERT(!m.is_false(fml));

        // sort out vars into bools, arith (int/real), and arrays
        for (app *v : vars) {
            if (m.is_bool(v)) {
                // obtain the interpretation of the ith var
                // using model completion
                model::scoped_model_completion _sc_(mdl, true);
                bool_sub.insert(v, mdl(v));
            }
            else if (arr_u.is_array(v)) 
                array_vars.push_back(v);
            else if (ari_u.is_int(v) || ari_u.is_real(v)) 
                arith_vars.push_back(v);
            else
                other_vars.push_back(v);
        }

        // substitute Booleans
        if (!bool_sub.empty()) {
            bool_sub(fml);
            // -- bool_sub is not simplifying
            rw(fml);
            SASSERT(!m.is_false(fml));
            TRACE(spacer_mbp, tout << "Projected Booleans:\n" << fml << "\n";);
            bool_sub.reset();
        }

        TRACE(spacer_mbp, tout << "Array vars:\n"; tout << array_vars;);

        vars.reset();

        // project arrays
        if (!array_vars.empty()) {
            scoped_no_proof _sp(m);
            // -- local rewriter that is aware of current proof mode
            th_rewriter srw(m);
            spacer_qe::array_project(mdl, array_vars, fml, vars,
                                     reduce_all_selects);
            SASSERT(array_vars.empty());
            srw(fml);
            SASSERT(!m.is_false(fml));
        }

        TRACE(spacer_mbp, tout << "extended model:\n"; model_pp(tout, mdl);
              tout << "Auxiliary variables of index and value sorts:\n";
              tout << vars << "\n";);

        if (vars.empty())
            break;        
    }

    // project reals and ints
    if (!arith_vars.empty()) {
        TRACE(spacer_mbp, tout << "Arith vars:" << arith_vars << "\n";);

        if (use_native_mbp) {
            qe::mbproj mbp(m);
            expr_ref_vector fmls(m);
            flatten_and(fml, fmls);

            mbp(true, arith_vars, mdl, fmls);
            fml = mk_and(fmls);
            SASSERT(arith_vars.empty());
        }
        else {
            scoped_no_proof _sp(m);
            spacer_qe::arith_project(mdl, arith_vars, fml);
        }

        TRACE(spacer_mbp, tout << "Projected arith vars: "<< fml << "\n";
              tout << "Remaining arith vars:" << arith_vars << "\n";);
        SASSERT(!m.is_false(fml));
    }

    if (!arith_vars.empty())
        mbqi_project(mdl, arith_vars, fml); 

    // substitute any remaining arith vars
    if (!dont_sub && !arith_vars.empty()) {
        subst_vars(m, arith_vars, mdl, fml);
        TRACE(spacer_mbp,
              tout << "After substituting remaining arith vars:\n";
              tout << mk_pp(fml, m) << "\n";);
        // an extra round of simplification because subst_vars is not
        // simplifying
        rw(fml);
    }

    DEBUG_CODE(model_evaluator mev(mdl); mev.set_model_completion(false);
               SASSERT(mev.is_true(fml)););

    vars.reset();
    vars.append(other_vars);
    if (dont_sub && !arith_vars.empty())
        vars.append(arith_vars);
    TRACE(qe, tout << "after projection: " << fml << ": " << vars << "\n");
}

static expr *apply_accessor(ast_manager &m, ptr_vector<func_decl> const &acc,
                            unsigned j, func_decl *f, expr *c) {
    if (is_app(c) && to_app(c)->get_decl() == f) 
        return to_app(c)->get_arg(j);
    else 
        return m.mk_app(acc[j], c);
}

void qe_project(ast_manager &m, app_ref_vector &vars, expr_ref &fml, model &mdl,
                bool reduce_all_selects, bool use_native_mbp, bool dont_sub) {
    if (!use_native_mbp) 
        qe_project_spacer(m, vars, fml, mdl, reduce_all_selects, use_native_mbp,
                          dont_sub);

    if (!vars.empty())
        qe_project_z3(m, vars, fml, mdl, reduce_all_selects, use_native_mbp,
                      dont_sub);        

}

void expand_literals(ast_manager &m, expr_ref_vector &conjs) {
    if (conjs.empty()) return;
    arith_util arith(m);
    datatype_util dt(m);
    bv_util bv(m);
    expr *e1, *e2, *c, *val;
    rational r;
    unsigned bv_size;

    TRACE(spacer_expand, tout << "begin expand\n" << conjs << "\n";);

    for (unsigned i = 0; i < conjs.size(); ++i) {
        expr *e = conjs[i].get();
        if (m.is_eq(e, e1, e2) && arith.is_int_real(e1) && !arith.is_mod(e1) &&
            !arith.is_mod(e2)) {
            conjs[i] = arith.mk_le(e1, e2);
            if (i + 1 == conjs.size()) {
                conjs.push_back(arith.mk_ge(e1, e2));
            }
            else {
                conjs.push_back(conjs[i + 1].get());
                conjs[i + 1] = arith.mk_ge(e1, e2);
            }
            ++i;
        }
        else if ((m.is_eq(e, c, val) && is_app(val) &&
                    dt.is_constructor(to_app(val))) ||
                   (m.is_eq(e, val, c) && is_app(val) &&
                    dt.is_constructor(to_app(val)))) {
            func_decl *f = to_app(val)->get_decl();
            func_decl *r = dt.get_constructor_is(f);
            conjs[i] = m.mk_app(r, c);
            ptr_vector<func_decl> const &acc = *dt.get_constructor_accessors(f);
            for (unsigned j = 0; j < acc.size(); ++j) {
                conjs.push_back(m.mk_eq(apply_accessor(m, acc, j, f, c),
                                        to_app(val)->get_arg(j)));
            }
        }
        else if ((m.is_eq(e, c, val) && bv.is_numeral(val, r, bv_size)) ||
                 (m.is_eq(e, val, c) && bv.is_numeral(val, r, bv_size))) {
            rational two(2);
            for (unsigned j = 0; j < bv_size; ++j) {
                parameter p(j);
                expr *e = m.mk_eq(m.mk_app(bv.get_family_id(), OP_BIT1),
                                  bv.mk_extract(j, j, c));
                if ((r % two).is_zero()) { e = m.mk_not(e); }
                r = div(r, two);
                if (j == 0) 
                    conjs[i] = e;
                else 
                    conjs.push_back(e);
            }
        }
    }
    TRACE(spacer_expand, tout << "end expand\n" << conjs << "\n";);
}

namespace {
class implicant_picker {
    model &m_model;
    ast_manager &m;
    arith_util m_arith;

    expr_ref_vector m_todo;
    expr_mark m_visited;

    // add literal to the implicant
    // applies lightweight normalization
    void add_literal(expr *e, expr_ref_vector &out) {
        SASSERT(m.is_bool(e));

        expr_ref res(m), v(m);
        v = m_model(e);
        // the literal must have a value
        SASSERT(m.limit().is_canceled() || m.is_true(v) || m.is_false(v));

        res = m.is_false(v) ? m.mk_not(e) : e;

        if (m.is_distinct(res)) {
            // --(distinct a b) == (not (= a b))
            if (to_app(res)->get_num_args() == 2) {
                res = m.mk_eq(to_app(res)->get_arg(0), to_app(res)->get_arg(1));
                res = m.mk_not(res);
            }
        }

        expr *nres = nullptr, *f1 = nullptr, *f2 = nullptr;
        if (m.is_not(res, nres)) {
            // --(not (xor a b)) == (= a b)
            if (m.is_xor(nres, f1, f2)) res = m.mk_eq(f1, f2);
            // -- split arithmetic inequality
            else if (m.is_eq(nres, f1, f2) && m_arith.is_int_real(f1)) {
                res = m_arith.mk_lt(f1, f2);
                if (!m_model.is_true(res)) res = m_arith.mk_lt(f2, f1);
            }
        }

        if (!m_model.is_true(res)) {
            IF_VERBOSE(2, verbose_stream()
                              << "(spacer-model-anomaly: " << res << ")\n");
        }
        out.push_back(res);
    }

    void process_app(app *a, expr_ref_vector &out) {
        if (m_visited.is_marked(a)) return;
        SASSERT(m.is_bool(a));
        expr_ref v(m);
        v = m_model(a);
        bool is_true = m.is_true(v);

        if (!is_true && !m.is_false(v)) return;

        expr *na = nullptr, *f1 = nullptr, *f2 = nullptr, *f3 = nullptr;

        if (m.is_true(a)|| m.is_false(a)) {
            // noop
        } else if (a->get_family_id() != m.get_basic_family_id()) {
            add_literal(a, out);
        } else if (is_uninterp_const(a)) {
            add_literal(a, out);
        } else if (m.is_not(a, na)) {
            m_todo.push_back(na);
        } else if (m.is_distinct(a)) {
            if (!is_true) {
                expr_ref tmp =
                    mbp::project_plugin::pick_equality(m, m_model, a);
                m_todo.push_back(tmp);
            } else if (a->get_num_args() == 2) {
                add_literal(a, out);
            } else {
                m_todo.push_back(
                    m.mk_distinct_expanded(a->get_num_args(), a->get_args()));
            }
        } else if (m.is_and(a)) {
            if (is_true) {
                m_todo.append(a->get_num_args(), a->get_args());
            } else {
                for (expr *e : *a) {
                    if (m_model.is_false(e)) {
                        m_todo.push_back(e);
                        break;
                    }
                }
            }
        } else if (m.is_or(a)) {
            if (!is_true)
                m_todo.append(a->get_num_args(), a->get_args());
            else {
                for (expr *e : *a) {
                    if (m_model.is_true(e)) {
                        m_todo.push_back(e);
                        break;
                    }
                }
            }
        } else if (m.is_eq(a, f1, f2) ||
                   (is_true && m.is_not(a, na) && m.is_xor(na, f1, f2))) {
            if (!m.are_equal(f1, f2) && !m.are_distinct(f1, f2)) {
                if (m.is_bool(f1) &&
                    (!is_uninterp_const(f1) || !is_uninterp_const(f2)))
                    m_todo.append(a->get_num_args(), a->get_args());
                else
                    add_literal(a, out);
            }
        } else if (m.is_ite(a, f1, f2, f3)) {
            if (m.are_equal(f2, f3)) {
                m_todo.push_back(f2);
            } else if (m_model.is_true(f2) && m_model.is_true(f3)) {
                m_todo.push_back(f2);
                m_todo.push_back(f3);
            } else if (m_model.is_false(f2) && m_model.is_false(f3)) {
                m_todo.push_back(f2);
                m_todo.push_back(f3);
            } else if (m_model.is_true(f1)) {
                m_todo.push_back(f1);
                m_todo.push_back(f2);
            } else if (m_model.is_false(f1)) {
                m_todo.push_back(f1);
                m_todo.push_back(f3);
            }
        } else if (m.is_xor(a, f1, f2)) {
            m_todo.append(a->get_num_args(), a->get_args());
        } else if (m.is_implies(a, f1, f2)) {
            if (is_true) {
                if (m_model.is_true(f2))
                    m_todo.push_back(f2);
                else if (m_model.is_false(f1))
                    m_todo.push_back(f1);
            } else
                m_todo.append(a->get_num_args(), a->get_args());
        } else {
            IF_VERBOSE(0, verbose_stream() << "Unexpected expression: "
                                           << mk_pp(a, m) << "\n");
            UNREACHABLE();
        }
    }

    void pick_literals(expr *e, expr_ref_vector &out) {
        SASSERT(m_todo.empty());
        if (m_visited.is_marked(e) || !is_app(e)) return;

        // -- keep track of all created expressions to
        // -- make sure that expression ids are stable
        expr_ref_vector pinned(m);

        m_todo.reset();
        m_todo.push_back(e);
        while (!m_todo.empty()) {
            pinned.push_back(m_todo.back());
            m_todo.pop_back();
            if (!is_app(pinned.back())) continue;
            app *a = to_app(pinned.back());
            process_app(a, out);
            m_visited.mark(a, true);
        }
        m_todo.reset();
    }

    bool pick_implicant(const expr_ref_vector &in, expr_ref_vector &out) {
        m_visited.reset();
        bool is_true = m_model.is_true(in);

        for (expr *e : in) {
            if (is_true || m_model.is_true(e)) { pick_literals(e, out); }
        }
        m_visited.reset();
        return is_true;
    }

  public:
    implicant_picker(model &mdl)
        : m_model(mdl), m(m_model.get_manager()), m_arith(m), m_todo(m) {}

    void operator()(expr_ref_vector &in, expr_ref_vector &out) {
        model::scoped_model_completion _sc_(m_model, false);
        pick_implicant(in, out);
    }
};
} // namespace

expr_ref_vector compute_implicant_literals(model &mdl,
                                           expr_ref_vector &formula) {
    // flatten the formula and remove all trivial literals

    // TBD: not clear why there is a dependence on it(other than
    // not handling of Boolean constants by implicant_picker), however,
    // it was a source of a problem on a benchmark
    expr_ref_vector res(formula.get_manager());
    flatten_and(formula);
    if (!formula.empty()) {
        implicant_picker ipick(mdl);
        ipick(formula, res);
    }
    return res;
}

void simplify_bounds_old(expr_ref_vector &cube) {
    ast_manager &m = cube.m();
    scoped_no_proof _no_pf_(m);
    goal_ref g(alloc(goal, m, false, false, false));
    for (expr *c : cube) g->assert_expr(c);

    goal_ref_buffer result;
    tactic_ref simplifier = mk_arith_bounds_tactic(m);
    (*simplifier)(g, result);
    SASSERT(result.size() == 1);
    goal *r = result[0];
    cube.reset();
    for (unsigned i = 0; i < r->size(); ++i) { cube.push_back(r->form(i)); }
}

void simplify_bounds_new(expr_ref_vector &cube) {
    ast_manager &m = cube.m();
    scoped_no_proof _no_pf_(m);
    goal_ref g(alloc(goal, m, false, false, false));
    for (expr *c : cube) g->assert_expr(c);

    goal_ref_buffer goals;
    tactic_ref prop_values = mk_propagate_values_tactic(m);
    tactic_ref prop_bounds = mk_propagate_ineqs_tactic(m);
    tactic_ref t = and_then(prop_values.get(), prop_bounds.get());

    (*t)(g, goals);
    SASSERT(goals.size() == 1);

    g = goals[0];
    cube.reset();
    for (unsigned i = 0; i < g->size(); ++i) { cube.push_back(g->form(i)); }
}

void simplify_bounds(expr_ref_vector &cube) { simplify_bounds_new(cube); }

/// Adhoc rewriting of arithmetic expressions
struct adhoc_rewriter_cfg : public default_rewriter_cfg {
    ast_manager &m;
    arith_util m_arith;

    adhoc_rewriter_cfg(ast_manager &manager) : m(manager), m_arith(m) {}

    bool is_le(func_decl const *n) const { return m_arith.is_le(n); }
    bool is_ge(func_decl const *n) const { return m_arith.is_ge(n); }

    br_status reduce_app(func_decl *f, unsigned num, expr *const *args,
                         expr_ref &result, proof_ref &result_pr) {
        expr *e;
        if (is_le(f)) return mk_le_core(args[0], args[1], result);
        if (is_ge(f)) return mk_ge_core(args[0], args[1], result);
        if (m.is_not(f) && m.is_not(args[0], e)) {
            result = e;
            return BR_DONE;
        }
        return BR_FAILED;
    }

    br_status mk_le_core(expr *arg1, expr *arg2, expr_ref &result) {
        // t <= -1  ==> t < 0 ==> !(t >= 0)
        if (m_arith.is_int(arg1) && m_arith.is_minus_one(arg2)) {
            result = m.mk_not(m_arith.mk_ge(arg1, mk_zero()));
            return BR_DONE;
        }
        return BR_FAILED;
    }
    br_status mk_ge_core(expr *arg1, expr *arg2, expr_ref &result) {
        // t >= 1 ==> t > 0 ==> !(t <= 0)
        if (m_arith.is_int(arg1) && is_one(arg2)) {

            result = m.mk_not(m_arith.mk_le(arg1, mk_zero()));
            return BR_DONE;
        }
        return BR_FAILED;
    }
    expr *mk_zero() { return m_arith.mk_numeral(rational(0), true); }
    bool is_one(expr const *n) const {
        rational val;
        return m_arith.is_numeral(n, val) && val.is_one();
    }
};

bool is_normalized(expr_ref e, bool use_simplify_bounds, bool use_factor_eqs) {
    expr_ref out(e.m());
    normalize(e, out, use_simplify_bounds, use_factor_eqs);

    expr_ref out0 = out;
    if (e != out) { normalize(out, out, use_simplify_bounds, use_factor_eqs); }

    CTRACE(inherit_bug, e != out,
           tout << "e==out0: " << (e == out0) << " e==out: " << (e == out)
                << " out0==out: " << (out0 == out) << "\n";
           tout << "e: " << e << "\n"
                << "out0: " << out0 << "\n"
                << "out: " << out << "\n";);
    return e == out;
}
void normalize(expr *e, expr_ref &out, bool use_simplify_bounds,
               bool use_factor_eqs) {

    ast_manager &m = out.m();
    params_ref params;
    // arith_rewriter
    params.set_bool("sort_sums", true);
    params.set_bool("gcd_rounding", true);
    // params.set_bool("arith_lhs", true);
     params.set_bool("arith_ineq_lhs", true);
    // poly_rewriter
    params.set_bool("som", true);
    params.set_bool("flat", true);

    // apply rewriter
    th_rewriter rw(m, params);
    rw(e, out);

    // adhoc_rewriter_cfg adhoc_cfg(m);
    // rewriter_tpl<adhoc_rewriter_cfg> adhoc_rw(m, false, adhoc_cfg);
    // adhoc_rw(out.get(), out);

    if (m.is_and(out)) {
        expr_ref_vector v(m);
        flatten_and(out, v);

        if (v.size() > 1) {
            if (use_simplify_bounds) {
                // remove redundant inequalities
                simplify_bounds(v);
            }
            if (use_factor_eqs) {
                // -- refactor equivalence classes and choose a representative
                mbp::term_graph egraph(out.m());
                egraph.add_lits(v);
                v.reset();
                egraph.to_lits(v);
            }
            // sort arguments of the top-level and
            std::stable_sort(v.data(), v.data() + v.size(), ast_lt_proc());

            TRACE(spacer_normalize, tout << "Normalized:\n"
                                           << out << "\n"
                                           << "to\n"
                                           << mk_and(v) << "\n";);
            TRACE(spacer_normalize, {
                mbp::term_graph egraph(m);
                for (expr *e : v) egraph.add_lit(to_app(e));
                tout << "Reduced app:\n" << mk_pp(egraph.to_expr(), m) << "\n";
            });
            out = mk_and(v);
        }
    }

    // normalize_order(out, out);
}

// rewrite term such that the pretty printing is easier to read
struct adhoc_rewriter_rpp : public default_rewriter_cfg {
    ast_manager &m;
    arith_util m_arith;

    adhoc_rewriter_rpp(ast_manager &manager) : m(manager), m_arith(m) {}

    bool is_le(func_decl const *n) const { return m_arith.is_le(n); }
    bool is_ge(func_decl const *n) const { return m_arith.is_ge(n); }
    bool is_lt(func_decl const *n) const { return m_arith.is_lt(n); }
    bool is_gt(func_decl const *n) const { return m_arith.is_gt(n); }
    bool is_zero(expr const *n) const {
        rational val;
        return m_arith.is_numeral(n, val) && val.is_zero();
    }

    br_status reduce_app(func_decl *f, unsigned num, expr *const *args,
                         expr_ref &result, proof_ref &result_pr) {
        br_status st = BR_FAILED;
        expr *e1, *e2, *e3, *e4;

        // rewrites(=(+ A(* -1 B)) 0) into(= A B)
        if (m.is_eq(f) && is_zero(args[1]) && m_arith.is_add(args[0], e1, e2) &&
            m_arith.is_mul(e2, e3, e4) && m_arith.is_minus_one(e3)) {
            result = m.mk_eq(e1, e4);
            return BR_DONE;
        }
        // simplify normalized leq, where right side is different from 0
        // rewrites(<=(+ A(* -1 B)) C) into(<= A B+C)
        else if ((is_le(f) || is_lt(f) || is_ge(f) || is_gt(f)) &&
                 m_arith.is_add(args[0], e1, e2) &&
                 m_arith.is_mul(e2, e3, e4) && m_arith.is_minus_one(e3)) {
            expr_ref rhs(m);
            rhs = is_zero(args[1]) ? e4 : m_arith.mk_add(e4, args[1]);

            if (is_le(f)) {
                result = m_arith.mk_le(e1, rhs);
                st = BR_DONE;
            } else if (is_lt(f)) {
                result = m_arith.mk_lt(e1, rhs);
                st = BR_DONE;
            } else if (is_ge(f)) {
                result = m_arith.mk_ge(e1, rhs);
                st = BR_DONE;
            } else if (is_gt(f)) {
                result = m_arith.mk_gt(e1, rhs);
                st = BR_DONE;
            } else {
                UNREACHABLE();
            }
        }
        // simplify negation of ordering predicate
        else if (m.is_not(f)) {
            if (m_arith.is_lt(args[0], e1, e2)) {
                result = m_arith.mk_ge(e1, e2);
                st = BR_DONE;
            } else if (m_arith.is_le(args[0], e1, e2)) {
                result = m_arith.mk_gt(e1, e2);
                st = BR_DONE;
            } else if (m_arith.is_gt(args[0], e1, e2)) {
                result = m_arith.mk_le(e1, e2);
                st = BR_DONE;
            } else if (m_arith.is_ge(args[0], e1, e2)) {
                result = m_arith.mk_lt(e1, e2);
                st = BR_DONE;
            }
        }
        return st;
    }
};

mk_epp::mk_epp(ast *t, ast_manager &m, unsigned indent, unsigned num_vars,
               char const *var_prefix)
    : mk_pp(t, m, m_epp_params, indent, num_vars, var_prefix), m_epp_expr(m) {
    m_epp_params.set_uint("min_alias_size", UINT_MAX);
    m_epp_params.set_uint("max_depth", UINT_MAX);

    if (is_expr(m_ast)) {
        rw(to_expr(m_ast), m_epp_expr);
        m_ast = m_epp_expr;
    }
}

void mk_epp::rw(expr *e, expr_ref &out) {
    adhoc_rewriter_rpp cfg(out.m());
    rewriter_tpl<adhoc_rewriter_rpp> arw(out.m(), false, cfg);
    arw(e, out);
}

void ground_expr(expr *e, expr_ref &out, app_ref_vector &vars) {
    expr_free_vars fv;
    ast_manager &m = out.m();

    fv(e);
    if (vars.size() < fv.size()) { vars.resize(fv.size()); }
    for (unsigned i = 0, sz = fv.size(); i < sz; ++i) {
        sort *s = fv[i] ? fv[i] : m.mk_bool_sort();
        vars[i] = mk_zk_const(m, i, s);
        var_subst vs(m, false);
        out = vs(e, vars.size(), (expr **)vars.data());
    }
}

struct index_term_finder {
    ast_manager &m;
    array_util m_array;
    app_ref m_var;
    expr_ref_vector &m_res;

    index_term_finder(ast_manager &mgr, app *v, expr_ref_vector &res)
        : m(mgr), m_array(m), m_var(v, m), m_res(res) {}
    void operator()(var *n) {}
    void operator()(quantifier *n) {}
    void operator()(app *n) {
        if (m_array.is_select(n) || m.is_eq(n)) {
            unsigned i = 0;
            for (expr *arg : *n) {
                if ((m.is_eq(n) || i > 0) && m_var != arg) m_res.push_back(arg);
                ++i;
            }
        }
    }
};

bool mbqi_project_var(model &mdl, app *var, expr_ref &fml) {
    ast_manager &m = fml.get_manager();
    model::scoped_model_completion _sc_(mdl, false);

    expr_ref val(m);
    val = mdl(var);

    TRACE(mbqi_project_verbose, tout << "MBQI: var: " << mk_pp(var, m) << "\n"
                                       << "fml: " << fml << "\n";);
    expr_ref_vector terms(m);
    index_term_finder finder(m, var, terms);
    for_each_expr(finder, fml);

    TRACE(mbqi_project_verbose, tout << "terms:\n" << terms << "\n";);

    for (expr *term : terms) {
        expr_ref tval(m);
        tval = mdl(term);

        TRACE(mbqi_project_verbose, tout << "term: " << mk_pp(term, m)
                                           << " tval: " << tval
                                           << " val: " << val << "\n";);

        // -- if the term does not contain an occurrence of var
        // -- and is in the same equivalence class in the model
        if (tval == val && !occurs(var, term)) {
            TRACE(mbqi_project, tout << "MBQI: replacing " << mk_pp(var, m)
                                       << " with " << mk_pp(term, m) << "\n";);
            expr_safe_replace sub(m);
            sub.insert(var, term);
            sub(fml);
            return true;
        }
    }

    TRACE(mbqi_project, tout << "MBQI: failed to eliminate " << mk_pp(var, m)
                               << " from " << fml << "\n";);

    return false;
}

void mbqi_project(model &mdl, app_ref_vector &vars, expr_ref &fml) {
    ast_manager &m = fml.get_manager();
    expr_ref tmp(m);
    model::scoped_model_completion _sc_(mdl, false);
    // -- evaluate to initialize mev cache
    tmp = mdl(fml);
    tmp.reset();

    unsigned j = 0;
    for (app *v : vars)
        if (!mbqi_project_var(mdl, v, fml)) vars[j++] = v;
    vars.shrink(j);
}

struct found {};
struct check_select {
    array_util a;
    check_select(ast_manager &m) : a(m) {}
    void operator()(expr *n) {}
    void operator()(app *n) {
        if (a.is_select(n)) throw found();
    }
};

bool contains_selects(expr *fml, ast_manager &m) {
    check_select cs(m);
    try {
        for_each_expr(cs, fml);
        return false;
    } catch (const found &) { return true; }
}

struct collect_indices {
    app_ref_vector &m_indices;
    array_util a;
    collect_indices(app_ref_vector &indices)
        : m_indices(indices), a(indices.get_manager()) {}
    void operator()(expr *n) {}
    void operator()(app *n) {
        if (a.is_select(n)) {
            // for all but first argument
            for (unsigned i = 1; i < n->get_num_args(); ++i) {
                expr *arg = n->get_arg(i);
                if (is_app(arg)) m_indices.push_back(to_app(arg));
            }
        }
    }
};

void get_select_indices(expr *fml, app_ref_vector &indices) {
    collect_indices ci(indices);
    for_each_expr(ci, fml);
}

struct collect_decls {
    app_ref_vector &m_decls;
    std::string &prefix;
    collect_decls(app_ref_vector &decls, std::string &p)
        : m_decls(decls), prefix(p) {}
    void operator()(expr *n) {}
    void operator()(app *n) {
        if (n->get_decl()->get_name().str().find(prefix) != std::string::npos)
            m_decls.push_back(n);
    }
};

void find_decls(expr *fml, app_ref_vector &decls, std::string &prefix) {
    collect_decls cd(decls, prefix);
    for_each_expr(cd, fml);
}

// set the value of a boolean function to true in model
void set_true_in_mdl(model &model, func_decl *f) {
    SASSERT(f->get_arity() == 0);
    model.unregister_decl(f);
    model.register_decl(f, model.get_manager().mk_true());
    model.reset_eval_cache();
}

// Return number of variables in \p e
unsigned get_num_vars(expr *e) {
    expr_free_vars fv;
    fv(e);
    unsigned count = 0;
    for (unsigned i = 0, sz = fv.size(); i < sz; ++i) {
        if (fv[i]) { count++; }
    }
    return count;
}

namespace collect_uninterp_consts_ns {
struct proc {
    expr_ref_vector &m_out;
    proc(expr_ref_vector &out) : m_out(out) {}
    void operator()(expr *n) const {}
    void operator()(app *n) {
        if (is_uninterp_const(n)) m_out.push_back(n);
    }
};
} // namespace collect_uninterp_consts_ns

// Return all uninterpreted constants of \p q
void collect_uninterp_consts(expr *e, expr_ref_vector &out) {
    collect_uninterp_consts_ns::proc proc(out);
    for_each_expr(proc, e);
}

namespace has_nonlinear_var_mul_ns {
struct found {};
// Detects multiplication of a variable by not-a-number
struct proc {
    arith_util m_arith;
    bv_util m_bv;
    proc(ast_manager &m) : m_arith(m), m_bv(m) {}
    bool is_numeral(expr *e) const {
        return m_arith.is_numeral(e) || m_bv.is_numeral(e);
    }
    bool is_mul(const expr *n, expr *&e1, expr *&e2) const {
        if (m_arith.is_mul(n, e1, e2)) return true;
        if (m_bv.is_bv_mul(n, e1, e2)) return true;
        return false;
    }
    void operator()(var *n) const {}
    void operator()(quantifier *q) const {}
    void operator()(app const *n) const {
        expr *e1, *e2;
        if (is_mul(n, e1, e2) && ((is_var(e1) && !is_numeral(e2)) ||
                                  (is_var(e2) && !is_numeral(e1))))
            throw found();
    }
};
} // namespace has_nonlinear_var_mul_ns

// Returns true if \p e contains a multiplication a variable by not-a-number
bool has_nonlinear_var_mul(expr *e, ast_manager &m) {
    has_nonlinear_var_mul_ns::proc proc(m);
    try {
        for_each_expr(proc, e);
    } catch (const has_nonlinear_var_mul_ns::found &) { return true; }
    return false;
}

namespace contains_mod_ns {
struct found {};
struct contains_mod_proc {
    ast_manager &m;
    arith_util m_arith;
    contains_mod_proc(ast_manager &a_m) : m(a_m), m_arith(m) {}
    void operator()(expr *n) const {}
    void operator()(app *n) {
        if (m_arith.is_mod(n)) throw found();
    }
};
} // namespace contains_mod_ns

// Returns true if \p e contains \p mod
bool contains_mod(expr *e, ast_manager &m) {
    contains_mod_ns::contains_mod_proc t(m);
    try {
        for_each_expr(t, e);
    } catch (const contains_mod_ns::found &) { return true; }

    return false;
}
bool contains_mod(const expr_ref &e) {
    return contains_mod(e.get(), e.get_manager());
}

namespace contains_real_ns {
struct found {};
struct contains_real_proc {
    ast_manager &m;
    arith_util m_arith;
    contains_real_proc(ast_manager &a_m) : m(a_m), m_arith(m) {}
    void operator()(expr *n) const {}
    void operator()(app *n) {
        if (m_arith.is_real(n)) throw found();
    }
};
} // namespace contains_real_ns

// Returns true if \p e contains a real-valued sub-term
bool contains_real(expr *e, ast_manager &m) {
    contains_real_ns::contains_real_proc t(m);
    try {
        for_each_expr(t, e);
        return false;
    } catch (const contains_real_ns::found &) { return true; }
}
bool contains_real(const expr_ref &e) {
    return contains_real(e.get(), e.get_manager());
}

/// Returns true if the range of substitution \p s is numeric
bool is_numeric_sub(const substitution &s) {
    ast_manager &m(s.get_manager());
    arith_util arith(m);
    bv_util bv(m);
    std::pair<unsigned, unsigned> var;
    expr_offset r;
    for (unsigned i = 0, sz = s.get_num_bindings(); i < sz; ++i) {
        s.get_binding(i, var, r);
        if (!(bv.is_numeral(r.get_expr()) || arith.is_numeral(r.get_expr())))
            return false;
    }
    return true;
}

} // namespace spacer
template class rewriter_tpl<spacer::adhoc_rewriter_cfg>;
template class rewriter_tpl<spacer::adhoc_rewriter_rpp>;
