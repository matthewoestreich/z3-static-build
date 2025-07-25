/*++
Copyright (c) 2016 Microsoft Corporation

Module Name:

    model_based_opt.cpp

Abstract:

    Model-based optimization and projection for linear real, integer arithmetic.

Author:

    Nikolaj Bjorner (nbjorner) 2016-27-4

Revision History:


--*/

#include "math/simplex/model_based_opt.h"
#include "util/uint_set.h"
#include "util/z3_exception.h"

std::ostream& operator<<(std::ostream& out, opt::ineq_type ie) {
    switch (ie) {
    case opt::t_eq: return out << " = ";
    case opt::t_lt: return out << " < ";
    case opt::t_le: return out << " <= ";        
    case opt::t_divides: return out << " divides ";
    case opt::t_mod: return out << " mod ";
    case opt::t_div: return out << " div ";
    }
    return out;
}


namespace opt {

    /**
     * Convert a row ax + coeffs + coeff = value into a definition for x
     *    x  = (value - coeffs - coeff)/a 
     * as backdrop we have existing assignments to x and other variables that 
     * satisfy the equality with value, and such that value satisfies
     * the row constraint ( = , <= , < , mod)
     */
    model_based_opt::def* model_based_opt::def::from_row(row const& r, unsigned x) {
        rational div(1), lc(denominator(r.m_coeff));

        for (var const & v : r.m_vars) {
            lc = lcm(lc, denominator(v.m_coeff));
            if (v.m_id == x) {
                div = -v.m_coeff;
                break;
            }    
        }   
        div *= lc;
        bool sign = div < 0;
        auto coeff = lc * r.m_coeff;
        switch (r.m_type) {
        case opt::t_lt: 
            coeff += div;
            break;
        case opt::t_le:
            // for: ax <= t, then x := (t + a - 1) div a
            if (!sign) {
                coeff += div;
                coeff -= rational::one();
            }
            break;
        default:
            break;
        }

        if (div < 0) {
            sign = true;
            div.neg();
            lc.neg();
            coeff.neg();
        }
        def* result = alloc(const_def, coeff);
        for (var const& v : r.m_vars) {
            if (v.m_id != x)
                result = *result + *alloc(var_def, v * lc);
        }
        if (div > 1) 
            result = *result / div;  
        return result;
    }
    void model_based_opt::def::dec_ref() {
        SASSERT(m_ref_count > 0);
        ++m_ref_count;
        if (m_ref_count == 0) 
            dealloc(this);            
    }

    model_based_opt::def* model_based_opt::def::operator+(def& other) {
        return alloc(add_def, this, &other);
    }
    model_based_opt::def* model_based_opt::def::operator*(def& other) {
        return alloc(mul_def, this, &other);
    }
    model_based_opt::def* model_based_opt::def::operator/(rational const& r) {
        if (r == 1)
            return this;
        return alloc(div_def, this, r);
    }
    model_based_opt::def* model_based_opt::def::operator*(rational const& n) {
        if (n == 1)
            return this;
        return alloc(mul_def, this, alloc(const_def, n));
    }
    model_based_opt::def* model_based_opt::def::operator+(rational const& n) {
        if (n == 0)
            return this;
        return alloc(add_def, this, alloc(const_def, n));
    }
    model_based_opt::add_def& model_based_opt::def::to_add() {
        return *static_cast<add_def*>(this);
    }
    model_based_opt::mul_def& model_based_opt::def::to_mul() {
        return *static_cast<mul_def*>(this);
    }
    model_based_opt::div_def& model_based_opt::def::to_div() {
        return *static_cast<div_def*>(this);
    }
    model_based_opt::var_def& model_based_opt::def::to_var() {
        return *static_cast<var_def*>(this);
    }
    model_based_opt::const_def& model_based_opt::def::to_const() {
        return *static_cast<const_def*>(this);
    }
    model_based_opt::add_def const& model_based_opt::def::to_add() const {
        return *static_cast<add_def const*>(this);
    }
    model_based_opt::mul_def const& model_based_opt::def::to_mul() const {
        return *static_cast<mul_def const*>(this);
    }
    model_based_opt::div_def const& model_based_opt::def::to_div() const {
        return *static_cast<div_def const*>(this);
    }
    model_based_opt::var_def const& model_based_opt::def::to_var() const {
        return *static_cast<var_def const*>(this);
    }
    model_based_opt::const_def const& model_based_opt::def::to_const() const {
        return *static_cast<const_def const*>(this);
    }



    /**
         a1*x1 + a2*x2 + a3*x3 + coeff1 / c1
         x2 |-> b1*x1 + b4*x4 + ceoff2 / c2
         ------------------------------------------------------------------------
         (a1*x1 + a2*((b1*x1 + b4*x4 + coeff2) / c2) + a3*x3 + coeff1) / c1
         ------------------------------------------------------------------------
         (c2*a1*x1 + a2*b1*x1 + a2*b4*x4 + c2*a3*x3 + c2*coeff1 + coeff2) / c1*c2
     */
    model_based_opt::def* model_based_opt::def::substitute(unsigned v, def& other) {
        if (is_add()) {
            auto x = to_add().x->substitute(v, other);
            auto y = to_add().y->substitute(v, other);
            if (x == to_add().x && y == to_add().y)
                return this;
            return *x + *y;
        }
        if (is_mul()) {
            auto x = to_mul().x->substitute(v, other);
            auto y = to_mul().y->substitute(v, other);
            if (x == to_mul().x && y == to_mul().y)
                return this;
            return *x * *y;
        }
        if (is_div()) {
            auto x = to_div().x->substitute(v, other);
            if (x == to_div().x)
                return this;
            return *x / to_div().m_div;
        }            
        if (is_var()) {
            if (to_var().v.m_id != v)
                return this;
            if (to_var().v.m_coeff == 1)
                return &other;
            return other * to_var().v.m_coeff;
        }
        if (is_const())
            return this;
        UNREACHABLE();
        return this;
    }

    model_based_opt::model_based_opt() {
        m_rows.push_back(row());
    }
        
    bool model_based_opt::invariant() {
        for (unsigned i = 0; i < m_rows.size(); ++i) {
            if (!invariant(i, m_rows[i])) {
                return false;
            }
        }
        return true;
    }

#define PASSERT(_e_) { CTRACE(qe, !(_e_), display(tout, r); display(tout);); SASSERT(_e_); }

    bool model_based_opt::invariant(unsigned index, row const& r) {
        vector<var> const& vars = r.m_vars;
        for (unsigned i = 0; i < vars.size(); ++i) {
            // variables in each row are sorted and have non-zero coefficients
            PASSERT(i + 1 == vars.size() || vars[i].m_id < vars[i+1].m_id);
            PASSERT(!vars[i].m_coeff.is_zero());
            PASSERT(index == 0 || m_var2row_ids[vars[i].m_id].contains(index));
        }
        
        PASSERT(r.m_value == eval(r));
        PASSERT(r.m_type != t_eq ||  r.m_value.is_zero());
        // values satisfy constraints
        PASSERT(index == 0 || r.m_type != t_lt ||  r.m_value.is_neg());
        PASSERT(index == 0 || r.m_type != t_le || !r.m_value.is_pos());        
        PASSERT(index == 0 || r.m_type != t_divides || (mod(r.m_value, r.m_mod).is_zero()));
        PASSERT(index == 0 || r.m_type != t_mod || r.m_id < m_var2value.size());
        PASSERT(index == 0 || r.m_type != t_div || r.m_id < m_var2value.size());
        return true;
    }
        
