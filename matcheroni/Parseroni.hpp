#pragma once
#include <stdlib.h>  // for malloc/free
#include <string.h>
#include <stdint.h>

#include "matcheroni/Matcheroni.hpp"

//#define PARSERONI_FAST_MODE

namespace matcheroni {

//------------------------------------------------------------------------------
// This is an optimized allocator for Parseroni - it allows for alloc/free, but
// frees must be in LIFO order - if you allocate A, B, and C, you must
// deallocate in C-B-A order.

struct LinearAlloc {
  struct Slab {
    Slab* prev;
    Slab* next;
    int cursor;
    int highwater;
    char buf[];
  };

  struct SlabState {
    Slab* top_slab;
    int   top_cursor;
    int   current_size;
  };

  SlabState save_state() {
    return {
      top_slab,
      top_slab->cursor,
      current_size
    };
  }

  void restore_state(SlabState s) {
    top_slab = s.top_slab;
    top_slab->cursor = s.top_cursor;
    current_size = s.current_size;
  }

  // slab size is 1 hugepage. seems to work ok.
  static constexpr int header_size = sizeof(Slab);
  static constexpr int slab_size = 2 * 1024 * 1024 - header_size;

  LinearAlloc() { add_slab(); }

  void reset() {
    if (!top_slab) add_slab();
    while (top_slab->prev) top_slab = top_slab->prev;
    for (auto c = top_slab; c; c = c->next) {
      c->cursor = 0;
    }
    max_size = 0;
    current_size = 0;
  }

  void destroy() {
    reset();
    auto c = top_slab;
    while (c) {
      auto next = c->next;
      ::free((void*)c);
      c = next;
    }
    top_slab = nullptr;
  }

  void add_slab() {
    if (top_slab && top_slab->next) {
      top_slab = top_slab->next;
      matcheroni_assert(top_slab->cursor == 0);
      return;
    }

    auto new_slab = (Slab*)malloc(header_size + slab_size);
    new_slab->prev = nullptr;
    new_slab->next = nullptr;
    new_slab->cursor = 0;
    new_slab->highwater = 0;

    if (top_slab) top_slab->next = new_slab;
    new_slab->prev = top_slab;
    top_slab = new_slab;
  }

  void* alloc(int size) {
    if (top_slab->cursor + size > slab_size) {
      add_slab();
    }

    auto result = top_slab->buf + top_slab->cursor;
    top_slab->cursor += size;

    current_size += size;
    if (current_size > max_size) max_size = current_size;

    return result;
  }

  void free(void* p, int size) {
    top_slab->cursor -= size;
    if (top_slab->cursor == 0) {
      if (top_slab->prev) {
        top_slab = top_slab->prev;
      }
    }
    current_size -= size;
  }

  static LinearAlloc& inst() {
    static LinearAlloc inst;
    return inst;
  }

  bool is_empty() const {
    return current_size == 0;
  }

  Slab* top_slab = nullptr;
  int current_size = 0;
  int max_size = 0;
  int refcount = 0;
};

//------------------------------------------------------------------------------

template<typename atom>
struct NodeBase {
  using SpanType = Span<atom>;

  NodeBase() {}
  virtual ~NodeBase() {}

  static void* operator new     (size_t s)          { return LinearAlloc::inst().alloc(s); }
  static void* operator new[]   (size_t s)          { return LinearAlloc::inst().alloc(s); }
  static void  operator delete  (void* p, size_t s) { LinearAlloc::inst().free(p, s); }
  static void  operator delete[](void* p, size_t s) { LinearAlloc::inst().free(p, s); }

  //----------------------------------------

  void init(const char* match_name, SpanType span, uint64_t flags) {
    this->match_name = match_name;
    this->span = span;
    this->flags = flags;
  }

  NodeBase* child(const char* name) {
    for (auto c = _child_head; c; c = c->_node_next) {
      if (strcmp(name, c->match_name) == 0) return c;
    }
    return nullptr;
  }

  size_t node_count() {
    size_t accum = 1;
    for (auto c = _child_head; c; c = c->_node_next) accum += c->node_count();
    return accum;
  }

  NodeBase* node_prev() { return _node_prev; }
  NodeBase* node_next() { return _node_next; }

  const NodeBase* node_prev() const { return _node_prev; }
  const NodeBase* node_next() const { return _node_next; }

  NodeBase* child_head() { return _child_head; }
  NodeBase* child_tail() { return _child_tail; }

  const NodeBase* child_head() const { return _child_head; }
  const NodeBase* child_tail() const { return _child_tail; }

  //----------------------------------------

  const char* match_name;
  SpanType    span;
  uint64_t    flags;

  NodeBase*   _node_prev;
  NodeBase*   _node_next;
  NodeBase*   _child_head;
  NodeBase*   _child_tail;
};

//------------------------------------------------------------------------------

template<typename SpanType, typename NodeType>
struct NodeContext {

  NodeContext() {
  }

  virtual ~NodeContext() {
    reset();
  }

