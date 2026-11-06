// POSIX tsearch family, used during libvlc's native initialization. Nodes
// own their storage, never the caller's keys; tdestroy additionally invokes
// the caller's destructor for every remaining key. AVL balancing keeps a
// sorted insertion sequence from making traversal/destruction exhaust a stack.
#include "search_bionic.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace gesher::bionic::search {
namespace {

struct Node {
  const void* key;  // Public API: a returned node can be dereferenced as a key pointer.
  Node* left;
  Node* right;
  int height;
};
static_assert(offsetof(Node, key) == 0);

int Height(const Node* node) { return node ? node->height : 0; }
void Update(Node* node) { node->height = 1 + std::max(Height(node->left), Height(node->right)); }

Node* RotateLeft(Node* node) {
  Node* right = node->right;
  node->right = right->left;
  right->left = node;
  Update(node);
  Update(right);
  return right;
}

Node* RotateRight(Node* node) {
  Node* left = node->left;
  node->left = left->right;
  left->right = node;
  Update(node);
  Update(left);
  return left;
}

Node* Balance(Node* node) {
  Update(node);
  int skew = Height(node->left) - Height(node->right);
  if (skew > 1) {
    if (Height(node->left->left) < Height(node->left->right)) {
      node->left = RotateLeft(node->left);
    }
    return RotateRight(node);
  }
  if (skew < -1) {
    if (Height(node->right->right) < Height(node->right->left)) {
      node->right = RotateRight(node->right);
    }
    return RotateLeft(node);
  }
  return node;
}

Node* Insert(Node* node, const void* key, Compare compare, Node** found) {
  if (node == nullptr) {
    auto* fresh = static_cast<Node*>(std::malloc(sizeof(Node)));
    if (fresh != nullptr) *fresh = {key, nullptr, nullptr, 1};
    *found = fresh;
    return fresh;
  }
  int order = compare(key, node->key);
  if (order == 0) {
    *found = node;
    return node;
  }
  if (order < 0) node->left = Insert(node->left, key, compare, found);
  else node->right = Insert(node->right, key, compare, found);
  return Balance(node);
}

Node* RemoveMinimum(Node* node, const void** key) {
  if (node->left == nullptr) {
    *key = node->key;
    Node* right = node->right;
    std::free(node);
    return right;
  }
  node->left = RemoveMinimum(node->left, key);
  return Balance(node);
}

Node* Remove(Node* node, const void* key, Compare compare) {
  int order = compare(key, node->key);
  if (order < 0) node->left = Remove(node->left, key, compare);
  else if (order > 0) node->right = Remove(node->right, key, compare);
  else if (node->left != nullptr && node->right != nullptr) {
    node->right = RemoveMinimum(node->right, &node->key);
  } else {
    Node* child = node->left ? node->left : node->right;
    std::free(node);
    return child;
  }
  return Balance(node);
}

void Walk(const Node* node, Visit action, int depth) {
  // search.h's VISIT values: preorder, postorder, endorder, leaf.
  if (node->left == nullptr && node->right == nullptr) {
    action(node, 3, depth);
    return;
  }
  action(node, 0, depth);
  if (node->left) Walk(node->left, action, depth + 1);
  action(node, 1, depth);
  if (node->right) Walk(node->right, action, depth + 1);
  action(node, 2, depth);
}

void DestroyTree(Node* node, Destroy free_key) {
  if (node->left) DestroyTree(node->left, free_key);
  if (node->right) DestroyTree(node->right, free_key);
  free_key(const_cast<void*>(node->key));
  std::free(node);
}

}  // namespace

void* BIONIC_SYSV Tsearch(const void* key, void** root, Compare compare) {
  if (root == nullptr) return nullptr;
  Node* found = nullptr;
  *root = Insert(static_cast<Node*>(*root), key, compare, &found);
  return found;
}

void* BIONIC_SYSV Tfind(const void* key, void* const* root, Compare compare) {
  if (root == nullptr) return nullptr;
  auto* node = static_cast<Node*>(*root);
  while (node != nullptr) {
    int order = compare(key, node->key);
    if (order == 0) return node;
    node = order < 0 ? node->left : node->right;
  }
  return nullptr;
}

void* BIONIC_SYSV Tdelete(const void* key, void** root, Compare compare) {
  if (root == nullptr) return nullptr;
  auto* node = static_cast<Node*>(*root);
  Node* parent = nullptr;
  while (node != nullptr) {
    int order = compare(key, node->key);
    if (order == 0) break;
    parent = node;
    node = order < 0 ? node->left : node->right;
  }
  if (node == nullptr) return nullptr;
  // For removal of the root, POSIX leaves the non-null return value unspecified.
  void* result = parent ? parent : node;
  *root = Remove(static_cast<Node*>(*root), key, compare);
  return result;
}

void BIONIC_SYSV Twalk(const void* root, Visit action) {
  if (root != nullptr && action != nullptr) Walk(static_cast<const Node*>(root), action, 0);
}

void BIONIC_SYSV Tdestroy(void* root, Destroy free_key) {
  if (root != nullptr) DestroyTree(static_cast<Node*>(root), free_key);
}

}  // namespace gesher::bionic::search