    // a1*x + obj 
    // a2*x + t2 <= 0
    // a3*x + t3 <= 0
    // a4*x + t4 <= 0
    // a1 > 0, a2 > 0, a3 > 0, a4 < 0
    // x <= -t2/a2
    // x <= -t2/a3
    // determine lub among these.
    // then resolve lub with others
    // e.g., -t2/a2 <= -t3/a3, then 
    // replace inequality a3*x + t3 <= 0 by -t2/a2 + t3/a3 <= 0
    // mark a4 as invalid.
    // 

    // a1 < 0, a2 < 0, a3 < 0, a4 > 0
    // x >= t2/a2
    // x >= t3/a3
    // determine glb among these
    // the resolve glb with others.
    // e.g. t2/a2 >= t3/a3
    // then replace a3*x + t3 by t3/a3 - t2/a2 <= 0
    // 
    inf_eps model_based_opt::maximize() {
        SASSERT(invariant());
        unsigned_vector bound_trail, bound_vars;
        TRACE(opt, display(tout << "tableau\n"););
        while (!objective().m_vars.empty()) {
            var v = objective().m_vars.back();
            unsigned x = v.m_id;
            rational const& coeff = v.m_coeff;
            unsigned bound_row_index;
            rational bound_coeff;
            if (find_bound(x, bound_row_index, bound_coeff, coeff.is_pos())) {
                SASSERT(!bound_coeff.is_zero());
                TRACE(opt, display(tout << "update: " << v << " ", objective());
                      for (unsigned above : m_above) {
                          display(tout << "resolve: ", m_rows[above]);
                      });
                for (unsigned above : m_above) {
                    resolve(bound_row_index, bound_coeff, above, x);
                }
                for (unsigned below : m_below) {
                    resolve(bound_row_index, bound_coeff, below, x);
                }
                // coeff*x + objective <= ub
                // a2*x + t2 <= 0
                // => coeff*x <= -t2*coeff/a2
                // objective + t2*coeff/a2 <= ub

                mul_add(false, m_objective_id, - coeff/bound_coeff, bound_row_index);
                retire_row(bound_row_index);
                bound_trail.push_back(bound_row_index);
                bound_vars.push_back(x);
            }
            else {
                TRACE(opt, display(tout << "unbound: " << v << " ", objective()););
                update_values(bound_vars, bound_trail);
                return inf_eps::infinity();
            }
        }

        //
        // update the evaluation of variables to satisfy the bound.
        //

        update_values(bound_vars, bound_trail);

        rational value = objective().m_value;
        if (objective().m_type == t_lt) {            
            return inf_eps(inf_rational(value, rational(-1)));
        }
        else {
            return inf_eps(inf_rational(value));
        }
    }


    void model_based_opt::update_value(unsigned x, rational const& val) {
        rational old_val = m_var2value[x];
        m_var2value[x] = val;
        SASSERT(val.is_int() || !is_int(x));
        unsigned_vector const& row_ids = m_var2row_ids[x];
        for (unsigned row_id : row_ids) {
            rational coeff = get_coefficient(row_id, x);
            if (coeff.is_zero()) {
                continue;
            }
            row & r = m_rows[row_id];
            rational delta = coeff * (val - old_val);            
            r.m_value += delta;
            SASSERT(invariant(row_id, r));
        }
    }


    void model_based_opt::update_values(unsigned_vector const& bound_vars, unsigned_vector const& bound_trail) {
        for (unsigned i = bound_trail.size(); i-- > 0; ) {
            unsigned x = bound_vars[i];
            row& r = m_rows[bound_trail[i]];
            rational val = r.m_coeff;
            rational old_x_val = m_var2value[x];
            rational new_x_val;
            rational x_coeff, eps(0);
            vector<var> const& vars = r.m_vars;
            for (var const& v : vars) {                 
                if (x == v.m_id) {
                    x_coeff = v.m_coeff;
                }
                else {
                    val += m_var2value[v.m_id]*v.m_coeff;
                }
            }
            SASSERT(!x_coeff.is_zero());
            new_x_val = -val/x_coeff;

            if (r.m_type == t_lt) {
                eps = abs(old_x_val - new_x_val)/rational(2);
                eps = std::min(rational::one(), eps);
                SASSERT(!eps.is_zero());

                //
                //     ax + t < 0
                // <=> x < -t/a
                // <=> x := -t/a - epsilon
                // 
                if (x_coeff.is_pos()) {
                    new_x_val -= eps;
                }
                //
                //     -ax + t < 0 
                // <=> -ax < -t
                // <=> -x < -t/a
                // <=> x > t/a
                // <=> x := t/a + epsilon
                //
                else {
                    new_x_val += eps;
                }
            }
            TRACE(opt, display(tout << "v" << x 
                                 << " coeff_x: " << x_coeff 
                                 << " old_x_val: " << old_x_val
                                 << " new_x_val: " << new_x_val
                                 << " eps: " << eps << " ", r); );
            m_var2value[x] = new_x_val;
            
            r.m_value = eval(r);
            SASSERT(invariant(bound_trail[i], r));
        }        
        
        // update and check bounds for all other affected rows.
        for (unsigned i = bound_trail.size(); i-- > 0; ) {
            unsigned x = bound_vars[i];
            unsigned_vector const& row_ids = m_var2row_ids[x];
            for (unsigned row_id : row_ids) {                
                row & r = m_rows[row_id];
                r.m_value = eval(r);
                SASSERT(invariant(row_id, r));
            }            
        }
        SASSERT(invariant());
    }

    bool model_based_opt::find_bound(unsigned x, unsigned& bound_row_index, rational& bound_coeff, bool is_pos) {
        bound_row_index = UINT_MAX;
        rational lub_val;
        rational const& x_val = m_var2value[x];
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        m_above.reset();
        m_below.reset();
        for (unsigned row_id : row_ids) {
            SASSERT(row_id != m_objective_id);
            if (visited.contains(row_id)) 
                continue;
            visited.insert(row_id);
            row& r = m_rows[row_id];
            if (!r.m_alive)
                continue;
            rational a = get_coefficient(row_id, x);
            if (a.is_zero()) {
                // skip
            }
            else if (a.is_pos() == is_pos || r.m_type == t_eq) {
                rational value = x_val - (r.m_value/a);
                if (bound_row_index == UINT_MAX) {
                    lub_val = value;
                    bound_row_index = row_id;
                    bound_coeff = a;
                }
                else if ((value == lub_val && r.m_type == opt::t_lt) ||
                         (is_pos && value < lub_val) || 
                         
                         (!is_pos && value > lub_val)) {
                    m_above.push_back(bound_row_index);
                    lub_val = value;
                    bound_row_index = row_id;                          
                    bound_coeff = a;
                }
                else 
                    m_above.push_back(row_id);
            }
            else 
                m_below.push_back(row_id);
        }
        return bound_row_index != UINT_MAX;
    }

    void model_based_opt::retire_row(unsigned row_id) {
        SASSERT(!m_retired_rows.contains(row_id));
        m_rows[row_id].m_alive = false;
        m_retired_rows.push_back(row_id);
    }

    rational model_based_opt::eval(unsigned x) const {
        return m_var2value[x];
    }

    rational model_based_opt::eval(def const& d) const {
        if (d.is_add()) 
            return eval(*d.to_add().x) + eval(*d.to_add().y);
        else if (d.is_div()) 
            return eval(*d.to_div().x) / d.to_div().m_div;
        else if (d.is_mul())
            return eval(*d.to_mul().x) * eval(*d.to_mul().y);
        else if (d.is_var())
            return d.to_var().v.m_coeff * eval(d.to_var().v.m_id);
        else if (d.is_const())
            return d.to_const().c;
        UNREACHABLE();
        return rational::zero();
    }
       
    rational model_based_opt::eval(row const& r) const {
        vector<var> const& vars = r.m_vars;
        rational val = r.m_coeff;
        for (var const& v : vars) {
            val += v.m_coeff * eval(v.m_id);
        }
        return val;
    }    

