/*++
Copyright (c) 2018 Microsoft Corporation

Module Name:

    theory_seq_params.cpp

Abstract:

    Parameters for sequence theory plugin

Revision History:


--*/

#include "params/theory_seq_params.h"
#include "params/smt_params_helper.hpp"

void theory_seq_params::updt_params(params_ref const & _p) {
    smt_params_helper p(_p);
    m_split_w_len = p.seq_split_w_len();
    m_seq_validate = p.seq_validate();
    m_seq_max_unfolding = p.seq_max_unfolding();
    m_seq_min_unfolding = p.seq_min_unfolding();
}