  void reset() {
    auto c = _top_tail;
    while (c) {
      auto prev = c->_node_prev;
      recycle(c);
      c = (NodeType*)prev;
    }

    _top_head = nullptr;
    _top_tail = nullptr;
    _highwater = nullptr;

    assert(LinearAlloc::inst().is_empty());
  }

  //----------------------------------------

  size_t node_count() {
    size_t accum = 0;
    for (auto c = _top_head; c; c = c->_node_next) accum += c->node_count();
    return accum;
  }

  //----------------------------------------

  NodeType* top_head() { return _top_head; }
  NodeType* top_tail() { return _top_tail; }

  const NodeType* top_head() const { return _top_head; }
  const NodeType* top_tail() const { return _top_tail; }

  void set_head(NodeType* head) { _top_head = head; }
  void set_tail(NodeType* tail) { _top_tail = tail; }

  //----------------------------------------

  void append(NodeType* new_node) {
    new_node->_node_prev = nullptr;
    new_node->_node_next = nullptr;
    new_node->_child_head = nullptr;
    new_node->_child_tail = nullptr;

    if (_top_tail) {
      new_node->_node_prev = _top_tail;
      _top_tail->_node_next = new_node;
      _top_tail = new_node;
    } else {
      _top_head = new_node;
      _top_tail = new_node;
    }
  }

  //----------------------------------------

  void detach(NodeType* n) {
    if (n->_node_prev) n->_node_prev->_node_next = n->_node_next;
    if (n->_node_next) n->_node_next->_node_prev = n->_node_prev;
    if (_top_head == n) _top_head = n->_node_next;
    if (_top_tail == n) _top_tail = n->_node_prev;
    n->_node_prev = nullptr;
    n->_node_next = nullptr;
  }

  //----------------------------------------

  void splice(NodeType* new_node, NodeType* child_head, NodeType* child_tail) {
    new_node->_node_prev = child_head->_node_prev;
    new_node->_node_next = child_tail->_node_next;
    new_node->_child_head = child_head;
    new_node->_child_tail = child_tail;

    if (child_head->_node_prev) child_head->_node_prev->_node_next = new_node;
    if (child_tail->_node_next) child_head->_node_next->_node_prev = new_node;

    child_head->_node_prev = nullptr;
    child_tail->_node_next = nullptr;

    if (_top_head == child_head) _top_head = new_node;
    if (_top_tail == child_tail) _top_tail = new_node;
  }

  //----------------------------------------
  // There's one critical detail we need to make the factory work correctly - if
  // we get partway through a match and then fail for some reason, we must
  // "rewind" our match state back to the start of the failed match. This means
  // we must also throw away any parse nodes that were created during the failed
  // match.

  void rewind(SpanType s) {
    while (_top_tail && (_top_tail->span.a >= s.a)) {
      auto dead = _top_tail;
      set_tail((NodeType*)_top_tail->_node_prev);
#ifndef FAST_MODE
      recycle(dead);
#endif
    }
  }

  //----------------------------------------

  void create2(NodeType* new_node, const char* match_name, SpanType span, uint64_t flags, NodeType* old_tail) {
    new_node->init(match_name, span, flags);

    if (span.b > _highwater) _highwater = span.b;

    // Move all nodes in (old_tail,new_tail] to be children of new_node and
    // append new_node to the node list.

    if (old_tail == _top_tail) {
      append(new_node);
    } else {
      auto child_head = old_tail ? old_tail->_node_next : _top_head;
      auto child_tail = _top_tail;
      splice(new_node, (NodeType*)child_head, child_tail);
    }
  }


  NodeType* enclose_bookmark(NodeType* old_tail, SpanType bounds) {
    //----------------------------------------
    // Scan down the node list to find the bookmark

    auto node_b = old_tail ? old_tail->node_next() : top_head();
    for (; node_b; node_b = node_b->node_next()) {
      if (node_b->flags & 1) {
        break;
      }
    }

    if (node_b == nullptr) {
      // No bookmark = no capture, but _not_ a failure
      return node_b;
    }

    /*
    // FIXME - this might be faster than tracking old_tail? Benchmark it.
    // Could use it for create2 also
    auto node_a = node_b;
    while(1) {
      auto prev = node_a->node_prev();
      if (!prev) break;
      if (prev->span.b <= bounds.a) {
        break;
      }
      node_a = prev;
    }
    */

    //----------------------------------------
    // Resize the bookmark's span and clear its bookmark flag

    node_b->span = bounds;
    node_b->flags &= ~1;

    //----------------------------------------
    // Enclose its children

    if (node_b->node_prev() != old_tail) {
      auto child_head = old_tail ? old_tail->node_next() : top_head();
      //auto child_head = node_a;
      auto child_tail = node_b->node_prev();
      detach(node_b);
      splice(node_b, child_head, child_tail);
    }

    return node_b;
  }