    rational model_based_opt::eval(vector<var> const& coeffs) const {
        rational val(0);
        for (var const& v : coeffs) 
            val += v.m_coeff * eval(v.m_id);
        return val;
    }
 
    rational model_based_opt::get_coefficient(unsigned row_id, unsigned var_id) const {
        return m_rows[row_id].get_coefficient(var_id);
    }

    rational model_based_opt::row::get_coefficient(unsigned var_id) const {
        if (m_vars.empty()) 
            return rational::zero();
        unsigned lo = 0, hi = m_vars.size();
        while (lo < hi) {
            unsigned mid = lo + (hi - lo)/2;
            SASSERT(mid < hi);
            unsigned id = m_vars[mid].m_id;
            if (id == var_id) {
                lo = mid;
                break;
            }
            if (id < var_id) 
                lo = mid + 1;
            else 
                hi = mid;
        }
        if (lo == m_vars.size()) 
            return rational::zero();
        unsigned id = m_vars[lo].m_id;
        if (id == var_id) 
            return m_vars[lo].m_coeff;
        else 
            return rational::zero();
    }

    model_based_opt::row& model_based_opt::row::normalize() {
#if 0
        if (m_type == t_divides || m_type == t_mod || m_type == t_div)
            return *this;
        rational D(denominator(abs(m_coeff)));
        if (D == 0)
            D = 1;
        for (auto const& [id, coeff] : m_vars)
            if (coeff != 0)
                D = lcm(D, denominator(abs(coeff)));
        if (D == 1)
            return *this;
        SASSERT(D > 0);
        for (auto & [id, coeff] : m_vars)
            coeff *= D;
        m_coeff *= D;
#endif
        return *this;
    }

    // 
    // Let
    //   row1: t1 + a1*x <= 0
    //   row2: t2 + a2*x <= 0
    //
    // assume a1, a2 have the same signs:
    //       (t2 + a2*x) <= (t1 + a1*x)*a2/a1 
    //   <=> t2*a1/a2 - t1 <= 0
    //   <=> t2 - t1*a2/a1 <= 0
    //
    // assume a1 > 0, -a2 < 0:
    //       t1 + a1*x <= 0,  t2 - a2*x <= 0
    //       t2/a2 <= -t1/a1
    //       t2 + t1*a2/a1 <= 0
    // assume -a1 < 0, a2 > 0:
    //       t1 - a1*x <= 0,  t2 + a2*x <= 0
    //       t1/a1 <= -t2/a2
    //       t2 + t1*a2/a1 <= 0
    //
    // the resolvent is the same in all cases (simpler proof should exist)
    //
    // assume a1 < 0, -a1 = a2:
    //    t1 <= a2*div(t2, a2)
    // 

    void model_based_opt::resolve(unsigned row_src, rational const& a1, unsigned row_dst, unsigned x) {

        SASSERT(a1 == get_coefficient(row_src, x));
        SASSERT(!a1.is_zero());
        SASSERT(row_src != row_dst);
                
        if (m_rows[row_dst].m_alive) {
            rational a2 = get_coefficient(row_dst, x);
            if (is_int(x)) {
                TRACE(opt, 
                      tout << "v" << x << ": " << a1 << " " << a2 << ":\n";
                      display(tout, m_rows[row_dst]);
                      display(tout, m_rows[row_src]););
                if (a1.is_pos() != a2.is_pos() || m_rows[row_src].m_type == opt::t_eq) {  
                    mul_add(x, a1, row_src, a2, row_dst);
                }
                else {
                    mul(row_dst, abs(a1));
                    mul_add(false, row_dst, -abs(a2), row_src);
                }
                TRACE(opt, display(tout << "result ", m_rows[row_dst]););
                normalize(row_dst);
            }
            else {
                mul_add(row_dst != m_objective_id && a1.is_pos() == a2.is_pos(), row_dst, -a2/a1, row_src);            
            }
        }
    }

    /**
    * a1 > 0
    * a1*x + r1 = value
    * a2*x + r2 <= 0
    * ------------------
    * a1*r2 - a2*r1 <= value
    */
    void model_based_opt::solve(unsigned row_src, rational const& a1, unsigned row_dst, unsigned x) {
        SASSERT(a1 == get_coefficient(row_src, x));
        SASSERT(a1.is_pos());
        SASSERT(row_src != row_dst);                
        if (!m_rows[row_dst].m_alive) return;
        rational a2 = get_coefficient(row_dst, x);
        mul(row_dst, a1);
        mul_add(false, row_dst, -a2, row_src);
        normalize(row_dst);
        SASSERT(get_coefficient(row_dst, x).is_zero());
    }

    // resolution for integer rows.
    void model_based_opt::mul_add(
        unsigned x, rational src_c, unsigned row_src, rational dst_c, unsigned row_dst) {
        row& dst = m_rows[row_dst];
        row const& src = m_rows[row_src];
        SASSERT(is_int(x));
        SASSERT(t_le == dst.m_type && t_le == src.m_type);
        SASSERT(src_c.is_int());
        SASSERT(dst_c.is_int());
        SASSERT(m_var2value[x].is_int());

        rational abs_src_c = abs(src_c);
        rational abs_dst_c = abs(dst_c);            
        rational x_val = m_var2value[x];
        rational slack = (abs_src_c - rational::one()) * (abs_dst_c - rational::one());
        rational dst_val = dst.m_value - x_val*dst_c;
        rational src_val = src.m_value - x_val*src_c;
        rational distance = abs_src_c * dst_val + abs_dst_c * src_val + slack;
        bool use_case1 = distance.is_nonpos() || abs_src_c.is_one() || abs_dst_c.is_one();
        bool use_case2 = false && abs_src_c == abs_dst_c && src_c.is_pos() != dst_c.is_pos() && !abs_src_c.is_one() && t_le == dst.m_type && t_le == src.m_type; 
        bool use_case3 = false && src_c.is_pos() != dst_c.is_pos() && t_le == dst.m_type && t_le == src.m_type;


        if (use_case1) {
            TRACE(opt, tout << "slack: " << slack << " " << src_c << " " << dst_val << " " << dst_c << " " << src_val << "\n";);
            // dst <- abs_src_c*dst + abs_dst_c*src + slack
            mul(row_dst, abs_src_c);
            add(row_dst, slack);
            mul_add(false, row_dst, abs_dst_c, row_src);
            return;
        }

        if (use_case2 || use_case3) {
            // case2:
            // x*src_c + s <= 0
            // -x*src_c + t <= 0
            // 
            // -src_c*div(-s, src_c) + t <= 0
            //
            // Example:
            //  t <= 100*x <= s
            // Then t <= 100*div(s, 100)
            //
            // case3:
            //  x*src_c + s <= 0
            // -x*dst_c + t <= 0
            // t <= x*dst_c, x*src_c <= -s ->
            // t <= dst_c*div(-s, src_c)   ->
            // -dst_c*div(-s,src_c) + t <= 0
            //

            bool swapped = false;
            if (src_c < 0) {
                std::swap(row_src, row_dst);
                std::swap(src_c, dst_c);
                std::swap(abs_src_c, abs_dst_c);
                swapped = true;
            }
            vector<var> src_coeffs, dst_coeffs;
            rational src_coeff = m_rows[row_src].m_coeff;
            rational dst_coeff = m_rows[row_dst].m_coeff;
            for (auto const& v : m_rows[row_src].m_vars)
                if (v.m_id != x)
                    src_coeffs.push_back(var(v.m_id, -v.m_coeff));
            for (auto const& v : m_rows[row_dst].m_vars)
                if (v.m_id != x)
                    dst_coeffs.push_back(v);
            unsigned v = UINT_MAX;
            if (src_coeffs.empty())
                dst_coeff -= abs_dst_c*div(-src_coeff, abs_src_c);
            else 
                v = add_div(src_coeffs, -src_coeff, abs_src_c);
            if (v != UINT_MAX) dst_coeffs.push_back(var(v, -abs_dst_c));
            if (swapped)
                std::swap(row_src, row_dst);
            retire_row(row_dst);
            add_constraint(dst_coeffs, dst_coeff, t_le);
            return;
        }      

        //
        // create finite disjunction for |b|.                                
        //    exists x, z in [0 .. |b|-2] . b*x + s + z = 0 && ax + t <= 0 && bx + s <= 0
        // <=> 
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && ax + t <= 0 && bx + s <= 0
        // <=>
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && bx + s <= 0
        // <=>
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && -z - s + s <= 0
        // <=>
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0 && -z <= 0
        // <=>
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a|b|x + |b|t <= 0
        // <=>
        //    exists x, z in [0 .. |b|-2] . b*x = -z - s && a*n_sign(b)(s + z) + |b|t <= 0
        // <=>
        //    exists z in [0 .. |b|-2] . |b| | (z + s) && a*n_sign(b)(s + z) + |b|t <= 0
        //

        TRACE(qe, tout << "finite disjunction " << distance << " " << src_c << " " << dst_c << "\n";); 
        vector<var> coeffs;
        if (abs_dst_c <= abs_src_c) {
            rational z = mod(dst_val, abs_dst_c);
            if (!z.is_zero()) z = abs_dst_c - z;
            mk_coeffs_without(coeffs, dst.m_vars, x); 
            add_divides(coeffs, dst.m_coeff + z, abs_dst_c);
            add(row_dst, z);
            mul(row_dst, src_c * n_sign(dst_c));
            mul_add(false, row_dst, abs_dst_c, row_src);            
        }
        else {
            // z := b - (s + bx) mod b 
            //   := b - s mod b
            // b | s + z <=> b | s + b - s mod b <=> b | s - s mod b
            rational z = mod(src_val, abs_src_c);
            if (!z.is_zero()) z = abs_src_c - z;      
            mk_coeffs_without(coeffs, src.m_vars, x);
            add_divides(coeffs, src.m_coeff + z, abs_src_c);
            mul(row_dst, abs_src_c);
            add(row_dst, z * dst_c * n_sign(src_c));            
            mul_add(false, row_dst, dst_c * n_sign(src_c), row_src);
        }
    }

