/*
 * Copyright (c) 2023, 2024, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2023, Arm Limited. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include "precompiled.hpp"
#include "opto/addnode.hpp"
#include "opto/connode.hpp"
#include "opto/convertnode.hpp"
#include "opto/matcher.hpp"
#include "opto/mulnode.hpp"
#include "opto/rootnode.hpp"
#include "opto/vectorization.hpp"
#include "opto/vectornode.hpp"

#ifndef PRODUCT
int VPointer::Tracer::_depth = 0;
#endif

VPointer::VPointer(const MemNode* mem, const VLoop& vloop,
                   Node_Stack* nstack, bool analyze_only) :
  _mem(mem), _vloop(vloop),
  _base(nullptr), _adr(nullptr), _scale(0), _offset(0), _invar(nullptr),
#ifdef ASSERT
  _debug_invar(nullptr), _debug_negate_invar(false), _debug_invar_scale(nullptr),
#endif
  _nstack(nstack), _analyze_only(analyze_only), _stack_idx(0)
#ifndef PRODUCT
  , _tracer(_vloop)
#endif
{
  NOT_PRODUCT(_tracer.ctor_1(mem);)

  Node* adr = mem->in(MemNode::Address);
  if (!adr->is_AddP()) {
    assert(!valid(), "too complex");
    return;
  }
  // Match AddP(base, AddP(ptr, k*iv [+ invariant]), constant)
  Node* base = adr->in(AddPNode::Base);
  // The base address should be loop invariant
  if (is_loop_member(base)) {
    assert(!valid(), "base address is loop variant");
    return;
  }
  // unsafe references require misaligned vector access support
  if (base->is_top() && !Matcher::misaligned_vectors_ok()) {
    assert(!valid(), "unsafe access");
    return;
  }

  NOT_PRODUCT(if(_tracer.is_trace_pointer_analysis()) { _tracer.store_depth(); })
  NOT_PRODUCT(_tracer.ctor_2(adr);)

  int i;
  for (i = 0; ; i++) {
    NOT_PRODUCT(_tracer.ctor_3(adr, i);)

    if (!scaled_iv_plus_offset(adr->in(AddPNode::Offset))) {
      assert(!valid(), "too complex");
      return;
    }
    adr = adr->in(AddPNode::Address);
    NOT_PRODUCT(_tracer.ctor_4(adr, i);)

    if (base == adr || !adr->is_AddP()) {
      NOT_PRODUCT(_tracer.ctor_5(adr, base, i);)
      break; // stop looking at addp's
    }
  }
  if (is_loop_member(adr)) {
    assert(!valid(), "adr is loop variant");
    return;
  }

  if (!base->is_top() && adr != base) {
    assert(!valid(), "adr and base differ");
    return;
  }

  NOT_PRODUCT(if(_tracer.is_trace_pointer_analysis()) { _tracer.restore_depth(); })
  NOT_PRODUCT(_tracer.ctor_6(mem);)

  _base = base;
  _adr  = adr;
  assert(valid(), "Usable");
}

// Following is used to create a temporary object during
// the pattern match of an address expression.
VPointer::VPointer(VPointer* p) :
  _mem(p->_mem), _vloop(p->_vloop),
  _base(nullptr), _adr(nullptr), _scale(0), _offset(0), _invar(nullptr),
#ifdef ASSERT
  _debug_invar(nullptr), _debug_negate_invar(false), _debug_invar_scale(nullptr),
#endif
  _nstack(p->_nstack), _analyze_only(p->_analyze_only), _stack_idx(p->_stack_idx)
#ifndef PRODUCT
  , _tracer(_vloop)
#endif
{}

// Biggest detectable factor of the invariant.
int VPointer::invar_factor() const {
  Node* n = invar();
  if (n == nullptr) {
    return 0;
  }
  int opc = n->Opcode();
  if (opc == Op_LShiftI && n->in(2)->is_Con()) {
    return 1 << n->in(2)->get_int();
  } else if (opc == Op_LShiftL && n->in(2)->is_Con()) {
    return 1 << n->in(2)->get_int();
  }
  // All our best-effort has failed.
  return 1;
}

bool VPointer::is_loop_member(Node* n) const {
  Node* n_c = phase()->get_ctrl(n);
  return lpt()->is_member(phase()->get_loop(n_c));
}

bool VPointer::invariant(Node* n) const {
  NOT_PRODUCT(Tracer::Depth dd;)
  bool is_not_member = !is_loop_member(n);
  if (is_not_member) {
    CountedLoopNode* cl = lpt()->_head->as_CountedLoop();
    if (cl->is_main_loop()) {
      // Check that n_c dominates the pre loop head node. If it does not, then
      // we cannot use n as invariant for the pre loop CountedLoopEndNode check
      // because n_c is either part of the pre loop or between the pre and the
      // main loop (Illegal invariant happens when n_c is a CastII node that
      // prevents data nodes to flow above the main loop).
      Node* n_c = phase()->get_ctrl(n);
      return phase()->is_dominator(n_c, vloop().pre_loop_head());
    }
  }
  return is_not_member;
}

// Match: k*iv + offset
// where: k is a constant that maybe zero, and
//        offset is (k2 [+/- invariant]) where k2 maybe zero and invariant is optional
bool VPointer::scaled_iv_plus_offset(Node* n) {
  NOT_PRODUCT(Tracer::Depth ddd;)
  NOT_PRODUCT(_tracer.scaled_iv_plus_offset_1(n);)

  if (scaled_iv(n)) {
    NOT_PRODUCT(_tracer.scaled_iv_plus_offset_2(n);)
    return true;
  }

  if (offset_plus_k(n)) {
    NOT_PRODUCT(_tracer.scaled_iv_plus_offset_3(n);)
    return true;
  }

  int opc = n->Opcode();
  if (opc == Op_AddI) {
    if (offset_plus_k(n->in(2)) && scaled_iv_plus_offset(n->in(1))) {
      NOT_PRODUCT(_tracer.scaled_iv_plus_offset_4(n);)
      return true;
    }
    if (offset_plus_k(n->in(1)) && scaled_iv_plus_offset(n->in(2))) {
      NOT_PRODUCT(_tracer.scaled_iv_plus_offset_5(n);)
      return true;
    }
  } else if (opc == Op_SubI || opc == Op_SubL) {
    if (offset_plus_k(n->in(2), true) && scaled_iv_plus_offset(n->in(1))) {
      NOT_PRODUCT(_tracer.scaled_iv_plus_offset_6(n);)
      return true;
    }
    if (offset_plus_k(n->in(1)) && scaled_iv_plus_offset(n->in(2))) {
      _scale *= -1;
      NOT_PRODUCT(_tracer.scaled_iv_plus_offset_7(n);)
      return true;
    }
  }

  NOT_PRODUCT(_tracer.scaled_iv_plus_offset_8(n);)
  return false;
}

// Match: k*iv where k is a constant that's not zero
bool VPointer::scaled_iv(Node* n) {
  NOT_PRODUCT(Tracer::Depth ddd;)
  NOT_PRODUCT(_tracer.scaled_iv_1(n);)

  if (_scale != 0) { // already found a scale
    NOT_PRODUCT(_tracer.scaled_iv_2(n, _scale);)
    return false;
  }

  if (n == iv()) {
    _scale = 1;
    NOT_PRODUCT(_tracer.scaled_iv_3(n, _scale);)
    return true;
  }
  if (_analyze_only && (is_loop_member(n))) {
    _nstack->push(n, _stack_idx++);
  }

  int opc = n->Opcode();
  if (opc == Op_MulI) {
    if (n->in(1) == iv() && n->in(2)->is_Con()) {
      _scale = n->in(2)->get_int();
      NOT_PRODUCT(_tracer.scaled_iv_4(n, _scale);)
      return true;
    } else if (n->in(2) == iv() && n->in(1)->is_Con()) {
      _scale = n->in(1)->get_int();
      NOT_PRODUCT(_tracer.scaled_iv_5(n, _scale);)
      return true;
    }
  } else if (opc == Op_LShiftI) {
    if (n->in(1) == iv() && n->in(2)->is_Con()) {
      _scale = 1 << n->in(2)->get_int();
      NOT_PRODUCT(_tracer.scaled_iv_6(n, _scale);)
      return true;
    }
  } else if (opc == Op_ConvI2L || opc == Op_CastII) {
    if (scaled_iv_plus_offset(n->in(1))) {
      NOT_PRODUCT(_tracer.scaled_iv_7(n);)
      return true;
    }
  } else if (opc == Op_LShiftL && n->in(2)->is_Con()) {
    if (!has_iv()) {
      // Need to preserve the current _offset value, so
      // create a temporary object for this expression subtree.
      // Hacky, so should re-engineer the address pattern match.
      NOT_PRODUCT(Tracer::Depth dddd;)
      VPointer tmp(this);
      NOT_PRODUCT(_tracer.scaled_iv_8(n, &tmp);)

      if (tmp.scaled_iv_plus_offset(n->in(1))) {
        int scale = n->in(2)->get_int();
        _scale   = tmp._scale  << scale;
        _offset += tmp._offset << scale;
        if (tmp._invar != nullptr) {
          BasicType bt = tmp._invar->bottom_type()->basic_type();
          assert(bt == T_INT || bt == T_LONG, "");
          maybe_add_to_invar(register_if_new(LShiftNode::make(tmp._invar, n->in(2), bt)), false);
#ifdef ASSERT
          _debug_invar_scale = n->in(2);
#endif
        }
        NOT_PRODUCT(_tracer.scaled_iv_9(n, _scale, _offset, _invar);)
        return true;
      }
    }
  }
  NOT_PRODUCT(_tracer.scaled_iv_10(n);)
  return false;
}

// Match: offset is (k [+/- invariant])
// where k maybe zero and invariant is optional, but not both.
bool VPointer::offset_plus_k(Node* n, bool negate) {
  NOT_PRODUCT(Tracer::Depth ddd;)
  NOT_PRODUCT(_tracer.offset_plus_k_1(n);)

  int opc = n->Opcode();
  if (opc == Op_ConI) {
    _offset += negate ? -(n->get_int()) : n->get_int();
    NOT_PRODUCT(_tracer.offset_plus_k_2(n, _offset);)
    return true;
  } else if (opc == Op_ConL) {
    // Okay if value fits into an int
    const TypeLong* t = n->find_long_type();
    if (t->higher_equal(TypeLong::INT)) {
      jlong loff = n->get_long();
      jint  off  = (jint)loff;
      _offset += negate ? -off : loff;
      NOT_PRODUCT(_tracer.offset_plus_k_3(n, _offset);)
      return true;
    }
    NOT_PRODUCT(_tracer.offset_plus_k_4(n);)
    return false;
  }
  assert((_debug_invar == nullptr) == (_invar == nullptr), "");

  if (_analyze_only && is_loop_member(n)) {
    _nstack->push(n, _stack_idx++);
  }
  if (opc == Op_AddI) {
    if (n->in(2)->is_Con() && invariant(n->in(1))) {
      maybe_add_to_invar(n->in(1), negate);
      _offset += negate ? -(n->in(2)->get_int()) : n->in(2)->get_int();
      NOT_PRODUCT(_tracer.offset_plus_k_6(n, _invar, negate, _offset);)
      return true;
    } else if (n->in(1)->is_Con() && invariant(n->in(2))) {
      _offset += negate ? -(n->in(1)->get_int()) : n->in(1)->get_int();
      maybe_add_to_invar(n->in(2), negate);
      NOT_PRODUCT(_tracer.offset_plus_k_7(n, _invar, negate, _offset);)
      return true;
    }
  }
  if (opc == Op_SubI) {
    if (n->in(2)->is_Con() && invariant(n->in(1))) {
      maybe_add_to_invar(n->in(1), negate);
      _offset += !negate ? -(n->in(2)->get_int()) : n->in(2)->get_int();
      NOT_PRODUCT(_tracer.offset_plus_k_8(n, _invar, negate, _offset);)
      return true;
    } else if (n->in(1)->is_Con() && invariant(n->in(2))) {
      _offset += negate ? -(n->in(1)->get_int()) : n->in(1)->get_int();
      maybe_add_to_invar(n->in(2), !negate);
      NOT_PRODUCT(_tracer.offset_plus_k_9(n, _invar, !negate, _offset);)
      return true;
    }
  }

  if (!is_loop_member(n)) {
    // 'n' is loop invariant. Skip ConvI2L and CastII nodes before checking if 'n' is dominating the pre loop.
    if (opc == Op_ConvI2L) {
      n = n->in(1);
    }
    if (n->Opcode() == Op_CastII) {
      // Skip CastII nodes
      assert(!is_loop_member(n), "sanity");
      n = n->in(1);
    }
    // Check if 'n' can really be used as invariant (not in main loop and dominating the pre loop).
    if (invariant(n)) {
      maybe_add_to_invar(n, negate);
      NOT_PRODUCT(_tracer.offset_plus_k_10(n, _invar, negate, _offset);)
      return true;
    }
  }

  NOT_PRODUCT(_tracer.offset_plus_k_11(n);)
  return false;
}

Node* VPointer::maybe_negate_invar(bool negate, Node* invar) {
#ifdef ASSERT
  _debug_negate_invar = negate;
#endif
  if (negate) {
    BasicType bt = invar->bottom_type()->basic_type();
    assert(bt == T_INT || bt == T_LONG, "");
    PhaseIterGVN& igvn = phase()->igvn();
    Node* zero = igvn.zerocon(bt);
    phase()->set_ctrl(zero, phase()->C->root());
    Node* sub = SubNode::make(zero, invar, bt);
    invar = register_if_new(sub);
  }
  return invar;
}

Node* VPointer::register_if_new(Node* n) const {
  PhaseIterGVN& igvn = phase()->igvn();
  Node* prev = igvn.hash_find_insert(n);
  if (prev != nullptr) {
    n->destruct(&igvn);
    n = prev;
  } else {
    Node* c = phase()->get_early_ctrl(n);
    phase()->register_new_node(n, c);
  }
  return n;
}

void VPointer::maybe_add_to_invar(Node* new_invar, bool negate) {
  new_invar = maybe_negate_invar(negate, new_invar);
  if (_invar == nullptr) {
    _invar = new_invar;
#ifdef ASSERT
    _debug_invar = new_invar;
#endif
    return;
  }
#ifdef ASSERT
  _debug_invar = NodeSentinel;
#endif
  BasicType new_invar_bt = new_invar->bottom_type()->basic_type();
  assert(new_invar_bt == T_INT || new_invar_bt == T_LONG, "");
  BasicType invar_bt = _invar->bottom_type()->basic_type();
  assert(invar_bt == T_INT || invar_bt == T_LONG, "");

  BasicType bt = (new_invar_bt == T_LONG || invar_bt == T_LONG) ? T_LONG : T_INT;
  Node* current_invar = _invar;
  if (invar_bt != bt) {
    assert(bt == T_LONG && invar_bt == T_INT, "");
    assert(new_invar_bt == bt, "");
    current_invar = register_if_new(new ConvI2LNode(current_invar));
  } else if (new_invar_bt != bt) {
    assert(bt == T_LONG && new_invar_bt == T_INT, "");
    assert(invar_bt == bt, "");
    new_invar = register_if_new(new ConvI2LNode(new_invar));
  }
  Node* add = AddNode::make(current_invar, new_invar, bt);
  _invar = register_if_new(add);
}

// Function for printing the fields of a VPointer
void VPointer::print() {
#ifndef PRODUCT
  tty->print("base: [%d]  adr: [%d]  scale: %d  offset: %d",
             _base != nullptr ? _base->_idx : 0,
             _adr  != nullptr ? _adr->_idx  : 0,
             _scale, _offset);
  if (_invar != nullptr) {
    tty->print("  invar: [%d]", _invar->_idx);
  }
  tty->cr();
#endif
}

// Following are functions for tracing VPointer match
#ifndef PRODUCT
void VPointer::Tracer::print_depth() const {
  for (int ii = 0; ii < _depth; ++ii) {
    tty->print("  ");
  }
}

void VPointer::Tracer::ctor_1(const Node* mem) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print(" %d VPointer::VPointer: start alignment analysis", mem->_idx); mem->dump();
  }
}

void VPointer::Tracer::ctor_2(Node* adr) {
  if (is_trace_pointer_analysis()) {
    inc_depth();
    print_depth(); tty->print(" %d (adr) VPointer::VPointer: ", adr->_idx); adr->dump();
    inc_depth();
    print_depth(); tty->print(" %d (base) VPointer::VPointer: ", adr->in(AddPNode::Base)->_idx); adr->in(AddPNode::Base)->dump();
  }
}

void VPointer::Tracer::ctor_3(Node* adr, int i) {
  if (is_trace_pointer_analysis()) {
    inc_depth();
    Node* offset = adr->in(AddPNode::Offset);
    print_depth(); tty->print(" %d (offset) VPointer::VPointer: i = %d: ", offset->_idx, i); offset->dump();
  }
}

void VPointer::Tracer::ctor_4(Node* adr, int i) {
  if (is_trace_pointer_analysis()) {
    inc_depth();
    print_depth(); tty->print(" %d (adr) VPointer::VPointer: i = %d: ", adr->_idx, i); adr->dump();
  }
}

void VPointer::Tracer::ctor_5(Node* adr, Node* base, int i) {
  if (is_trace_pointer_analysis()) {
    inc_depth();
    if (base == adr) {
      print_depth(); tty->print_cr("  \\ %d (adr) == %d (base) VPointer::VPointer: breaking analysis at i = %d", adr->_idx, base->_idx, i);
    } else if (!adr->is_AddP()) {
      print_depth(); tty->print_cr("  \\ %d (adr) is NOT Addp VPointer::VPointer: breaking analysis at i = %d", adr->_idx, i);
    }
  }
}

void VPointer::Tracer::ctor_6(const Node* mem) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d (adr) VPointer::VPointer: stop analysis", mem->_idx);
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_1(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print(" %d VPointer::scaled_iv_plus_offset testing node: ", n->_idx);
    n->dump();
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_2(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: PASSED", n->_idx);
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_3(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: PASSED", n->_idx);
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_4(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: Op_AddI PASSED", n->_idx);
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(1) is scaled_iv: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(2) is offset_plus_k: ", n->in(2)->_idx); n->in(2)->dump();
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_5(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: Op_AddI PASSED", n->_idx);
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(2) is scaled_iv: ", n->in(2)->_idx); n->in(2)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(1) is offset_plus_k: ", n->in(1)->_idx); n->in(1)->dump();
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_6(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: Op_%s PASSED", n->_idx, n->Name());
    print_depth(); tty->print("  \\  %d VPointer::scaled_iv_plus_offset: in(1) is scaled_iv: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(2) is offset_plus_k: ", n->in(2)->_idx); n->in(2)->dump();
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_7(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: Op_%s PASSED", n->_idx, n->Name());
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(2) is scaled_iv: ", n->in(2)->_idx); n->in(2)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv_plus_offset: in(1) is offset_plus_k: ", n->in(1)->_idx); n->in(1)->dump();
  }
}

void VPointer::Tracer::scaled_iv_plus_offset_8(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv_plus_offset: FAILED", n->_idx);
  }
}

void VPointer::Tracer::scaled_iv_1(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print(" %d VPointer::scaled_iv: testing node: ", n->_idx); n->dump();
  }
}

void VPointer::Tracer::scaled_iv_2(Node* n, int scale) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: FAILED since another _scale has been detected before", n->_idx);
    print_depth(); tty->print_cr("  \\ VPointer::scaled_iv: _scale (%d) != 0", scale);
  }
}

void VPointer::Tracer::scaled_iv_3(Node* n, int scale) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: is iv, setting _scale = %d", n->_idx, scale);
  }
}

void VPointer::Tracer::scaled_iv_4(Node* n, int scale) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: Op_MulI PASSED, setting _scale = %d", n->_idx, scale);
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(1) is iv: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(2) is Con: ", n->in(2)->_idx); n->in(2)->dump();
  }
}

void VPointer::Tracer::scaled_iv_5(Node* n, int scale) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: Op_MulI PASSED, setting _scale = %d", n->_idx, scale);
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(2) is iv: ", n->in(2)->_idx); n->in(2)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(1) is Con: ", n->in(1)->_idx); n->in(1)->dump();
  }
}

void VPointer::Tracer::scaled_iv_6(Node* n, int scale) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: Op_LShiftI PASSED, setting _scale = %d", n->_idx, scale);
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(1) is iv: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::scaled_iv: in(2) is Con: ", n->in(2)->_idx); n->in(2)->dump();
  }
}

void VPointer::Tracer::scaled_iv_7(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: Op_ConvI2L PASSED", n->_idx);
    print_depth(); tty->print_cr("  \\ VPointer::scaled_iv: in(1) %d is scaled_iv_plus_offset: ", n->in(1)->_idx);
    inc_depth(); inc_depth();
    print_depth(); n->in(1)->dump();
    dec_depth(); dec_depth();
  }
}

void VPointer::Tracer::scaled_iv_8(Node* n, VPointer* tmp) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print(" %d VPointer::scaled_iv: Op_LShiftL, creating tmp VPointer: ", n->_idx); tmp->print();
  }
}

void VPointer::Tracer::scaled_iv_9(Node* n, int scale, int offset, Node* invar) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: Op_LShiftL PASSED, setting _scale = %d, _offset = %d", n->_idx, scale, offset);
    print_depth(); tty->print_cr("  \\ VPointer::scaled_iv: in(1) [%d] is scaled_iv_plus_offset, in(2) [%d] used to scale: _scale = %d, _offset = %d",
    n->in(1)->_idx, n->in(2)->_idx, scale, offset);
    if (invar != nullptr) {
      print_depth(); tty->print_cr("  \\ VPointer::scaled_iv: scaled invariant: [%d]", invar->_idx);
    }
    inc_depth(); inc_depth();
    print_depth(); n->in(1)->dump();
    print_depth(); n->in(2)->dump();
    if (invar != nullptr) {
      print_depth(); invar->dump();
    }
    dec_depth(); dec_depth();
  }
}

void VPointer::Tracer::scaled_iv_10(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::scaled_iv: FAILED", n->_idx);
  }
}

void VPointer::Tracer::offset_plus_k_1(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print(" %d VPointer::offset_plus_k: testing node: ", n->_idx); n->dump();
  }
}

void VPointer::Tracer::offset_plus_k_2(Node* n, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_ConI PASSED, setting _offset = %d", n->_idx, _offset);
  }
}

void VPointer::Tracer::offset_plus_k_3(Node* n, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_ConL PASSED, setting _offset = %d", n->_idx, _offset);
  }
}

void VPointer::Tracer::offset_plus_k_4(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: FAILED", n->_idx);
    print_depth(); tty->print_cr("  \\ " JLONG_FORMAT " VPointer::offset_plus_k: Op_ConL FAILED, k is too big", n->get_long());
  }
}

void VPointer::Tracer::offset_plus_k_5(Node* n, Node* _invar) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: FAILED since another invariant has been detected before", n->_idx);
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: _invar is not null: ", _invar->_idx); _invar->dump();
  }
}

void VPointer::Tracer::offset_plus_k_6(Node* n, Node* _invar, bool _negate_invar, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_AddI PASSED, setting _debug_negate_invar = %d, _invar = %d, _offset = %d",
    n->_idx, _negate_invar, _invar->_idx, _offset);
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(2) is Con: ", n->in(2)->_idx); n->in(2)->dump();
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(1) is invariant: ", _invar->_idx); _invar->dump();
  }
}

void VPointer::Tracer::offset_plus_k_7(Node* n, Node* _invar, bool _negate_invar, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_AddI PASSED, setting _debug_negate_invar = %d, _invar = %d, _offset = %d",
    n->_idx, _negate_invar, _invar->_idx, _offset);
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(1) is Con: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(2) is invariant: ", _invar->_idx); _invar->dump();
  }
}

void VPointer::Tracer::offset_plus_k_8(Node* n, Node* _invar, bool _negate_invar, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_SubI is PASSED, setting _debug_negate_invar = %d, _invar = %d, _offset = %d",
    n->_idx, _negate_invar, _invar->_idx, _offset);
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(2) is Con: ", n->in(2)->_idx); n->in(2)->dump();
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(1) is invariant: ", _invar->_idx); _invar->dump();
  }
}

void VPointer::Tracer::offset_plus_k_9(Node* n, Node* _invar, bool _negate_invar, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: Op_SubI PASSED, setting _debug_negate_invar = %d, _invar = %d, _offset = %d", n->_idx, _negate_invar, _invar->_idx, _offset);
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(1) is Con: ", n->in(1)->_idx); n->in(1)->dump();
    print_depth(); tty->print("  \\ %d VPointer::offset_plus_k: in(2) is invariant: ", _invar->_idx); _invar->dump();
  }
}

void VPointer::Tracer::offset_plus_k_10(Node* n, Node* _invar, bool _negate_invar, int _offset) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: PASSED, setting _debug_negate_invar = %d, _invar = %d, _offset = %d", n->_idx, _negate_invar, _invar->_idx, _offset);
    print_depth(); tty->print_cr("  \\ %d VPointer::offset_plus_k: is invariant", n->_idx);
  }
}

void VPointer::Tracer::offset_plus_k_11(Node* n) {
  if (is_trace_pointer_analysis()) {
    print_depth(); tty->print_cr(" %d VPointer::offset_plus_k: FAILED", n->_idx);
  }
}
#endif

AlignmentSolution* AlignmentSolver::solve() const {
  NOT_PRODUCT( trace_start_solve(); )

  // Out of simplicity: non power-of-2 stride not supported.
  if (!is_power_of_2(abs(_pre_stride))) {
    return new EmptyAlignmentSolution("non power-of-2 stride not supported");
  }
  assert(is_power_of_2(abs(_main_stride)), "main_stride is power of 2");
  assert(_aw > 0 && is_power_of_2(_aw), "aw must be power of 2");

  // Out of simplicity: non power-of-2 scale not supported.
  if (abs(_scale) == 0 || !is_power_of_2(abs(_scale))) {
    return new EmptyAlignmentSolution("non power-of-2 scale not supported");
  }

  // We analyze the address of mem_ref. The idea is to disassemble it into a linear
  // expression, where we can use the constant factors as the basis for ensuring the
  // alignment of vector memory accesses.
  //
  // The Simple form of the address is disassembled by VPointer into:
  //
  //   adr = base + offset + invar + scale * iv
  //
  // Where the iv can be written as:
  //
  //   iv = init + pre_stride * pre_iter + main_stride * main_iter
  //
  // init:        value before pre-loop
  // pre_stride:  increment per pre-loop iteration
  // pre_iter:    number of pre-loop iterations (adjustable via pre-loop limit)
  // main_stride: increment per main-loop iteration (= pre_stride * unroll_factor)
  // main_iter:   number of main-loop iterations (main_iter >= 0)
  //
  // In the following, we restate the Simple form of the address expression, by first
  // expanding the iv variable. In a second step, we reshape the expression again, and
  // state it as a linear expression, consisting of 6 terms.
  //
  //          Simple form           Expansion of iv variable                  Reshaped with constants   Comments for terms
  //          -----------           ------------------------                  -----------------------   ------------------
  //   adr =  base               =  base                                   =  base                      (base % aw = 0)
  //        + offset              + offset                                  + C_const                   (sum of constant terms)
  //        + invar               + invar_factor * var_invar                + C_invar * var_invar       (term for invariant)
  //                          /   + scale * init                            + C_init  * var_init        (term for variable init)
  //        + scale * iv   -> |   + scale * pre_stride * pre_iter           + C_pre   * pre_iter        (adjustable pre-loop term)
  //                          \   + scale * main_stride * main_iter         + C_main  * main_iter       (main-loop term)
  //
  // We describe the 6 terms:
  //   1) The "base" of the address is the address of a Java object (e.g. array),
  //      and as such ObjectAlignmentInBytes (a power of 2) aligned. We have
  //      defined aw = MIN(vector_width, ObjectAlignmentInBytes), which is also
  //      a power of 2. And hence we know that "base" is thus also aw-aligned:
  //
  //        base % ObjectAlignmentInBytes = 0     ==>    base % aw = 0
  //
  //   2) The "C_const" term is the sum of all constant terms. This is "offset",
  //      plus "scale * init" if it is constant.
  //   3) The "C_invar * var_invar" is the factorization of "invar" into a constant
  //      and variable term. If there is no invariant, then "C_invar" is zero.
  //
  //        invar = C_invar * var_invar                                             (FAC_INVAR)
  //
  //   4) The "C_init * var_init" is the factorization of "scale * init" into a
  //      constant and a variable term. If "init" is constant, then "C_init" is
  //      zero, and "C_const" accounts for "init" instead.
  //
  //        scale * init = C_init * var_init + scale * C_const_init                 (FAC_INIT)
  //        C_init       = (init is constant) ? 0    : scale
  //        C_const_init = (init is constant) ? init : 0
  //
  //   5) The "C_pre * pre_iter" term represents how much the iv is incremented
  //      during the "pre_iter" pre-loop iterations. This term can be adjusted
  //      by changing the pre-loop limit, which defines how many pre-loop iterations
  //      are executed. This allows us to adjust the alignment of the main-loop
  //      memory reference.
  //   6) The "C_main * main_iter" term represents how much the iv is increased
  //      during "main_iter" main-loop iterations.

  // Attribute init (i.e. _init_node) either to C_const or to C_init term.
  const int C_const_init = _init_node->is_ConI() ? _init_node->as_ConI()->get_int() : 0;
  const int C_const =      _offset + C_const_init * _scale;

  // Set C_invar depending on if invar is present
  const int C_invar = (_invar == nullptr) ? 0 : abs(_invar_factor);

  const int C_init = _init_node->is_ConI() ? 0 : _scale;
  const int C_pre =  _scale * _pre_stride;
  const int C_main = _scale * _main_stride;

  NOT_PRODUCT( trace_reshaped_form(C_const, C_const_init, C_invar, C_init, C_pre, C_main); )

  // We must find a pre_iter, such that adr is aw aligned: adr % aw = 0. Note, that we are defining the
  // modulo operator "%" such that the remainder is always positive, see AlignmentSolution::mod(i, q).
  //
  // Since "base % aw = 0", we only need to ensure alignment of the other 5 terms:
  //
  //   (C_const + C_invar * var_invar + C_init * var_init + C_pre * pre_iter + C_main * main_iter) % aw = 0      (1)
  //
  // Alignment must be maintained over all main-loop iterations, i.e. for any main_iter >= 0, we require:
  //
  //   C_main % aw = 0                                                                                           (2)
  //
  const int C_main_mod_aw = AlignmentSolution::mod(C_main, _aw);

  NOT_PRODUCT( trace_main_iteration_alignment(C_const, C_invar, C_init, C_pre, C_main, C_main_mod_aw); )

  if (C_main_mod_aw != 0) {
    return new EmptyAlignmentSolution("EQ(2) not satisfied (cannot align across main-loop iterations)");
  }

  // In what follows, we need to show that the C_const, init and invar terms can be aligned by
  // adjusting the pre-loop iteration count (pre_iter), which is controlled by the pre-loop
  // limit.
  //
  //     (C_const + C_invar * var_invar + C_init * var_init + C_pre * pre_iter) % aw = 0                         (3)
  //
  // We strengthen the constraints by splitting the equation into 3 equations, where we
  // want to find integer solutions for pre_iter_C_const, pre_iter_C_invar, and
  // pre_iter_C_init, which means that the C_const, init and invar terms can be aligned
  // independently:
  //
  //   (C_const             + C_pre * pre_iter_C_const) % aw = 0                 (4a)
  //   (C_invar * var_invar + C_pre * pre_iter_C_invar) % aw = 0                 (4b)
  //   (C_init  * var_init  + C_pre * pre_iter_C_init ) % aw = 0                 (4c)
  //
  // We now prove that (4a, b, c) are sufficient as well as necessary to guarantee (3)
  // for any runtime value of var_invar and var_init (i.e. for any invar and init).
  // This tells us that the "strengthening" does not restrict the algorithm more than
  // necessary.
  //
  // Sufficient (i.e (4a, b, c) imply (3)):
  //
  //   pre_iter = pre_iter_C_const + pre_iter_C_invar + pre_iter_C_init
  //
  // Adding up (4a, b, c):
  //
  //   0 = (  C_const             + C_pre * pre_iter_C_const
  //        + C_invar * var_invar + C_pre * pre_iter_C_invar
  //        + C_init  * var_init  + C_pre * pre_iter_C_init  ) % aw
  //
  //     = (  C_const + C_invar * var_invar + C_init * var_init
  //        + C_pre * (pre_iter_C_const + pre_iter_C_invar + pre_iter_C_init)) % aw
  //
  //     = (  C_const + C_invar * var_invar + C_init * var_init
  //        + C_pre * pre_iter) % aw
  //
  // Necessary (i.e. (3) implies (4a, b, c)):
  //  (4a): Set var_invar = var_init = 0 at runtime. Applying this to (3), we get:
  //
  //        0 =
  //          = (C_const + C_invar * var_invar + C_init * var_init + C_pre * pre_iter) % aw
  //          = (C_const + C_invar * 0         + C_init * 0        + C_pre * pre_iter) % aw
  //          = (C_const                                           + C_pre * pre_iter) % aw
  //
  //        This is of the same form as (4a), and we have a solution:
  //        pre_iter_C_const = pre_iter
  //
  //  (4b): Set var_init = 0, and assume (4a), which we just proved is implied by (3).
  //        Subtract (4a) from (3):
  //
  //        0 =
  //          =  (C_const + C_invar * var_invar + C_init * var_init + C_pre * pre_iter) % aw
  //           - (C_const + C_pre * pre_iter_C_const) % aw
  //          =  (C_invar * var_invar + C_init * var_init + C_pre * pre_iter - C_pre * pre_iter_C_const) % aw
  //          =  (C_invar * var_invar + C_init * 0        + C_pre * (pre_iter - pre_iter_C_const)) % aw
  //          =  (C_invar * var_invar +                   + C_pre * (pre_iter - pre_iter_C_const)) % aw
  //
  //        This is of the same form as (4b), and we have a solution:
  //        pre_iter_C_invar = pre_iter - pre_iter_C_const
  //
  //  (4c): Set var_invar = 0, and assume (4a), which we just proved is implied by (3).
  //        Subtract (4a) from (3):
  //
  //        0 =
  //          =  (C_const + C_invar * var_invar + C_init * var_init + C_pre * pre_iter) % aw
  //           - (C_const + C_pre * pre_iter_C_const) % aw
  //          =  (C_invar * var_invar + C_init * var_init + C_pre * pre_iter - C_pre * pre_iter_C_const) % aw
  //          =  (C_invar * 0         + C_init * var_init + C_pre * (pre_iter - pre_iter_C_const)) % aw
  //          =  (                    + C_init * var_init + C_pre * (pre_iter - pre_iter_C_const)) % aw
  //
  //        This is of the same form as (4c), and we have a solution:
  //        pre_iter_C_invar = pre_iter - pre_iter_C_const
  //
  // The solutions of Equations (4a, b, c) for pre_iter_C_const, pre_iter_C_invar, and pre_iter_C_init
  // respectively, can have one of these states:
  //
  //   trivial:     The solution can be any integer.
  //   constrained: There is a (periodic) solution, but it is not trivial.
  //   empty:       Statically we cannot guarantee a solution for all var_invar and var_init.
  //
  // We look at (4a):
  //
  //   abs(C_pre) >= aw
  //   -> Since abs(C_pre) is a power of two, we have C_pre % aw = 0. Therefore:
  //
  //        For any pre_iter_C_const: (C_pre * pre_iter_C_const) % aw = 0
  //
  //        (C_const + C_pre * pre_iter_C_const) % aw = 0
  //         C_const                             % aw = 0
  //
  //      Hence, we can only satisfy (4a) if C_Const is aw aligned:
  //
  //      C_const % aw == 0:
  //      -> (4a) has a trivial solution since we can choose any value for pre_iter_C_const.
  //
  //      C_const % aw != 0:
  //      -> (4a) has an empty solution since no pre_iter_C_const can achieve aw alignment.
  //
  //   abs(C_pre) < aw:
  //   -> Since both abs(C_pre) and aw are powers of two, we know:
  //
  //        There exists integer x > 1: aw = abs(C_pre) * x
  //
  //      C_const % abs(C_pre) == 0:
  //      -> There exists integer z: C_const = C_pre * z
  //
  //          (C_const   + C_pre * pre_iter_C_const) % aw               = 0
  //          ==>
  //          (C_pre * z + C_pre * pre_iter_C_const) % aw               = 0
  //          ==>
  //          (C_pre * z + C_pre * pre_iter_C_const) % (abs(C_pre) * x) = 0
  //          ==>
  //          (        z +         pre_iter_C_const) %               x  = 0
  //          ==>
  //          for any m: pre_iter_C_const = m * x - z
  //
  //        Hence, pre_iter_C_const has a non-trivial (because x > 1) periodic (periodicity x)
  //        solution, i.e. it has a constrained solution.
  //
  //      C_const % abs(C_pre) != 0:
  //        There exists integer x > 1: aw = abs(C_pre) * x
  //
  //           C_const                             %  abs(C_pre)      != 0
  //          ==>
  //          (C_const + C_pre * pre_iter_C_const) %  abs(C_pre)      != 0
  //          ==>
  //          (C_const + C_pre * pre_iter_C_const) % (abs(C_pre) * x) != 0
  //          ==>
  //          (C_const + C_pre * pre_iter_C_const) % aw               != 0
  //
  //        This is in contradiction with (4a), and therefore there cannot be any solution,
  //        i.e. we have an empty solution.
  //
  // In summary, for (4a):
  //
  //   abs(C_pre) >= aw  AND  C_const % aw == 0          -> trivial
  //   abs(C_pre) >= aw  AND  C_const % aw != 0          -> empty
  //   abs(C_pre) <  aw  AND  C_const % abs(C_pre) == 0  -> constrained
  //   abs(C_pre) <  aw  AND  C_const % abs(C_pre) != 0  -> empty
  //
  // With analogue argumentation for (4b):
  //
  //   abs(C_pre) >= aw  AND  C_invar % aw == 0           -> trivial
  //   abs(C_pre) >= aw  AND  C_invar % aw != 0           -> empty
  //   abs(C_pre) <  aw  AND  C_invar % abs(C_pre) == 0   -> constrained
  //   abs(C_pre) <  aw  AND  C_invar % abs(C_pre) != 0   -> empty
  //
  // With analogue argumentation for (4c):
  //
  //   abs(C_pre) >= aw  AND  C_init  % aw == 0           -> trivial
  //   abs(C_pre) >= aw  AND  C_init  % aw != 0           -> empty
  //   abs(C_pre) <  aw  AND  C_init  % abs(C_pre) == 0   -> constrained
  //   abs(C_pre) <  aw  AND  C_init  % abs(C_pre) != 0   -> empty
  //
  // Out of these states follows the state for the solution of pre_iter:
  //
  //   Trivial:     If (4a, b, c) are all trivial.
  //   Empty:       If any of (4a, b, c) is empty, because then we cannot guarantee a solution
  //                for pre_iter, for all possible invar and init values.
  //   Constrained: Else. Incidentally, (4a, b, c) are all constrained themselves, as we argue below.

  const EQ4 eq4(C_const, C_invar, C_init, C_pre, _aw);
  const EQ4::State eq4a_state = eq4.eq4a_state();
  const EQ4::State eq4b_state = eq4.eq4b_state();
  const EQ4::State eq4c_state = eq4.eq4c_state();

#ifndef PRODUCT
  if (is_trace()) {
    eq4.trace();
  }
#endif

  // If (4a, b, c) are all trivial, then also the solution for pre_iter is trivial:
  if (eq4a_state == EQ4::State::TRIVIAL &&
      eq4b_state == EQ4::State::TRIVIAL &&
      eq4c_state == EQ4::State::TRIVIAL) {
    return new TrivialAlignmentSolution();
  }

  // If any of (4a, b, c) is empty, then we also cannot guarantee a solution for pre_iter, for
  // any init and invar, hence the solution for pre_iter is empty:
  if (eq4a_state == EQ4::State::EMPTY ||
      eq4b_state == EQ4::State::EMPTY ||
      eq4c_state == EQ4::State::EMPTY) {
    return new EmptyAlignmentSolution("EQ(4a, b, c) not all non-empty: cannot align const, invar and init terms individually");
  }

  // If abs(C_pre) >= aw, then the solutions to (4a, b, c) are all either trivial or empty, and
  // hence we would have found the solution to pre_iter above as either trivial or empty. Thus
  // we now know that:
  //
  //   abs(C_pre) < aw
  //
  assert(abs(C_pre) < _aw, "implied by constrained case");

  // And since abs(C_pre) < aw, the solutions of (4a, b, c) can now only be constrained or empty.
  // But since we already handled the empty case, the solutions are now all constrained.
  assert(eq4a_state == EQ4::State::CONSTRAINED &&
         eq4a_state == EQ4::State::CONSTRAINED &&
         eq4a_state == EQ4::State::CONSTRAINED, "all must be constrained now");

  // And since they are all constrained, we must have:
  //
  //   C_const % abs(C_pre) = 0                                                  (5a)
  //   C_invar % abs(C_pre) = 0                                                  (5b)
  //   C_init  % abs(C_pre) = 0                                                  (5c)
  //
  assert(AlignmentSolution::mod(C_const, abs(C_pre)) == 0, "EQ(5a): C_const must be alignable");
  assert(AlignmentSolution::mod(C_invar, abs(C_pre)) == 0, "EQ(5b): C_invar must be alignable");
  assert(AlignmentSolution::mod(C_init,  abs(C_pre)) == 0, "EQ(5c): C_init  must be alignable");

  // With (5a, b, c), we know that there are integers X, Y, Z:
  //
  //   C_const = X * abs(C_pre)   ==>   X = C_const / abs(C_pre)                 (6a)
  //   C_invar = Y * abs(C_pre)   ==>   Y = C_invar / abs(C_pre)                 (6b)
  //   C_init  = Z * abs(C_pre)   ==>   Z = C_init  / abs(C_pre)                 (6c)
  //
  // Further, we define:
  //
  //   sign(C_pre) = C_pre / abs(C_pre) = (C_pre > 0) ? 1 : -1,                  (7)
  //
  // We know that abs(C_pre) as well as aw are powers of 2, and since (5) we can define integer q:
  //
  //   q = aw / abs(C_pre)                                                       (8)
  //
  const int q = _aw / abs(C_pre);

  assert(q >= 2, "implied by constrained solution");

  // We now know that all terms in (4a, b, c) are divisible by abs(C_pre):
  //
  //   (C_const                    / abs(C_pre) + C_pre * pre_iter_C_const /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (X * abs(C_pre)             / abs(C_pre) + C_pre * pre_iter_C_const /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (X                                       +         pre_iter_C_const * sign(C_pre)) % q                 = 0  (9a)
  //
  //   -> pre_iter_C_const * sign(C_pre) = mx1 * q -               X
  //   -> pre_iter_C_const               = mx2 * q - sign(C_pre) * X                                               (10a)
  //      (for any integers mx1, mx2)
  //
  //   (C_invar        * var_invar / abs(C_pre) + C_pre * pre_iter_C_invar /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (Y * abs(C_pre) * var_invar / abs(C_pre) + C_pre * pre_iter_C_invar /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (Y              * var_invar              +         pre_iter_C_invar * sign(C_pre)) % q                 = 0  (9b)
  //
  //   -> pre_iter_C_invar * sign(C_pre) = my1 * q -               Y * var_invar
  //   -> pre_iter_C_invar               = my2 * q - sign(C_pre) * Y * var_invar                                   (10b)
  //      (for any integers my1, my2)
  //
  //   (C_init          * var_init  / abs(C_pre) + C_pre * pre_iter_C_init /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (Z * abs(C_pre)  * var_init  / abs(C_pre) + C_pre * pre_iter_C_init /  abs(C_pre)) % (aw / abs(C_pre)) =
  //   (Z * var_init                             +         pre_iter_C_init * sign(C_pre)) % q                 = 0  (9c)
  //
  //   -> pre_iter_C_init  * sign(C_pre) = mz1 * q -               Z * var_init
  //   -> pre_iter_C_init                = mz2 * q - sign(C_pre) * Z * var_init                                    (10c)
  //      (for any integers mz1, mz2)
  //
  //
  // Having solved the equations using the division, we can re-substitute X, Y, and Z, and apply (FAC_INVAR) as
  // well as (FAC_INIT). We use the fact that sign(x) == 1 / sign(x) and sign(x) * abs(x) == x:
  //
  //   pre_iter_C_const = mx2 * q - sign(C_pre) * X
  //                    = mx2 * q - sign(C_pre) * C_const             / abs(C_pre)
  //                    = mx2 * q - C_const / C_pre
  //                    = mx2 * q - C_const / (scale * pre_stride)                                  (11a)
  //
  // If there is an invariant:
  //
  //   pre_iter_C_invar = my2 * q - sign(C_pre) * Y       * var_invar
  //                    = my2 * q - sign(C_pre) * C_invar * var_invar / abs(C_pre)
  //                    = my2 * q - sign(C_pre) * invar               / abs(C_pre)
  //                    = my2 * q - invar / C_pre
  //                    = my2 * q - invar / (scale * pre_stride)                                    (11b, with invar)
  //
  // If there is no invariant (i.e. C_invar = 0 ==> Y = 0):
  //
  //   pre_iter_C_invar = my2 * q                                                                   (11b, no invar)
  //
  // If init is variable (i.e. C_init = scale, init = var_init):
  //
  //   pre_iter_C_init  = mz2 * q - sign(C_pre) * Z       * var_init
  //                    = mz2 * q - sign(C_pre) * C_init  * var_init  / abs(C_pre)
  //                    = mz2 * q - sign(C_pre) * scale   * init      / abs(C_pre)
  //                    = mz2 * q - scale * init / C_pre
  //                    = mz2 * q - scale * init / (scale * pre_stride)
  //                    = mz2 * q - init / pre_stride                                               (11c, variable init)
  //
  // If init is constant (i.e. C_init = 0 ==> Z = 0):
  //
  //   pre_iter_C_init  = mz2 * q                                                                   (11c, constant init)
  //
  // Note, that the solutions found by (11a, b, c) are all periodic with periodicity q. We combine them,
  // with m = mx2 + my2 + mz2:
  //
  //   pre_iter =   pre_iter_C_const + pre_iter_C_invar + pre_iter_C_init
  //            =   mx2 * q  - C_const / (scale * pre_stride)
  //              + my2 * q [- invar / (scale * pre_stride) ]
  //              + mz2 * q [- init / pre_stride            ]
  //
  //            =   m * q                                 (periodic part)
  //              - C_const / (scale * pre_stride)        (align constant term)
  //             [- invar / (scale * pre_stride)   ]      (align invariant term, if present)
  //             [- init / pre_stride              ]      (align variable init term, if present)    (12)
  //
  // We can further simplify this solution by introducing integer 0 <= r < q:
  //
  //   r = (-C_const / (scale * pre_stride)) % q                                                    (13)
  //
  const int r = AlignmentSolution::mod(-C_const / (_scale * _pre_stride), q);
  //
  //   pre_iter = m * q + r
  //                   [- invar / (scale * pre_stride)  ]
  //                   [- init / pre_stride             ]                                           (14)
  //
  // We thus get a solution that can be stated in terms of:
  //
  //   q (periodicity), r (constant alignment), invar, scale, pre_stride, init
  //
  // However, pre_stride and init are shared by all mem_ref in the loop, hence we do not need to provide
  // them in the solution description.

  NOT_PRODUCT( trace_constrained_solution(C_const, C_invar, C_init, C_pre, q, r); )

  return new ConstrainedAlignmentSolution(_mem_ref, q, r, _invar, _scale);

  // APPENDIX:
  // We can now verify the success of the solution given by (12):
  //
  //   adr % aw =
  //
  //   -> Simple form
  //   (base + offset + invar + scale * iv) % aw =
  //
  //   -> Expand iv
  //   (base + offset + invar + scale * (init + pre_stride * pre_iter + main_stride * main_iter)) % aw =
  //
  //   -> Reshape
  //   (base + offset + invar
  //         + scale * init
  //         + scale * pre_stride * pre_iter
  //         + scale * main_stride * main_iter)) % aw =
  //
  //   -> base aligned: base % aw = 0
  //   -> main-loop iterations aligned (2): C_main % aw = (scale * main_stride) % aw = 0
  //   (offset + invar + scale * init + scale * pre_stride * pre_iter) % aw =
  //
  //   -> apply (12)
  //   (offset + invar + scale * init
  //           + scale * pre_stride * (m * q - C_const / (scale * pre_stride)
  //                                        [- invar / (scale * pre_stride) ]
  //                                        [- init / pre_stride            ]
  //                                  )
  //   ) % aw =
  //
  //   -> expand C_const = offset [+ init * scale]  (if init const)
  //   (offset + invar + scale * init
  //           + scale * pre_stride * (m * q - offset / (scale * pre_stride)
  //                                        [- init / pre_stride            ]             (if init constant)
  //                                        [- invar / (scale * pre_stride) ]             (if invar present)
  //                                        [- init / pre_stride            ]             (if init variable)
  //                                  )
  //   ) % aw =
  //
  //   -> assuming invar = 0 if it is not present
  //   -> merge the two init terms (variable or constant)
  //   -> apply (8): q = aw / (abs(C_pre)) = aw / abs(scale * pre_stride)
  //   -> and hence: (scale * pre_stride * q) % aw = 0
  //   -> all terms are canceled out
  //   (offset + invar + scale * init
  //           + scale * pre_stride * m * q                             -> aw aligned
  //           - scale * pre_stride * offset / (scale * pre_stride)     -> = offset
  //           - scale * pre_stride * init / pre_stride                 -> = scale * init
  //           - scale * pre_stride * invar / (scale * pre_stride)      -> = invar
  //   ) % aw = 0
  //
  // The solution given by (12) does indeed guarantee alignment.
}

#ifndef PRODUCT
void print_con_or_idx(const Node* n) {
  if (n == nullptr) {
    tty->print("(0)");
  } else if (n->is_ConI()) {
    jint val = n->as_ConI()->get_int();
    tty->print("(%d)", val);
  } else {
    tty->print("[%d]", n->_idx);
  }
}

void AlignmentSolver::trace_start_solve() const {
  if (is_trace()) {
    tty->print(" vector mem_ref:");
    _mem_ref->dump();
    tty->print_cr("  vector_width = vector_length(%d) * element_size(%d) = %d",
                  _vector_length, _element_size, _vector_width);
    tty->print_cr("  aw = alignment_width = min(vector_width(%d), ObjectAlignmentInBytes(%d)) = %d",
                  _vector_width, ObjectAlignmentInBytes, _aw);

    if (!_init_node->is_ConI()) {
      tty->print("  init:");
      _init_node->dump();
    }

    if (_invar != nullptr) {
      tty->print("  invar:");
      _invar->dump();
    }

    tty->print_cr("  invar_factor = %d", _invar_factor);

    // iv = init + pre_iter * pre_stride + main_iter * main_stride
    tty->print("  iv = init");
    print_con_or_idx(_init_node);
    tty->print_cr(" + pre_iter * pre_stride(%d) + main_iter * main_stride(%d)",
                  _pre_stride, _main_stride);

    // adr = base + offset + invar + scale * iv
    tty->print("  adr = base");
    print_con_or_idx(_base);
    tty->print(" + offset(%d) + invar", _offset);
    print_con_or_idx(_invar);
    tty->print_cr(" + scale(%d) * iv", _scale);
  }
}

void AlignmentSolver::trace_reshaped_form(const int C_const,
                                          const int C_const_init,
                                          const int C_invar,
                                          const int C_init,
                                          const int C_pre,
                                          const int C_main) const
{
  if (is_trace()) {
    tty->print("      = base[%d] + ", _base->_idx);
    tty->print_cr("C_const(%d) + C_invar(%d) * var_invar + C_init(%d) * var_init + C_pre(%d) * pre_iter + C_main(%d) * main_iter",
                  C_const, C_invar, C_init,  C_pre, C_main);
    if (_init_node->is_ConI()) {
      tty->print_cr("  init is constant:");
      tty->print_cr("    C_const_init = %d", C_const_init);
      tty->print_cr("    C_init = %d", C_init);
    } else {
      tty->print_cr("  init is variable:");
      tty->print_cr("    C_const_init = %d", C_const_init);
      tty->print_cr("    C_init = abs(scale)= %d", C_init);
    }
    if (_invar != nullptr) {
      tty->print_cr("  invariant present:");
      tty->print_cr("    C_invar = abs(invar_factor) = %d", C_invar);
    } else {
      tty->print_cr("  no invariant:");
      tty->print_cr("    C_invar = %d", C_invar);
    }
    tty->print_cr("  C_const = offset(%d) + scale(%d) * C_const_init(%d) = %d",
                  _offset, _scale, C_const_init, C_const);
    tty->print_cr("  C_pre   = scale(%d) * pre_stride(%d) = %d",
                  _scale, _pre_stride, C_pre);
    tty->print_cr("  C_main  = scale(%d) * main_stride(%d) = %d",
                  _scale, _main_stride, C_main);
  }
}

void AlignmentSolver::trace_main_iteration_alignment(const int C_const,
                                                     const int C_invar,
                                                     const int C_init,
                                                     const int C_pre,
                                                     const int C_main,
                                                     const int C_main_mod_aw) const
{
  if (is_trace()) {
    tty->print("  EQ(1 ): (C_const(%d) + C_invar(%d) * var_invar + C_init(%d) * var_init",
                  C_const, C_invar, C_init);
    tty->print(" + C_pre(%d) * pre_iter + C_main(%d) * main_iter) %% aw(%d) = 0",
                  C_pre, C_main, _aw);
    tty->print_cr(" (given base aligned -> align rest)");
    tty->print("  EQ(2 ): C_main(%d) %% aw(%d) = %d = 0",
               C_main, _aw, C_main_mod_aw);
    tty->print_cr(" (alignment across iterations)");
  }
}

void AlignmentSolver::EQ4::trace() const {
  tty->print_cr("  EQ(4a): (C_const(%3d)             + C_pre(%d) * pre_iter_C_const) %% aw(%d) = 0  (align const term individually)",
                _C_const, _C_pre, _aw);
  tty->print_cr("          -> %s", state_to_str(eq4a_state()));

  tty->print_cr("  EQ(4b): (C_invar(%3d) * var_invar + C_pre(%d) * pre_iter_C_invar) %% aw(%d) = 0  (align invar term individually)",
                _C_invar, _C_pre, _aw);
  tty->print_cr("          -> %s", state_to_str(eq4b_state()));

  tty->print_cr("  EQ(4c): (C_init( %3d) * var_init  + C_pre(%d) * pre_iter_C_init ) %% aw(%d) = 0  (align init term individually)",
                _C_init, _C_pre, _aw);
  tty->print_cr("          -> %s", state_to_str(eq4c_state()));
}

void AlignmentSolver::trace_constrained_solution(const int C_const,
                                                 const int C_invar,
                                                 const int C_init,
                                                 const int C_pre,
                                                 const int q,
                                                 const int r) const
{
  if (is_trace()) {
    tty->print_cr("  EQ(4a, b, c) all constrained, hence:");
    tty->print_cr("  EQ(5a): C_const(%3d) %% abs(C_pre(%d)) = 0", C_const, C_pre);
    tty->print_cr("  EQ(5b): C_invar(%3d) %% abs(C_pre(%d)) = 0", C_invar, C_pre);
    tty->print_cr("  EQ(5c): C_init( %3d) %% abs(C_pre(%d)) = 0", C_init,  C_pre);

    tty->print_cr("  All terms in EQ(4a, b, c) are divisible by abs(C_pre(%d)).", C_pre);
    const int X    = C_const / abs(C_pre);
    const int Y    = C_invar / abs(C_pre);
    const int Z    = C_init  / abs(C_pre);
    const int sign = (C_pre > 0) ? 1 : -1;
    tty->print_cr("  X = C_const(%3d) / abs(C_pre(%d)) = %d       (6a)", C_const, C_pre, X);
    tty->print_cr("  Y = C_invar(%3d) / abs(C_pre(%d)) = %d       (6b)", C_invar, C_pre, Y);
    tty->print_cr("  Z = C_init( %3d) / abs(C_pre(%d)) = %d       (6c)", C_init , C_pre, Z);
    tty->print_cr("  q = aw(     %3d) / abs(C_pre(%d)) = %d       (8)",  _aw,     C_pre, q);
    tty->print_cr("  sign(C_pre) = (C_pre(%d) > 0) ? 1 : -1 = %d  (7)",  C_pre,   sign);

    tty->print_cr("  EQ(9a): (X(%3d)             + pre_iter_C_const * sign(C_pre)) %% q(%d) = 0", X, q);
    tty->print_cr("  EQ(9b): (Y(%3d) * var_invar + pre_iter_C_invar * sign(C_pre)) %% q(%d) = 0", Y, q);
    tty->print_cr("  EQ(9c): (Z(%3d) * var_init  + pre_iter_C_init  * sign(C_pre)) %% q(%d) = 0", Z, q);

    tty->print_cr("  EQ(10a): pre_iter_C_const = mx2 * q(%d) - sign(C_pre) * X(%d)",             q, X);
    tty->print_cr("  EQ(10b): pre_iter_C_invar = my2 * q(%d) - sign(C_pre) * Y(%d) * var_invar", q, Y);
    tty->print_cr("  EQ(10c): pre_iter_C_init  = mz2 * q(%d) - sign(C_pre) * Z(%d) * var_init ", q, Z);

    tty->print_cr("  r = (-C_const(%d) / (scale(%d) * pre_stride(%d)) %% q(%d) = %d",
                  C_const, _scale, _pre_stride, q, r);

    tty->print_cr("  EQ(14):  pre_iter = m * q(%3d) - r(%d)", q, r);
    if (_invar != nullptr) {
      tty->print_cr("                                 - invar / (scale(%d) * pre_stride(%d))",
                    _scale, _pre_stride);
    }
    if (!_init_node->is_ConI()) {
      tty->print_cr("                                 - init / pre_stride(%d)",
                    _pre_stride);
    }
  }
}
#endif

bool VLoop::check_preconditions() {
#ifndef PRODUCT
  if (is_trace_precondition()) {
    tty->print_cr("\nVLoop::check_precondition");
    lpt()->dump_head();
    lpt()->head()->dump();
  }
#endif

  const char* return_state = check_preconditions_helper();
  assert(return_state != nullptr, "must have return state");
  if (return_state == VLoop::SUCCESS) {
    return true; // success
  }

#ifndef PRODUCT
  if (is_trace_precondition()) {
    tty->print_cr("VLoop::check_precondition: failed: %s", return_state);
  }
#endif
  return false; // failure
}

const char* VLoop::check_preconditions_helper() {
  // Only accept vector width that is power of 2
  int vector_width = Matcher::vector_width_in_bytes(T_BYTE);
  if (vector_width < 2 || !is_power_of_2(vector_width)) {
    return VLoop::FAILURE_VECTOR_WIDTH;
  }

  // Only accept valid counted loops (int)
  if (!_lpt->_head->as_Loop()->is_valid_counted_loop(T_INT)) {
    return VLoop::FAILURE_VALID_COUNTED_LOOP;
  }
  _cl = _lpt->_head->as_CountedLoop();
  _iv = _cl->phi()->as_Phi();

  if (_cl->is_vectorized_loop()) {
    return VLoop::FAILURE_ALREADY_VECTORIZED;
  }

  if (_cl->is_unroll_only()) {
    return VLoop::FAILURE_UNROLL_ONLY;
  }

  // Check for control flow in the body
  _cl_exit = _cl->loopexit();
  bool has_cfg = _cl_exit->in(0) != _cl;
  if (has_cfg && !is_allow_cfg()) {
#ifndef PRODUCT
    if (is_trace_precondition()) {
      tty->print_cr("VLoop::check_preconditions: fails because of control flow.");
      tty->print("  cl_exit %d", _cl_exit->_idx); _cl_exit->dump();
      tty->print("  cl_exit->in(0) %d", _cl_exit->in(0)->_idx); _cl_exit->in(0)->dump();
      tty->print("  lpt->_head %d", _cl->_idx); _cl->dump();
      _lpt->dump_head();
    }
#endif
    return VLoop::FAILURE_CONTROL_FLOW;
  }

  // Make sure the are no extra control users of the loop backedge
  if (_cl->back_control()->outcnt() != 1) {
    return VLoop::FAILURE_BACKEDGE;
  }

  // To align vector memory accesses in the main-loop, we will have to adjust
  // the pre-loop limit.
  if (_cl->is_main_loop()) {
    CountedLoopEndNode* pre_end = _cl->find_pre_loop_end();
    if (pre_end == nullptr) {
      return VLoop::FAILURE_PRE_LOOP_LIMIT;
    }
    Node* pre_opaq1 = pre_end->limit();
    if (pre_opaq1->Opcode() != Op_Opaque1) {
      return VLoop::FAILURE_PRE_LOOP_LIMIT;
    }
    _pre_loop_end = pre_end;
  }

  return VLoop::SUCCESS;
}

bool VLoopAnalyzer::analyze() {
  bool success = _vloop.check_preconditions();
  if (!success) { return false; }

#ifndef PRODUCT
  if (vloop().is_trace_loop_analyzer()) {
    tty->print_cr("VLoopAnalyzer::analyze");
    vloop().lpt()->dump_head();
    vloop().cl()->dump();
  }
#endif

  const char* return_state = analyze_helper();
  assert(return_state != nullptr, "must have return state");
  if (return_state == VLoopAnalyzer::SUCCESS) {
    return true; // success
  }

#ifndef PRODUCT
  if (vloop().is_trace_loop_analyzer()) {
    tty->print_cr("VLoopAnalyze::analyze: failed: %s", return_state);
  }
#endif
  return false; // failure
}

const char* VLoopAnalyzer::analyze_helper() {
  // skip any loop that has not been assigned max unroll by analysis
  if (SuperWordLoopUnrollAnalysis && vloop().cl()->slp_max_unroll() == 0) {
    return VLoopAnalyzer::FAILURE_NO_MAX_UNROLL;
  }

  if (SuperWordReductions) {
    _reductions.mark_reductions();
  }

  _memory_slices.analyze();

  // If there is no memory slice detected, that means there is no store.
  // If there is no reduction and no store, then we give up, because
  // vectorization is not possible anyway (given current limitations).
  if (!_reductions.is_marked_reduction_loop() &&
      _memory_slices.heads().is_empty()) {
    return VLoopAnalyzer::FAILURE_NO_REDUCTION_OR_STORE;
  }

  const char* body_failure = _body.construct();
  if (body_failure != nullptr) {
    return body_failure;
  }

  _types.compute_vector_element_type();

  _dependence_graph.build();

  return VLoopAnalyzer::SUCCESS;
}

bool VLoopReductions::is_reduction(const Node* n) {
  if (!is_reduction_operator(n)) {
    return false;
  }
  // Test whether there is a reduction cycle via every edge index
  // (typically indices 1 and 2).
  for (uint input = 1; input < n->req(); input++) {
    if (in_reduction_cycle(n, input)) {
      return true;
    }
  }
  return false;
}

bool VLoopReductions::is_marked_reduction_pair(Node* s1, Node* s2) const {
  if (is_marked_reduction(s1) &&
      is_marked_reduction(s2)) {
    // This is an ordered set, so s1 should define s2
    for (DUIterator_Fast imax, i = s1->fast_outs(imax); i < imax; i++) {
      Node* t1 = s1->fast_out(i);
      if (t1 == s2) {
        // both nodes are reductions and connected
        return true;
      }
    }
  }
  return false;
}

bool VLoopReductions::is_reduction_operator(const Node* n) {
  int opc = n->Opcode();
  return (opc != ReductionNode::opcode(opc, n->bottom_type()->basic_type()));
}

bool VLoopReductions::in_reduction_cycle(const Node* n, uint input) {
  // First find input reduction path to phi node.
  auto has_my_opcode = [&](const Node* m){ return m->Opcode() == n->Opcode(); };
  PathEnd path_to_phi = find_in_path(n, input, LoopMaxUnroll, has_my_opcode,
                                     [&](const Node* m) { return m->is_Phi(); });
  const Node* phi = path_to_phi.first;
  if (phi == nullptr) {
    return false;
  }
  // If there is an input reduction path from the phi's loop-back to n, then n
  // is part of a reduction cycle.
  const Node* first = phi->in(LoopNode::LoopBackControl);
  PathEnd path_from_phi = find_in_path(first, input, LoopMaxUnroll, has_my_opcode,
                                       [&](const Node* m) { return m == n; });
  return path_from_phi.first != nullptr;
}

Node* VLoopReductions::original_input(const Node* n, uint i) {
  if (n->has_swapped_edges()) {
    assert(n->is_Add() || n->is_Mul(), "n should be commutative");
    if (i == 1) {
      return n->in(2);
    } else if (i == 2) {
      return n->in(1);
    }
  }
  return n->in(i);
}

void VLoopReductions::mark_reductions() {
  assert(_loop_reductions.is_empty(), "must have been reset");
  IdealLoopTree*  lpt = _vloop.lpt();
  CountedLoopNode* cl = _vloop.cl();
  PhiNode*         iv = _vloop.iv();

  // Iterate through all phi nodes associated to the loop and search for
  // reduction cycles in the basic block.
  for (DUIterator_Fast imax, i = cl->fast_outs(imax); i < imax; i++) {
    const Node* phi = cl->fast_out(i);
    if (!phi->is_Phi()) {
      continue;
    }
    if (phi->outcnt() == 0) {
      continue;
    }
    if (phi == iv) {
      continue;
    }
    // The phi's loop-back is considered the first node in the reduction cycle.
    const Node* first = phi->in(LoopNode::LoopBackControl);
    if (first == nullptr) {
      continue;
    }
    // Test that the node fits the standard pattern for a reduction operator.
    if (!is_reduction_operator(first)) {
      continue;
    }
    // Test that 'first' is the beginning of a reduction cycle ending in 'phi'.
    // To contain the number of searched paths, assume that all nodes in a
    // reduction cycle are connected via the same edge index, modulo swapped
    // inputs. This assumption is realistic because reduction cycles usually
    // consist of nodes cloned by loop unrolling.
    int reduction_input = -1;
    int path_nodes = -1;
    for (uint input = 1; input < first->req(); input++) {
      // Test whether there is a reduction path in the basic block from 'first'
      // to the phi node following edge index 'input'.
      PathEnd path =
        find_in_path(
          first, input, lpt->_body.size(),
          [&](const Node* n) { return n->Opcode() == first->Opcode() &&
                                      _vloop.in_body(n); },
          [&](const Node* n) { return n == phi; });
      if (path.first != nullptr) {
        reduction_input = input;
        path_nodes = path.second;
        break;
      }
    }
    if (reduction_input == -1) {
      continue;
    }
    // Test that reduction nodes do not have any users in the loop besides their
    // reduction cycle successors.
    const Node* current = first;
    const Node* succ = phi; // current's successor in the reduction cycle.
    bool used_in_loop = false;
    for (int i = 0; i < path_nodes; i++) {
      for (DUIterator_Fast jmax, j = current->fast_outs(jmax); j < jmax; j++) {
        Node* u = current->fast_out(j);
        if (!_vloop.in_body(u)) {
          continue;
        }
        if (u == succ) {
          continue;
        }
        used_in_loop = true;
        break;
      }
      if (used_in_loop) {
        break;
      }
      succ = current;
      current = original_input(current, reduction_input);
    }
    if (used_in_loop) {
      continue;
    }
    // Reduction cycle found. Mark all nodes in the found path as reductions.
    current = first;
    for (int i = 0; i < path_nodes; i++) {
      _loop_reductions.set(current->_idx);
      current = original_input(current, reduction_input);
    }
  }
}

void VLoopMemorySlices::analyze() {
  assert(_heads.is_empty(), "must have been reset");
  assert(_tails.is_empty(), "must have been reset");

  CountedLoopNode* cl = _vloop.cl();

  for (DUIterator_Fast imax, i = cl->fast_outs(imax); i < imax; i++) {
    PhiNode* phi = cl->fast_out(i)->isa_Phi();
    if (phi != nullptr &&
        _vloop.in_body(phi) &&
        phi->is_memory_phi()) {
      Node* phi_tail  = phi->in(LoopNode::LoopBackControl);
      if (phi_tail != phi->in(LoopNode::EntryControl)) {
        _heads.push(phi);
        _tails.push(phi_tail->as_Mem());
      }
    }
  }

#ifndef PRODUCT
  if (_vloop.is_trace_memory_slices()) {
    print();
  }
#endif
}

void VLoopMemorySlices::get_slice(Node* head,
                                  Node* tail,
                                  GrowableArray<Node*> &slice) const {
  slice.clear();
  // Start at tail, and go up through Store nodes.
  // For each Store node, find all Loads below that Store.
  // Terminate once we reach the head.
  Node* n = tail;
  Node* prev = nullptr;
  while (true) {
    assert(_vloop.in_body(n), "must be in block");
    for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
      Node* out = n->fast_out(i);
      if (out->is_Load()) {
        if (_vloop.in_body(out)) {
          slice.push(out);
        }
      } else {
        // Expect other outputs to be the prev (with some exceptions)
        if (out->is_MergeMem() && !_vloop.in_body(out)) {
          // Either unrolling is causing a memory edge not to disappear,
          // or need to run igvn.optimize() again before vectorization
        } else if (out->is_memory_phi() && !_vloop.in_body(out)) {
          // Ditto.  Not sure what else to check further.
        } else if (out->Opcode() == Op_StoreCM && out->in(MemNode::OopStore) == n) {
          // StoreCM has an input edge used as a precedence edge.
          // Maybe an issue when oop stores are vectorized.
        } else {
          assert(out == prev || prev == nullptr, "no branches off of store slice");
        }
      }
    }
    if (n == head) { break; };
    slice.push(n);
    prev = n;
    assert(n->is_Mem(), "unexpected node %s", n->Name());
    n = n->in(MemNode::Memory);
  }

#ifndef PRODUCT
  if (_vloop.is_trace_memory_slices()) {
    tty->print_cr("\nVLoopMemorySlices::get_slice:");
    head->dump();
    for (int j = slice.length() - 1; j >= 0 ; j--) {
      slice.at(j)->dump();
    }
  }
#endif
}

bool VLoopMemorySlices::same_memory_slice(MemNode* n1, MemNode* n2) const {
  return _vloop.phase()->C->get_alias_index(n1->adr_type()) ==
         _vloop.phase()->C->get_alias_index(n2->adr_type());
}

#ifndef PRODUCT
void VLoopMemorySlices::print() const {
  tty->print_cr("\nVLoopMemorySlices::print: %s",
                _heads.length() > 0 ? "" : "NONE");
  for (int m = 0; m < _heads.length(); m++) {
    tty->print("%6d ", m);  _heads.at(m)->dump();
    tty->print("       ");  _tails.at(m)->dump();
  }
}
#endif

const char* VLoopBody::construct() {
  assert(_body.is_empty(),     "must have been reset");
  assert(_body_idx.is_empty(), "must have been reset");

  IdealLoopTree*  lpt = _vloop.lpt();
  CountedLoopNode* cl = _vloop.cl();

  // First pass over loop body:
  //  (1) Check that there are no unwanted nodes (LoadStore, MergeMem, data Proj).
  //  (2) Count number of nodes, and create a temporary map (_idx -> body_idx).
  //  (3) Verify that all non-ctrl nodes have an input inside the loop.
  int body_count = 0;
  for (uint i = 0; i < lpt->_body.size(); i++) {
    Node* n = lpt->_body.at(i);
    if (!_vloop.in_body(n)) { continue; }

    // Create a temporary map
    set_body_idx(n, i);
    body_count++;

    if (n->is_LoadStore() ||
        n->is_MergeMem() ||
        (n->is_Proj() && !n->as_Proj()->is_CFG())) {
      // Bailout if the loop has LoadStore, MergeMem or data Proj
      // nodes. Superword optimization does not work with them.
#ifndef PRODUCT
      if (_vloop.is_trace_body()) {
        tty->print_cr("VLoopBody::construct: fails because of unhandled node:");
        n->dump();
      }
#endif
      return VLoopBody::FAILURE_NODE_NOT_ALLOWED;
    }
#ifndef PRODUCT
    if (!n->is_CFG()) {
      bool found = false;
      for (uint j = 0; j < n->req(); j++) {
        Node* def = n->in(j);
        if (def != nullptr && _vloop.in_body(def)) {
          found = true;
          break;
        }
      }
      assert(found, "every non-cfg node must have an input that is also inside the loop");
    }
#endif
  }

  // Create reverse-post-order list of nodes in body
  ResourceMark rm;
  GrowableArray<Node*> stack;
  VectorSet visited;
  VectorSet post_visited;

  visited.set(body_idx(cl));
  stack.push(cl);

  // Do a depth first walk over out edges
  int rpo_idx = body_count - 1;
  while (!stack.is_empty()) {
    Node* n = stack.top(); // Leave node on stack
    if (!visited.test_set(body_idx(n))) {
      // forward arc in graph
    } else if (!post_visited.test(body_idx(n))) {
      // cross or back arc
      int old_size = stack.length();
      for (DUIterator_Fast imax, i = n->fast_outs(imax); i < imax; i++) {
        Node* use = n->fast_out(i);
        if (_vloop.in_body(use) &&
            !visited.test(body_idx(use)) &&
            // Don't go around backedge
            (!use->is_Phi() || n == cl)) {
          stack.push(use);
        }
      }
      if (stack.length() == old_size) {
        // There were no additional uses, post visit node now
        stack.pop(); // Remove node from stack
        assert(rpo_idx >= 0, "must still have idx to pass out");
        _body.at_put_grow(rpo_idx, n);
        rpo_idx--;
        post_visited.set(body_idx(n));
        assert(rpo_idx >= 0 || stack.is_empty(), "still have idx left or are finished");
      }
    } else {
      stack.pop(); // Remove post-visited node from stack
    }
  }

  // Create real map of block indices for nodes
  for (int j = 0; j < _body.length(); j++) {
    Node* n = _body.at(j);
    set_body_idx(n, j);
  }

#ifndef PRODUCT
  if (_vloop.is_trace_body()) {
    print();
  }
#endif

  assert(rpo_idx == -1 && body_count == _body.length(), "all block members found");
  return nullptr; // success
}

#ifndef PRODUCT
void VLoopBody::print() const {
  tty->print_cr("\nVLoopBody::print:");
  for (int i = 0; i < _body.length(); i++) {
    Node* n = _body.at(i);
    if (n != nullptr) {
      n->dump();
    }
  }
}
#endif

void VLoopDependenceGraph::build() {
  assert(_map.length() == 0, "must be freshly reset");
  CountedLoopNode *cl = _vloop.cl();

  // First, assign a dependence node to each memory node
  for (int i = 0; i < _body.body().length(); i++ ) {
    Node* n = _body.body().at(i);
    if (n->is_Mem() || n->is_memory_phi()) {
      make_node(n);
    }
  }

  const GrowableArray<PhiNode*> &mem_slice_head = _memory_slices.heads();
  const GrowableArray<MemNode*> &mem_slice_tail = _memory_slices.tails();

  ResourceMark rm;
  GrowableArray<Node*> slice_nodes;

  // For each memory slice, create the dependences
  for (int i = 0; i < mem_slice_head.length(); i++) {
    Node* head = mem_slice_head.at(i);
    Node* tail = mem_slice_tail.at(i);

    // Get slice in predecessor order (last is first)
    _memory_slices.get_slice(head, tail, slice_nodes);

    // Make the slice dependent on the root
    DependenceNode* slice_head = get_node(head);
    make_edge(root(), slice_head);

    // Create a sink for the slice
    DependenceNode* slice_sink = make_node(nullptr);
    make_edge(slice_sink, sink());

    // Now visit each pair of memory ops, creating the edges
    for (int j = slice_nodes.length() - 1; j >= 0 ; j--) {
      Node* s1 = slice_nodes.at(j);

      // If no dependency yet, use slice_head
      if (get_node(s1)->in_cnt() == 0) {
        make_edge(slice_head, get_node(s1));
      }
      VPointer p1(s1->as_Mem(), _vloop);
      bool sink_dependent = true;
      for (int k = j - 1; k >= 0; k--) {
        Node* s2 = slice_nodes.at(k);
        if (s1->is_Load() && s2->is_Load()) {
          continue;
        }
        VPointer p2(s2->as_Mem(), _vloop);

        int cmp = p1.cmp(p2);
        if (!VPointer::not_equal(cmp)) {
          // Possibly same address
          make_edge(get_node(s1), get_node(s2));
          sink_dependent = false;
        }
      }
      if (sink_dependent) {
        make_edge(get_node(s1), slice_sink);
      }
    }
  }

  compute_max_depth();

#ifndef PRODUCT
  if(_vloop.is_trace_dependence_graph()) {
    print();
  }
#endif
}

void VLoopDependenceGraph::compute_max_depth() {
  assert(_depth.length() == 0, "must be freshly reset");
  // set all depths to zero
  _depth.at_put_grow(_body.body().length()-1, 0);

  int ct = 0;
  bool again;
  do {
    again = false;
    for (int i = 0; i < _body.body().length(); i++) {
      Node* n = _body.body().at(i);
      if (!n->is_Phi()) {
        int d_orig = depth(n);
        int d_in   = 0;
        for (PredsIterator preds(n, *this); !preds.done(); preds.next()) {
          Node* pred = preds.current();
          if (_vloop.in_body(pred)) {
            d_in = MAX2(d_in, depth(pred));
          }
        }
        if (d_in + 1 != d_orig) {
          set_depth(n, d_in + 1);
          again = true;
        }
      }
    }
    ct++;
  } while (again);

#ifndef PRODUCT
  if (_vloop.is_trace_dependence_graph()) {
    tty->print_cr("\nVLoopDependenceGraph::compute_max_depth iterated: %d times", ct);
  }
#endif
}

bool VLoopDependenceGraph::independent(Node* s1, Node* s2) const {
  int d1 = depth(s1);
  int d2 = depth(s2);

  if (d1 == d2) {
    // Same depth:
    //  1) same node       -> dependent
    //  2) different nodes -> same level implies there is no path
    return s1 != s2;
  }

  // Traversal starting at the deeper node to find the shallower one.
  Node* deep    = d1 > d2 ? s1 : s2;
  Node* shallow = d1 > d2 ? s2 : s1;
  int min_d = MIN2(d1, d2); // prune traversal at min_d

  ResourceMark rm;
  Unique_Node_List worklist;
  worklist.push(deep);
  for (uint i = 0; i < worklist.size(); i++) {
    Node* n = worklist.at(i);
    for (PredsIterator preds(n, *this); !preds.done(); preds.next()) {
      Node* pred = preds.current();
      if (_vloop.in_body(pred) && depth(pred) >= min_d) {
        if (pred == shallow) {
          return false; // found it -> dependent
        }
        worklist.push(pred);
      }
    }
  }
  return true; // not found -> independent
}

// Are all nodes in nodes mutually independent?
// We could query independent(s1, s2) for all pairs, but that results
// in O(size * size) graph traversals. We can do it all in one BFS!
// Start the BFS traversal at all nodes from the nodes list. Traverse
// Preds recursively, for nodes that have at least depth min_d, which
// is the smallest depth of all nodes from the nodes list. Once we have
// traversed all those nodes, and have not found another node from the
// nodes list, we know that all nodes in the nodes list are independent.
bool VLoopDependenceGraph::mutually_independent(Node_List* nodes) const {
  ResourceMark rm;
  Unique_Node_List worklist;
  VectorSet nodes_set;
  int min_d = depth(nodes->at(0));
  for (uint k = 0; k < nodes->size(); k++) {
    Node* n = nodes->at(k);
    min_d = MIN2(min_d, depth(n));
    worklist.push(n); // start traversal at all nodes in nodes list
    nodes_set.set(_body.body_idx(n));
  }
  for (uint i = 0; i < worklist.size(); i++) {
    Node* n = worklist.at(i);
    for (PredsIterator preds(n, *this); !preds.done(); preds.next()) {
      Node* pred = preds.current();
      if (_vloop.in_body(pred) && depth(pred) >= min_d) {
        if (nodes_set.test(_body.body_idx(pred))) { // in nodes list?
          return false;
        }
        worklist.push(pred);
      }
    }
  }
  return true;
}

#ifndef PRODUCT
void VLoopDependenceGraph::print() const {
  tty->print_cr("\nVLoopDependenceGraph::print:");
  // Memory graph
  tty->print_cr("memory root:");
  root()->print();
  tty->print_cr("memory nodes:");
  for (int i = 0; i < _map.length(); i++) {
    DependenceNode* d = _map.at(i);
    if (d != nullptr) {
      d->print();
    }
  }
  tty->print_cr("memory sink:");
  sink()->print();
  // Combined graph
  tty->print_cr("\nDependencies inside combined graph:");
  for (int i = 0; i < _body.body().length(); i++) {
    Node* n = _body.body().at(i);
    tty->print("d:%2d %5d %-10s (", depth(n), n->_idx, n->Name());
    for (PredsIterator preds(n, *this); !preds.done(); preds.next()) {
      Node* pred = preds.current();
      if (_vloop.in_body(pred)) {
        tty->print("%d ", pred->_idx);
      }
    }
    tty->print_cr(")");
  }
}
#endif

VLoopDependenceGraph::DependenceNode*
VLoopDependenceGraph::make_node(Node* node) {
  DependenceNode* m = new (arena()) DependenceNode(node);
  if (node != nullptr) {
    assert(_map.at_grow(node->_idx) == nullptr, "one init only");
    _map.at_put_grow(node->_idx, m);
  }
  return m;
}

VLoopDependenceGraph::DependenceEdge*
VLoopDependenceGraph::make_edge(DependenceNode* dpred, DependenceNode* dsucc) {
  DependenceEdge* e = new (arena()) DependenceEdge(dpred,
                                                   dsucc,
                                                   dsucc->in_head(),
                                                   dpred->out_head());
  dpred->set_out_head(e);
  dsucc->set_in_head(e);
  return e;
}

int VLoopDependenceGraph::DependenceNode::in_cnt() {
  int ct = 0;
  for (DependenceEdge* e = _in_head; e != nullptr; e = e->next_in()) {
    ct++;
  };
  return ct;
}

int VLoopDependenceGraph::DependenceNode::out_cnt() {
  int ct = 0;
  for (DependenceEdge* e = _out_head; e != nullptr; e = e->next_out()) {
    ct++;
  }
  return ct;
}


void VLoopDependenceGraph::DependenceNode::print() const {
#ifndef PRODUCT
  if (_node != nullptr) {
    tty->print("  %4d %-6s (", _node->_idx, _node->Name());
  } else {
    tty->print("  sentinel (");
  }
  for (DependenceEdge* p = _in_head; p != nullptr; p = p->next_in()) {
    Node* pred = p->pred()->node();
    tty->print(" %d", pred != nullptr ? pred->_idx : 0);
  }
  tty->print(") [");
  for (DependenceEdge* s = _out_head; s != nullptr; s = s->next_out()) {
    Node* succ = s->succ()->node();
    tty->print(" %d", succ != nullptr ? succ->_idx : 0);
  }
  tty->print_cr(" ]");
#endif
}

VLoopDependenceGraph::PredsIterator::PredsIterator(Node* n,
                                                   const VLoopDependenceGraph &dg) {
  _n = n;
  _done = false;
  if (_n->is_Store() || _n->is_Load()) {
    // Load: only memory dependencies
    // Store: memory dependence and data input
    _next_idx = MemNode::Address;
    _end_idx  = n->req();
    _dep_next = dg.get_node(_n)->in_head();
  } else if (_n->is_Mem()) {
    _next_idx = 0;
    _end_idx  = 0;
    _dep_next = dg.get_node(_n)->in_head();
  } else {
    // Data node: only has its own edges
    _next_idx = 1;
    _end_idx  = _n->req();
    _dep_next = nullptr;
  }
  next();
}

void VLoopDependenceGraph::PredsIterator::next() {
  if (_dep_next != nullptr) {
    // Have memory preds left
    _current  = _dep_next->pred()->node();
    _dep_next = _dep_next->next_in();
  } else if (_next_idx < _end_idx) {
    // Have data preds left
    _current  = _n->in(_next_idx++);
  } else {
    _done = true;
  }
}

void VLoopTypes::compute_vector_element_type() {
#ifndef PRODUCT
  if (_vloop.is_trace_vector_element_type()) {
    tty->print_cr("\nVLoopTypes::compute_vector_element_type:");
  }
#endif

  assert(_velt_type.length() == 0, "must be freshly reset");
  // reserve space
  _velt_type.at_put_grow(_body.body().length()-1, nullptr);

  // Initial type
  for (int i = 0; i < _body.body().length(); i++) {
    Node* n = _body.body().at(i);
    set_velt_type(n, container_type(n));
  }

  // Propagate integer narrowed type backwards through operations
  // that don't depend on higher order bits
  for (int i = _body.body().length() - 1; i >= 0; i--) {
    Node* n = _body.body().at(i);
    // Only integer types need be examined
    const Type* vtn = velt_type(n);
    if (vtn->basic_type() == T_INT) {
      uint start, end;
      VectorNode::vector_operands(n, &start, &end);

      for (uint j = start; j < end; j++) {
        Node* in  = n->in(j);
        // Don't propagate through a memory
        if (!in->is_Mem() &&
            _vloop.in_body(in) &&
            velt_type(in)->basic_type() == T_INT &&
            data_size(n) < data_size(in)) {
          bool same_type = true;
          for (DUIterator_Fast kmax, k = in->fast_outs(kmax); k < kmax; k++) {
            Node* use = in->fast_out(k);
            if (!_vloop.in_body(use) || !same_velt_type(use, n)) {
              same_type = false;
              break;
            }
          }
          if (same_type) {
            // In any Java arithmetic operation, operands of small integer types
            // (boolean, byte, char & short) should be promoted to int first.
            // During narrowed integer type backward propagation, for some operations
            // like RShiftI, Abs, and ReverseBytesI,
            // the compiler has to know the higher order bits of the 1st operand,
            // which will be lost in the narrowed type. These operations shouldn't
            // be vectorized if the higher order bits info is imprecise.
            const Type* vt = vtn;
            int op = in->Opcode();
            if (VectorNode::is_shift_opcode(op) || op == Op_AbsI || op == Op_ReverseBytesI) {
              Node* load = in->in(1);
              if (load->is_Load() &&
                  _vloop.in_body(load) &&
                  velt_type(load)->basic_type() == T_INT) {
                // Only Load nodes distinguish signed (LoadS/LoadB) and unsigned
                // (LoadUS/LoadUB) values. Store nodes only have one version.
                vt = velt_type(load);
              } else if (op != Op_LShiftI) {
                // Widen type to int to avoid the creation of vector nodes. Note
                // that left shifts work regardless of the signedness.
                vt = TypeInt::INT;
              }
            }
            set_velt_type(in, vt);
          }
        }
      }
    }
  }

  // Look for pattern: Bool -> Cmp -> x.
  // Propagate type down to Cmp and Bool.
  // If this gets vectorized, the bit-mask
  // has the same size as the compared values.
  for (int i = 0; i < _body.body().length(); i++) {
    Node* n = _body.body().at(i);
    Node* nn = n;
    if (nn->is_Bool() && nn->in(0) == nullptr) {
      nn = nn->in(1);
      assert(nn->is_Cmp(), "always have Cmp above Bool");
    }
    if (nn->is_Cmp() && nn->in(0) == nullptr) {
      assert(_vloop.in_body(nn->in(1)) ||
             _vloop.in_body(nn->in(2)),
             "one of the inputs must be in the loop too");
      if (_vloop.in_body(nn->in(1))) {
        set_velt_type(n, velt_type(nn->in(1)));
      } else {
        set_velt_type(n, velt_type(nn->in(2)));
      }
    }
  }

#ifndef PRODUCT
  if (_vloop.is_trace_vector_element_type()) {
    print();
  }
#endif
}

#ifndef PRODUCT
void VLoopTypes::print() const {
  tty->print_cr("\nVLoopTypes::print:");
  for (int i = 0; i < _body.body().length(); i++) {
    Node* n = _body.body().at(i);
    tty->print("  %5d %-10s ", n->_idx, n->Name());
    velt_type(n)->dump();
    tty->cr();
  }
}
#endif

const Type* VLoopTypes::container_type(Node* n) const {
  if (n->is_Mem()) {
    BasicType bt = n->as_Mem()->memory_type();
    if (n->is_Store() && (bt == T_CHAR)) {
      // Use T_SHORT type instead of T_CHAR for stored values because any
      // preceding arithmetic operation extends values to signed Int.
      bt = T_SHORT;
    }
    if (n->Opcode() == Op_LoadUB) {
      // Adjust type for unsigned byte loads, it is important for right shifts.
      // T_BOOLEAN is used because there is no basic type representing type
      // TypeInt::UBYTE. Use of T_BOOLEAN for vectors is fine because only
      // size (one byte) and sign is important.
      bt = T_BOOLEAN;
    }
    return Type::get_const_basic_type(bt);
  }
  const Type* t = _vloop.phase()->igvn().type(n);
  if (t->basic_type() == T_INT) {
    // A narrow type of arithmetic operations will be determined by
    // propagating the type of memory operations.
    return TypeInt::INT;
  }
  return t;
}