  //----------------------------------------
  // Nodes _must_ be deleted in the reverse order they were allocated.
  // In practice, this means we must delete the "parent" node first and then
  // must delete the child nodes from tail to head.

#ifndef FAST_MODE
  void recycle(NodeType* node) {
    //printf("recycle!\n");
    if (node == nullptr) return;

    auto tail = node->_child_tail;

    delete node;

    while (tail) {
      auto prev = tail->_node_prev;
      recycle((NodeType*)tail);
      tail = prev;
    }
  }
#endif

  //----------------------------------------

  NodeType* _top_head = nullptr;
  NodeType* _top_tail = nullptr;
  const SpanType::AtomType* _highwater = nullptr;
  int trace_depth = 0;
};

//------------------------------------------------------------------------------
// We'll be parsing text a lot, so these are convenience declarations.

using TextNode = NodeBase<char>;

struct TextNodeContext : public NodeContext<TextSpan, TextNode> {

  static bool atom_eq(char a, char b) { return a == b; }
  static bool atom_lt(char a, char b) { return a < b; }
  static bool atom_gt(char a, char b) { return a > b; }

  TextNode* top_head() { return _top_head; }
  TextNode* top_tail() { return _top_tail; }

  const TextNode* top_head() const { return _top_head; }
  const TextNode* top_tail() const { return _top_tail; }

  void set_head(TextNode* head) { _top_head = head; }
  void set_tail(TextNode* tail) { _top_tail = tail; }
};

//------------------------------------------------------------------------------
// To convert our pattern matches to parse nodes, we create a Capture<>
// matcher that constructs a new NodeType() for a successful match, attaches
// any sub-nodes to it, and places it on the context's node list.

template<typename context, typename atom, typename node_type>
inline Span<atom> capture(context& ctx, Span<atom> s, const char* match_name, matcher_function<context, atom> match) {

  auto old_tail = ctx.top_tail();
#ifdef PARSERONI_FAST_MODE
  auto old_state = LinearAlloc::inst().save_state();
#endif

  auto end = match(ctx, s);

  if (end.is_valid()) {
    //printf("Capture %s\n", match_name);
    Span<atom> node_span = {s.a, end.a};
    auto new_node = new node_type();
    ctx.create2(new_node, match_name, node_span, 0, old_tail);
  }
  else {
    // We don't set the tail back here, rewind will do it.
#ifdef PARSERONI_FAST_MODE
    LinearAlloc::inst().restore_state(old_state);
#endif
  }

  return end;
}

template <StringParam match_name, typename pattern, typename node_type>
struct Capture {
  template<typename context, typename atom>
  static Span<atom> match(context& ctx, Span<atom> s) {
    return capture<context, atom, node_type>(ctx, s, match_name.str_val, pattern::match);
  }
};

//------------------------------------------------------------------------------
// Suffixes are a problem. A pattern that has to match a bunch of stuff before
// failing due to a suffix can cause an exponential increase in parse time.

// To partly work around this, we can split capture into two pieces:

// CaptureBegin<> behaves like Seq<> and encloses things to capture.

// CaptureEnd<> marks the ending point of a capture and defines the match name
// and type of the capture.

// There should be at most one reachable CaptureEnd per CaptureBegin.

// If no CaptureEnd is hit inside a CaptureBegin, the capture does not occur
// but this is _not_ an error.

template <typename context, typename atom>
inline Span<atom> capture_begin(context& ctx, Span<atom> s, matcher_function<context, atom> match) {
  using SpanType = Span<atom>;

  auto old_tail = ctx.top_tail();
#ifdef PARSERONI_FAST_MODE
    auto old_state = LinearAlloc::inst().save_state();
#endif

  auto end = match(ctx, s);
  if (end.is_valid()) {
    Span<atom> bounds(s.a, end.a);
    ctx.enclose_bookmark(old_tail, bounds);
  }
  else {
#ifdef PARSERONI_FAST_MODE
    LinearAlloc::inst().restore_state(old_state);
#endif
  }
  return end;
}

//----------------------------------------

template <typename node_type, typename... rest>
struct CaptureBegin {
  template<typename context, typename atom>
  static Span<atom> match(context& ctx, Span<atom> s) {
    return capture_begin<context, atom>(ctx, s, Seq<rest...>::match);
  }
};

//----------------------------------------

template<typename context, typename atom, typename node_type>
inline Span<atom> capture_end(context& ctx, Span<atom> s, const char* match_name, matcher_function<context, atom> match) {

  auto end = match(ctx, s);
  if (end.is_valid()) {
    Span<atom> new_span(end.a, end.a);
    auto new_node = new node_type();
    ctx.create2(new_node, match_name, new_span, /*flags*/ 1, ctx.top_tail());
  }
  return end;
}

//----------------------------------------

template<StringParam match_name, typename P, typename node_type>
struct CaptureEnd {
  template<typename context, typename atom>
  static Span<atom> match(context& ctx, Span<atom> s) {
    return capture_end<context, atom, node_type>(ctx, s, match_name.str_val, P::match);
  }
};

//------------------------------------------------------------------------------

}; // namespace matcheroni