    void model_based_opt::mk_coeffs_without(vector<var>& dst, vector<var> const& src, unsigned x) {
        for (var const & v : src) {
            if (v.m_id != x) dst.push_back(v);
        }
    }

    rational model_based_opt::n_sign(rational const& b) const {
        return rational(b.is_pos()?-1:1);
    }
   
    void model_based_opt::mul(unsigned dst, rational const& c) {
        if (c.is_one())
            return;
        row& r = m_rows[dst];
        for (auto & v : r.m_vars) 
            v.m_coeff *= c;
        r.m_mod *= c;
        r.m_coeff *= c;
        if (r.m_type != t_div && r.m_type != t_mod)
            r.m_value *= c;
    }

    void model_based_opt::add(unsigned dst, rational const& c) {
        row& r = m_rows[dst];
        r.m_coeff += c;
        r.m_value += c;
    }

    void model_based_opt::sub(unsigned dst, rational const& c) {
        row& r = m_rows[dst];
        r.m_coeff -= c;
        r.m_value -= c;
    }

    void model_based_opt::normalize(unsigned row_id) {
        row& r = m_rows[row_id];
        if (!r.m_alive)
            return;
        if (r.m_vars.empty()) {
            retire_row(row_id);
            return;
        }
        if (r.m_type == t_divides) 
            return;
        if (r.m_type == t_mod)
            return;
        if (r.m_type == t_div)
            return;
        rational g(abs(r.m_vars[0].m_coeff));
        bool all_int = g.is_int();
        for (unsigned i = 1; all_int && !g.is_one() && i < r.m_vars.size(); ++i) {
            rational const& coeff = r.m_vars[i].m_coeff;
            if (coeff.is_int()) {
                g = gcd(g, abs(coeff));
            }
            else {
                all_int = false;
            }
        }
        if (all_int && !r.m_coeff.is_zero()) {
            if (r.m_coeff.is_int()) {
                g = gcd(g, abs(r.m_coeff));
            }
            else {
                all_int = false;
            }
        }
        if (all_int && !g.is_one()) {
            SASSERT(!g.is_zero());
            mul(row_id, rational::one()/g);
        }
    }
 
    //
    // set row1 <- row1 + c*row2
    //
    void model_based_opt::mul_add(bool same_sign, unsigned row_id1, rational const& c, unsigned row_id2) {
        if (c.is_zero()) 
            return;
        

        m_new_vars.reset();
        row& r1 = m_rows[row_id1];
        row const& r2 = m_rows[row_id2];
        unsigned i = 0, j = 0;
        while (i < r1.m_vars.size() || j < r2.m_vars.size()) {
            if (j == r2.m_vars.size()) {
                m_new_vars.append(r1.m_vars.size() - i, r1.m_vars.data() + i);
                break;
            }
            if (i == r1.m_vars.size()) {
                for (; j < r2.m_vars.size(); ++j) {
                    m_new_vars.push_back(r2.m_vars[j]);
                    m_new_vars.back().m_coeff *= c;
                    if (row_id1 != m_objective_id) 
                        m_var2row_ids[r2.m_vars[j].m_id].push_back(row_id1);                    
                }
                break;
            }

            unsigned v1 = r1.m_vars[i].m_id;
            unsigned v2 = r2.m_vars[j].m_id;
            if (v1 == v2) {
                m_new_vars.push_back(r1.m_vars[i]);
                m_new_vars.back().m_coeff += c*r2.m_vars[j].m_coeff;
                ++i;
                ++j;
                if (m_new_vars.back().m_coeff.is_zero()) 
                    m_new_vars.pop_back();                
            }
            else if (v1 < v2) {
                m_new_vars.push_back(r1.m_vars[i]);
                ++i;                        
            }
            else {
                m_new_vars.push_back(r2.m_vars[j]);
                m_new_vars.back().m_coeff *= c;
                if (row_id1 != m_objective_id) 
                    m_var2row_ids[r2.m_vars[j].m_id].push_back(row_id1);                
                ++j;                        
            }
        }
        r1.m_coeff += c*r2.m_coeff;
        r1.m_vars.swap(m_new_vars);
        r1.m_value += c*r2.m_value;

        if (!same_sign && r2.m_type == t_lt) 
            r1.m_type = t_lt;        
        else if (same_sign && r1.m_type == t_lt && r2.m_type == t_lt) 
            r1.m_type = t_le;                
        SASSERT(invariant(row_id1, r1));
    }
    
    void model_based_opt::display(std::ostream& out) const {
        for (auto const& r : m_rows) 
            display(out, r);        
        for (unsigned i = 0; i < m_var2row_ids.size(); ++i) {
            unsigned_vector const& rows = m_var2row_ids[i];
            out << i << ": ";
            for (auto const& r : rows) 
                out << r << " ";            
            out << "\n";
        }
    }        

    void model_based_opt::display(std::ostream& out, vector<var> const& vars, rational const& coeff) {
        unsigned i = 0;
        for (var const& v : vars) {
            if (i > 0 && v.m_coeff.is_pos()) 
                out << "+ ";            
            ++i;
            if (v.m_coeff.is_one())
                out << "v" << v.m_id << " ";            
            else 
                out << v.m_coeff << "*v" << v.m_id << " ";                            
        }
        if (coeff.is_pos()) 
            out << " + " << coeff << " ";
        else if (coeff.is_neg()) 
            out << coeff << " ";
    }

    std::ostream& model_based_opt::display(std::ostream& out, row const& r) {
        out << (r.m_alive?"a":"d") << " ";
        display(out, r.m_vars, r.m_coeff);
        switch (r.m_type) {
        case opt::t_divides:
            out << r.m_type << " " << r.m_mod << " = 0; value: " << r.m_value  << "\n";
            break;
        case opt::t_mod:
            out << r.m_type << " " << r.m_mod << " = v" << r.m_id << " ; mod: " << mod(r.m_value, r.m_mod)  << "\n";
            break;            
        case opt::t_div:
            out << r.m_type << " " << r.m_mod << " = v" << r.m_id << " ; div: " << div(r.m_value, r.m_mod)  << "\n";
            break;
        default:
            out << r.m_type << " 0; value: " << r.m_value  << "\n";
            break;
        }
        return out;
    }

    std::ostream& model_based_opt::display(std::ostream& out, def const& r) {
        if (r.is_add())
            return out << "(" << * r.to_add().x << " + " << *r.to_add().y << ")";
        if (r.is_mul())
            return out << "(" << * r.to_mul().x << " * " << *r.to_mul().y << ")";
        if (r.is_var())
            return out << r.to_var().v.m_coeff << "* v" << r.to_var().v.m_id;
        if (r.is_div())
            return out << "(" << * r.to_div().x << " / " << r.to_div().m_div << ")";
        if (r.is_const())
            return out << r.to_const().c;
        UNREACHABLE();
        return out;
    }

    unsigned model_based_opt::add_var(rational const& value, bool is_int) {
        unsigned v = m_var2value.size();
        m_var2value.push_back(value);
        m_var2is_int.push_back(is_int);
        SASSERT(value.is_int() || !is_int);
        m_var2row_ids.push_back(unsigned_vector());
        return v;
    }

    rational model_based_opt::get_value(unsigned var) {
        return m_var2value[var];
    }

    void model_based_opt::set_row(unsigned row_id, vector<var> const& coeffs, rational const& c, rational const& m, ineq_type rel) {
        row& r = m_rows[row_id];
        rational val(c);
        SASSERT(r.m_vars.empty());
        r.m_vars.append(coeffs.size(), coeffs.data());
        bool is_int_row = !coeffs.empty();
        std::sort(r.m_vars.begin(), r.m_vars.end(), var::compare());
        for (auto const& c : coeffs) {
            val += m_var2value[c.m_id] * c.m_coeff;
            SASSERT(!is_int(c.m_id) || c.m_coeff.is_int());
            is_int_row &= is_int(c.m_id);
        }
        r.m_alive = true;
        r.m_coeff = c;
        r.m_value = val;
        r.m_type = rel;
        r.m_mod = m;
        if (is_int_row && rel == t_lt) {
            r.m_type = t_le;
            r.m_coeff  += rational::one();
            r.m_value  += rational::one();
        }
    }

    unsigned model_based_opt::new_row() {
        unsigned row_id = 0;
        if (m_retired_rows.empty()) {
            row_id = m_rows.size();
            m_rows.push_back(row());
        }
        else {
            row_id = m_retired_rows.back();
            m_retired_rows.pop_back();
            SASSERT(!m_rows[row_id].m_alive);
            m_rows[row_id].reset();
            m_rows[row_id].m_alive = true;
        }
        return row_id;
    }

    unsigned model_based_opt::copy_row(unsigned src, unsigned excl) {
        unsigned dst = new_row();
        row const& r = m_rows[src];
        set_row(dst, r.m_vars, r.m_coeff, r.m_mod, r.m_type);
        for (auto const& v : r.m_vars) {
            if (v.m_id != excl)
                m_var2row_ids[v.m_id].push_back(dst);
        }
        SASSERT(invariant(dst, m_rows[dst]));
        return dst;
    }

    // -x + lo <= 0
    void model_based_opt::add_lower_bound(unsigned x, rational const& lo) {
        vector<var> coeffs;
        coeffs.push_back(var(x, rational::minus_one()));
        add_constraint(coeffs, lo, t_le);
    }

    // x - hi <= 0
    void model_based_opt::add_upper_bound(unsigned x, rational const& hi) {
        vector<var> coeffs;
        coeffs.push_back(var(x, rational::one()));
        add_constraint(coeffs, -hi, t_le);
    }

    void model_based_opt::add_constraint(vector<var> const& coeffs, rational const& c, ineq_type rel) {
        add_constraint(coeffs, c, rational::zero(), rel, 0);
    }

    void model_based_opt::add_divides(vector<var> const& coeffs, rational const& c, rational const& m) {
        rational g(c);
        for (auto const& [v, coeff] : coeffs)
            g = gcd(coeff, g);
        if ((g/m).is_int())
            return;
        add_constraint(coeffs, c, m, t_divides, 0);
    }

    unsigned model_based_opt::add_mod(vector<var> const& coeffs, rational const& c, rational const& m) {
        rational value = c;
        for (auto const& var : coeffs)
            value += var.m_coeff * m_var2value[var.m_id];
        unsigned v = add_var(mod(value, m), true);
        add_constraint(coeffs, c, m, t_mod, v);
        return v;
    }

    unsigned model_based_opt::add_div(vector<var> const& coeffs, rational const& c, rational const& m) {        
        rational value = c;
        for (auto const& var : coeffs)
            value += var.m_coeff * m_var2value[var.m_id];
        unsigned v = add_var(div(value, m), true);
        add_constraint(coeffs, c, m, t_div, v);
        return v;
    }

    unsigned model_based_opt::add_constraint(vector<var> const& coeffs, rational const& c, rational const& m, ineq_type rel, unsigned id) {
        auto const& r = m_rows.back();
        if (r.m_vars == coeffs && r.m_coeff == c && r.m_mod == m && r.m_type == rel && r.m_id == id && r.m_alive)
            return m_rows.size() - 1;
        unsigned row_id = new_row();
        set_row(row_id, coeffs, c, m, rel);
        m_rows[row_id].m_id = id;
        for (var const& coeff : coeffs) 
            m_var2row_ids[coeff.m_id].push_back(row_id); 
        SASSERT(invariant(row_id, m_rows[row_id]));
        normalize(row_id);
        return row_id;
    }

    void model_based_opt::set_objective(vector<var> const& coeffs, rational const& c) {
        set_row(m_objective_id, coeffs, c, rational::zero(), t_le);
    }

    void model_based_opt::get_live_rows(vector<row>& rows) {
        for (row & r : m_rows) 
            if (r.m_alive) 
                rows.push_back(r.normalize());                   
    }

    //
    // pick glb and lub representative.
    // The representative is picked such that it 
    // represents the fewest inequalities. 
    // The constraints that enforce a glb or lub are not forced.
    // The constraints that separate the glb from ub or the lub from lb
    // are not forced.
    // In other words, suppose there are 
    // . N inequalities of the form t <= x
    // . M inequalities of the form s >= x
    // . t0 is glb among N under valuation.
    // . s0 is lub among M under valuation.
    // If N < M
    //    create the inequalities:
    //       t <= t0 for each t other than t0 (N-1 inequalities).
    //       t0 <= s for each s (M inequalities).
    // If N >= M the construction is symmetric.
    // 
    model_based_opt::def_ref model_based_opt::project(unsigned x, bool compute_def) {
        unsigned_vector& lub_rows = m_lub;
        unsigned_vector& glb_rows = m_glb;
        unsigned_vector& divide_rows = m_divides;
        unsigned_vector& mod_rows = m_mod;
        unsigned_vector& div_rows = m_div;
        unsigned lub_index = UINT_MAX, glb_index = UINT_MAX;
        bool     lub_strict = false, glb_strict = false;
        rational lub_val, glb_val;
        rational const& x_val = m_var2value[x];
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        lub_rows.reset();
        glb_rows.reset();
        divide_rows.reset();
        mod_rows.reset();
        div_rows.reset();
        bool lub_is_unit = true, glb_is_unit = true;
        unsigned eq_row = UINT_MAX;
        // select the lub and glb.
        for (unsigned row_id : row_ids) {
            if (visited.contains(row_id)) 
                continue;
            visited.insert(row_id);
            row& r = m_rows[row_id];
            if (!r.m_alive) 
                continue;
            rational a = get_coefficient(row_id, x);
            if (a.is_zero()) 
                continue;
            if (r.m_type == t_eq) 
                eq_row = row_id;
            else if (r.m_type == t_mod) 
                mod_rows.push_back(row_id);
            else if (r.m_type == t_div) 
                div_rows.push_back(row_id);
            else if (r.m_type == t_divides) 
                divide_rows.push_back(row_id);
            else if (a.is_pos()) {
                rational lub_value = x_val - (r.m_value/a);
                if (lub_rows.empty() || 
                    lub_value < lub_val ||
                    (lub_value == lub_val && r.m_type == t_lt && !lub_strict)) {
                    lub_val = lub_value;
                    lub_index = row_id;
                    lub_strict = r.m_type == t_lt;                    
                }
                lub_rows.push_back(row_id);
                lub_is_unit &= a.is_one();
            }
            else {
                SASSERT(a.is_neg());
                rational glb_value = x_val - (r.m_value/a);
                if (glb_rows.empty() || 
                    glb_value > glb_val ||
                    (glb_value == glb_val && r.m_type == t_lt && !glb_strict)) {
                    glb_val = glb_value;
                    glb_index = row_id;
                    glb_strict = r.m_type == t_lt;                    
                }
                glb_rows.push_back(row_id);
                glb_is_unit &= a.is_minus_one();
            }
        }

        if (!divide_rows.empty()) 
            return solve_divides(x, divide_rows, compute_def);

        if (!div_rows.empty() || !mod_rows.empty())
            return solve_mod_div(x, mod_rows, div_rows, compute_def);

        if (eq_row != UINT_MAX) 
            return solve_for(eq_row, x, compute_def);

        def_ref result(nullptr);
        unsigned lub_size = lub_rows.size();
        unsigned glb_size = glb_rows.size();
        unsigned row_index = (lub_size <= glb_size) ? lub_index : glb_index;
        
        // There are only upper or only lower bounds.
        if (row_index == UINT_MAX) {
            if (compute_def) {
                if (lub_index != UINT_MAX)                    
                    result = solve_for(lub_index, x, true);                
                else if (glb_index != UINT_MAX) 
                    result = solve_for(glb_index, x, true);                                
                else 
                    result = alloc(const_def, m_var2value[x]);                
                SASSERT(eval(*result) == eval(x));
            }
            else {
                for (unsigned row_id : lub_rows) retire_row(row_id);
                for (unsigned row_id : glb_rows) retire_row(row_id);
            }
            return result;
        }

        SASSERT(lub_index != UINT_MAX);
        SASSERT(glb_index != UINT_MAX);
        if (compute_def) {
            if (lub_size <= glb_size) 
                result = def::from_row(m_rows[lub_index], x);            
            else 
                result = def::from_row(m_rows[glb_index], x);
            TRACE(opt1, display(tout << "resolution result:", *result) << "\n");
        }

        // The number of matching lower and upper bounds is small.
        if ((lub_size <= 2 || glb_size <= 2) &&
            (lub_size <= 3 && glb_size <= 3) && 
            (!is_int(x) || lub_is_unit || glb_is_unit)) {
            for (unsigned i = 0; i < lub_size; ++i) {
                unsigned row_id1 = lub_rows[i];
                bool last = i + 1 == lub_size;
                rational coeff = get_coefficient(row_id1, x);
                for (unsigned row_id2 : glb_rows) {
                    if (last) {
                        resolve(row_id1, coeff, row_id2, x);
                    }
                    else {
                        unsigned row_id3 = copy_row(row_id2);
                        resolve(row_id1, coeff, row_id3, x);
                    }
                }
            }
            for (unsigned row_id : lub_rows) 
                retire_row(row_id);

            return result;
        }

        // General case.
        rational coeff = get_coefficient(row_index, x);
        
        for (unsigned row_id : lub_rows) 
            if (row_id != row_index) 
                resolve(row_index, coeff, row_id, x);
        
        for (unsigned row_id : glb_rows) 
            if (row_id != row_index) 
                resolve(row_index, coeff, row_id, x);
        retire_row(row_index);
        return result;
    }


    //    
    // Given v = a*x + b mod K
    //
    // - remove v = a*x + b mod K
    // 
    // case a = 1:
    // - add w = b mod K
    // - x |-> K*y + z, 0 <= z < K
    // - if z.value + w.value < K:
    //      add z + w - v = 0
    // - if z.value + w.value >= K:
    //      add z + w - v - K = 0
    //
    // case a != 1, gcd(a, K) = 1
    // - x |-> x*y + a^-1*z, 0 <= z < K
    // - add w = b mod K
    // if z.value + w.value < K
    //    add z + w - v = 0
    // if z.value + w.value >= K
    //    add z + w - v - K = 0
    //
    // case a != 1, gcd(a,K) = g != 1
    // - x |-> x*y + a^-1*z, 0 <= z < K
    //  a*x + b mod K = v is now
    //  g*z + b mod K = v
    // - add w = b mod K
    // - 0 <= g*z.value + w.value < K*(g+1)
    // -  add g*z + w - v - k*K = 0 for suitable k from 0 .. g based on model
    // 
    //
    //
    // Given v = a*x + b div K
    // Replace x |-> K*y + z
    // - w = b div K
    // - v = ((a*K*y + a*z) + b) div K
    //     = a*y + (a*z + b) div K
    //     = a*y + b div K + (b mod K + a*z) div K
    //     = a*y + b div K + k
    //  where k := (b.value mod K + a*z.value) div K
    //  k is between 0 and a 
    // 
    // - k*K <= b mod K + a*z < (k+1)*K
    // 
    // A better version using a^-1
    // - v = (a*K*y + a^-1*a*z + b) div K
    //     = a*y + ((K*A + g)*z + b) div K   where we write a*a^-1 = K*A + g
    //     = a*y + A + (g*z + b) div K
    // - k*K <= b Kod m + gz < (k+1)*K
    // where k is between 0 and g
    // when gcd(a, K) = 1, then there are only two cases.
    // 
    model_based_opt::def_ref model_based_opt::solve_mod_div(unsigned x, unsigned_vector const& _mod_rows, unsigned_vector const& _div_rows, bool compute_def) {
        def_ref result(nullptr);
        unsigned_vector div_rows(_div_rows), mod_rows(_mod_rows);
        SASSERT(!div_rows.empty() || !mod_rows.empty());
        TRACE(opt, display(tout << "solve_div v" << x << "\n"));

        rational K(1);
        for (unsigned ri : div_rows)
            K = lcm(K, m_rows[ri].m_mod);
        for (unsigned ri : mod_rows)
            K = lcm(K, m_rows[ri].m_mod);

        rational x_value = m_var2value[x];
        rational z_value = mod(x_value, K);
        rational y_value = div(x_value, K);
        SASSERT(x_value == K * y_value + z_value);
        SASSERT(0 <= z_value && z_value < K);
        // add new variables
        unsigned z = add_var(z_value, true);
        unsigned y = add_var(y_value, true);

        uint_set visited;
        unsigned j = 0;
        for (unsigned ri : div_rows) {
            if (visited.contains(ri))
                continue;
            row& r = m_rows[ri];
            mul(ri, K / r.m_mod);
            r.m_alive = false;
            visited.insert(ri);
            div_rows[j++] = ri;
        }
        div_rows.shrink(j);
        
        j = 0;
        for (unsigned ri : mod_rows) {
            if (visited.contains(ri))
                continue;
            m_rows[ri].m_alive = false;
            visited.insert(ri);
            mod_rows[j++] = ri;
        }
        mod_rows.shrink(j);

        // replace x by K*y + z in other rows.
        for (unsigned ri : m_var2row_ids[x]) {
            if (visited.contains(ri))
                continue;         
            replace_var(ri, x, K, y, rational::one(), z);           
            visited.insert(ri);
            normalize(ri);
        }

        // add bounds for z
        add_lower_bound(z, rational::zero());
        add_upper_bound(z, K - 1);
        
        // solve for x_value = K*y_value + z_value, 0 <= z_value < K.

        unsigned_vector vs;

        for (unsigned ri : div_rows) {

            rational a = get_coefficient(ri, x);
            replace_var(ri, x, rational::zero());

            // add w = b div m
            vector<var> coeffs = m_rows[ri].m_vars;
            rational coeff = m_rows[ri].m_coeff;
            unsigned w = UINT_MAX;
            rational offset(0);
            if (K == 1)
                offset = coeff;
            else if (coeffs.empty())
                offset = div(coeff, K);
            else 
                w = add_div(coeffs, coeff, K);

            //
            // w = b div K
            // v = a*y + w + k
            // k = (a*z_value + (b_value mod K)) div K
            // k*K <= a*z + b mod K < (k+1)*K
            //
            /**
            * It is based on the following claim (tested for select values of a, K)
            * (define-const K Int 13)
            * (declare-const b Int)
            * (define-const a Int -11)
            * (declare-const y Int)
            * (declare-const z Int)
            * (define-const w Int (div b K))
            * (define-const k1 Int (+ (* a z) (mod b K)))
            * (define-const k Int (div k1 K))
            * (define-const x Int (+ (* K y) z))
            * (define-const u Int (+ (* a x) b))
            * (define-const v Int (+ (* a y) w k))
            * (assert (<= 0 z))
            * (assert (< z K))
            * (assert (<= (* K k) k1))
            * (assert (< k1 (* K (+ k 1))))
            * (assert (not (= (div u K) v)))
            * (check-sat)
            */
            unsigned v = m_rows[ri].m_id;
            rational b_value = eval(coeffs) + coeff;
            rational k = div(a * z_value + mod(b_value, K), K);
            vector<var> div_coeffs;
            div_coeffs.push_back(var(v, rational::minus_one()));
            div_coeffs.push_back(var(y, a));
            if (w != UINT_MAX) 
                div_coeffs.push_back(var(w, rational::one()));
            else if (K == 1) 
                div_coeffs.append(coeffs);
            add_constraint(div_coeffs, k + offset, t_eq);

            unsigned u = UINT_MAX;
            offset = 0;
            if (K == 1)
                offset = 0;
            else if (coeffs.empty())
                offset = mod(coeff, K);
            else
                u = add_mod(coeffs, coeff, K);


            // add a*z + (b mod K) < (k + 1)*K
            vector<var> bound_coeffs;
            bound_coeffs.push_back(var(z, a));
            if (u != UINT_MAX)
                bound_coeffs.push_back(var(u, rational::one()));
            add_constraint(bound_coeffs, 1 - K * (k + 1) + offset, t_le);

            // add k*K <= az + (b mod K)
            for (auto& c : bound_coeffs)
                c.m_coeff.neg();
            add_constraint(bound_coeffs, k * K - offset, t_le);
            // allow to recycle row.
            retire_row(ri);
            vs.push_back(v);
        }

        for (unsigned ri : mod_rows) {
            rational a = get_coefficient(ri, x);
            replace_var(ri, x, rational::zero());
            rational rMod = m_rows[ri].m_mod;

            // add w = b mod rMod
            vector<var> coeffs = m_rows[ri].m_vars;
            rational coeff = m_rows[ri].m_coeff;
            unsigned v = m_rows[ri].m_id;
            rational v_value = m_var2value[v];

            unsigned w = UINT_MAX;
            rational offset(0);
            if (coeffs.empty() || rMod == 1)
                offset = mod(coeff, rMod);
            else
                w = add_mod(coeffs, coeff, rMod);


            rational w_value = w == UINT_MAX ? offset : m_var2value[w];

#if 0
            // V := (a * z_value + w_value) div rMod
            // V*rMod <= a*z + w < (V+1)*rMod
            // v = a*z + w - V*rMod
            SASSERT(a > 0);
            SASSERT(z_value >= 0);
            SASSERT(w_value >= 0);
            SASSERT(a * z_value + w_value >= 0);
            rational V = div(a * z_value + w_value, rMod);
            vector<var> mod_coeffs;
            SASSERT(V >= 0);
            SASSERT(a * z_value + w_value >= V*rMod);
            SASSERT((V+1)*rMod > a*z_value + w_value);
            // -a*z - w + V*rMod <= 0
            mod_coeffs.push_back(var(z, -a));
            if (w != UINT_MAX) mod_coeffs.push_back(var(w, -rational::one()));
            add_constraint(mod_coeffs, V*rMod - offset, t_le);
            mod_coeffs.reset();
            // a*z + w - (V+1)*rMod + 1 <= 0
            mod_coeffs.push_back(var(z, a));
            if (w != UINT_MAX) mod_coeffs.push_back(var(w, rational::one()));
            add_constraint(mod_coeffs, -(V+1)*rMod + offset + 1, t_le); 
            mod_coeffs.reset();
            // -v + a*z + w - V*rMod = 0
            mod_coeffs.push_back(var(v, rational::minus_one()));
            mod_coeffs.push_back(var(z, a));
            if (w != UINT_MAX) mod_coeffs.push_back(var(w, rational::one()));
            add_constraint(mod_coeffs, offset - V*rMod, t_eq);

#else
            // add v = a*z + w - V, for V = v_value - a * z_value - w_value
            // claim: (= (mod x rMod) (- x (* rMod (div x rMod)))))) is a theorem for every x, rMod != 0
            rational V = v_value - a * z_value - w_value;
            vector<var> mod_coeffs;
            mod_coeffs.push_back(var(v, rational::minus_one()));
            mod_coeffs.push_back(var(z, a));
            if (w != UINT_MAX) mod_coeffs.push_back(var(w, rational::one()));
            add_constraint(mod_coeffs, V + offset, t_eq);
            add_lower_bound(v, rational::zero());
            add_upper_bound(v, rMod - 1);
#endif

            retire_row(ri);
            vs.push_back(v);
        }


        for (unsigned v : vs) {
            def_ref v_def = project(v, compute_def);
            if (compute_def)
                eliminate(v, *v_def);
        }
                      
        // project internal variables.
        def_ref z_def = project(z, compute_def);
        def_ref y_def = project(y, compute_def); // may depend on z

        if (compute_def) {
            z_def = z_def->substitute(y, *y_def);
            eliminate(y, *y_def);
            eliminate(z, *z_def);

            result = *(*y_def * K) + *z_def;
            m_var2value[x] = eval(*result);
            TRACE(opt, tout << y << " := " << *y_def << "\n";
                         tout << z << " := " << *z_def << "\n";
                         tout << x << " := " << *result << "\n");
        }
        TRACE(opt, display(tout << "solve_div done v" << x << "\n"));
        return result;
    }

    // 
    // compute D and u.
    //
    // D = lcm(d1, d2)
    // u = eval(x) mod D
    // 
    //   d1 | (a1x + t1) & d2 | (a2x + t2)
    // = 
    //   d1 | (a1(D*x' + u) + t1) & d2 | (a2(D*x' + u) + t2)
    // =
    //   d1 | (a1*u + t1) & d2 | (a2*u + t2)
    // 
    // x := D*x' + u
    // 

    model_based_opt::def_ref model_based_opt::solve_divides(unsigned x, unsigned_vector const& divide_rows, bool compute_def) {
        SASSERT(!divide_rows.empty());
        rational D(1);
        for (unsigned idx : divide_rows) {
            D = lcm(D, m_rows[idx].m_mod);            
        }
        if (D.is_zero()) {
            throw default_exception("modulo 0 is not defined");
        }
        if (D.is_neg()) D = abs(D);
        TRACE(opt1, display(tout << "lcm: " << D << " x: v" << x << " tableau\n"););
        rational val_x = m_var2value[x];
        rational u = mod(val_x, D);
        SASSERT(u.is_nonneg() && u < D);
        for (unsigned idx : divide_rows) {
            replace_var(idx, x, u);            
            SASSERT(invariant(idx, m_rows[idx]));
            normalize(idx);
        }
        TRACE(opt1, display(tout << "tableau after replace x under mod\n"););
        //
        // update inequalities such that u is added to t and
        // D is multiplied to coefficient of x.
        // the interpretation of the new version of x is (x-u)/D
        //
        // a*x + t <= 0
        // a*(D*x' + u) + t <= 0
        // a*D*x' + a*u + t <= 0
        //
        rational new_val = (val_x - u) / D;
        SASSERT(new_val.is_int());
        unsigned y = add_var(new_val, true);
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        for (unsigned row_id : row_ids) {           
            if (visited.contains(row_id))
                continue;
            // x |-> D*y + u
            replace_var(row_id, x, D, y, u);
            visited.insert(row_id);
            normalize(row_id);            
        }
        TRACE(opt1, display(tout << "tableau after replace v" << x << " := " << D << " * v" << y << "\n"););
        def_ref result = project(y, compute_def);
        if (compute_def) {
            result = *(*result * D) + u;
            m_var2value[x] = eval(*result);
        }
        TRACE(opt1, display(tout << "tableau after project v" << y << "\n"););
	
        return result;
    }

    // update row with: x |-> C
    void model_based_opt::replace_var(unsigned row_id, unsigned x, rational const& C) {
        row& r = m_rows[row_id];
        SASSERT(!get_coefficient(row_id, x).is_zero());
        unsigned sz = r.m_vars.size();
        unsigned i = 0, j = 0;
        rational coeff(0);
        for (; i < sz; ++i) {
            if (r.m_vars[i].m_id == x) {
                coeff = r.m_vars[i].m_coeff;
            }
            else {
                if (i != j) {
                    r.m_vars[j] = r.m_vars[i];
                }
                ++j;
            }
        }
        if (j != sz) {
            r.m_vars.shrink(j);
        }
        r.m_coeff += coeff*C;
        r.m_value += coeff*(C - m_var2value[x]);
    }

    // update row with: x |-> A*y + B
    void model_based_opt::replace_var(unsigned row_id, unsigned x, rational const& A, unsigned y, rational const& B) {
        row& r = m_rows[row_id];
        rational coeff = get_coefficient(row_id, x);
        if (coeff.is_zero()) return;
        if (!r.m_alive) return;
        replace_var(row_id, x, B);        
        r.m_vars.push_back(var(y, coeff*A));
        r.m_value += coeff*A*m_var2value[y];
        if (!r.m_vars.empty() && r.m_vars.back().m_id > y) 
            std::sort(r.m_vars.begin(), r.m_vars.end(), var::compare());
        m_var2row_ids[y].push_back(row_id);
        SASSERT(invariant(row_id, r));
    }
    
    // update row with: x |-> A*y + B*z
    void model_based_opt::replace_var(unsigned row_id, unsigned x, rational const& A, unsigned y, rational const& B, unsigned z) {
        row& r = m_rows[row_id];
        rational coeff = get_coefficient(row_id, x);
        if (coeff.is_zero() || !r.m_alive)
            return;
        replace_var(row_id, x, rational::zero());        
        if (A != 0) r.m_vars.push_back(var(y, coeff*A));
        if (B != 0) r.m_vars.push_back(var(z, coeff*B));
        r.m_value += coeff*A*m_var2value[y];
        r.m_value += coeff*B*m_var2value[z];
        std::sort(r.m_vars.begin(), r.m_vars.end(), var::compare());
        if (A != 0) m_var2row_ids[y].push_back(row_id);
        if (B != 0) m_var2row_ids[z].push_back(row_id);
        SASSERT(invariant(row_id, r));
    }

    // 3x + t = 0 & 7 | (c*x + s) & ax <= u 
    // 3 | -t  & 21 | (-ct + 3s) & a-t <= 3u

    model_based_opt::def_ref model_based_opt::solve_for(unsigned row_id1, unsigned x, bool compute_def) {
        TRACE(opt, tout << "v" << x << " := " << eval(x) << "\n" << m_rows[row_id1] << "\n";
        display(tout));
        rational a = get_coefficient(row_id1, x), b;
        row& r1 = m_rows[row_id1];
        ineq_type ty = r1.m_type;
        SASSERT(!a.is_zero());
        SASSERT(r1.m_alive);
        if (a.is_neg()) {
            a.neg();
            r1.neg();
        }
        SASSERT(a.is_pos());
        if (ty == t_lt) {
            SASSERT(compute_def);
            r1.m_coeff -= r1.m_value;
            r1.m_type = t_le;
            r1.m_value = 0;
        }        

        if (m_var2is_int[x] && !a.is_one()) {            
            r1.m_coeff -= r1.m_value;
            r1.m_value = 0;
            vector<var> coeffs;
            mk_coeffs_without(coeffs, r1.m_vars, x);
            rational c = mod(-eval(coeffs), a);
            add_divides(coeffs, c, a);
        }
        unsigned_vector const& row_ids = m_var2row_ids[x];
        uint_set visited;
        visited.insert(row_id1);
        for (unsigned row_id2 : row_ids) {
            if (visited.contains(row_id2))
                continue;
            visited.insert(row_id2);
            row& r = m_rows[row_id2];
            if (!r.m_alive)
                continue;
            b = get_coefficient(row_id2, x);
            if (b.is_zero())
                continue;
            row& dst = m_rows[row_id2];
            switch (dst.m_type) {
            case t_eq:
            case t_lt:
            case t_le:
                solve(row_id1, a, row_id2, x);
                break;
            case t_divides:
            case t_mod:
            case t_div:
                // mod reduction already done.
                UNREACHABLE();
                break;
            }
        }
        def_ref result(nullptr);
        if (compute_def) {
            result = def::from_row(m_rows[row_id1], x);
            m_var2value[x] = eval(*result);
            TRACE(opt1, tout << "updated eval " << x << " := " << eval(x) << "\n";);
        }
        retire_row(row_id1);
        TRACE(opt, display(tout << "solved v" << x << "\n"));
        return result;
    }

    void model_based_opt::eliminate(unsigned v, def& new_def) {
        for (auto & d : m_result)
            if (d)
                d = d->substitute(v, new_def);
    }
    
    vector<model_based_opt::def_ref> model_based_opt::project(unsigned num_vars, unsigned const* vars, bool compute_def) {
        m_result.reset();
        for (unsigned i = 0; i < num_vars; ++i) {
            m_result.push_back(project(vars[i], compute_def));
            if (compute_def)
                eliminate(vars[i], *(m_result.back()));
            TRACE(opt, display(tout << "After projecting: v" << vars[i] << "\n"););
        }
        return m_result;
    }

}

