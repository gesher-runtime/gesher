// Public tsearch/tfind/tdelete/twalk contracts shared by the host and guest.
// Deliberately does not inspect the implementation's private node layout.
#ifndef TREE_DELETE_CHECK_BALANCE
#define TREE_DELETE_CHECK_BALANCE 1
#endif
#ifndef TREE_DELETE_EXTENDED
#define TREE_DELETE_EXTENDED 0
#endif
static unsigned long long tree_delete_checks, tree_delete_failed;
static void tree_delete_check(const char* what, int ok) {
  ++tree_delete_checks;
  if (!ok && ++tree_delete_failed <= 8) check(what, 0);
}
enum { tree_delete_capacity = 512 };
static int tree_delete_keys[tree_delete_capacity];
static int tree_delete_live[tree_delete_capacity];
static const void* tree_delete_nodes[tree_delete_capacity];
static const void* tree_delete_parents[tree_delete_capacity];
static const void* tree_delete_stack[tree_delete_capacity];
static int tree_delete_depth[tree_delete_capacity];
static int tree_delete_left[tree_delete_capacity];
static int tree_delete_right[tree_delete_capacity];
static int tree_delete_height[tree_delete_capacity];
static unsigned tree_delete_visits[tree_delete_capacity];
static int tree_delete_valid, tree_delete_seen, tree_delete_previous;
static unsigned long long tree_delete_digest = 1469598103934665603ULL;
static unsigned long long tree_delete_comparisons;
static unsigned tree_delete_destroyed[tree_delete_capacity];

static int TREE_CALLBACK_ABI tree_delete_compare(const void* a, const void* b) {
  ++tree_delete_comparisons;
  errno = ERANGE;
  int x = *(const int*)a, y = *(const int*)b;
  return (x > y) - (x < y);
}

static void TREE_CALLBACK_ABI tree_delete_visit(const void* node, VISIT visit, int depth) {
  int key = **(const int* const*)node;
  if (key < 0 || key >= tree_delete_capacity || depth < 0 ||
      depth >= tree_delete_capacity || visit < preorder || visit > leaf) {
    tree_delete_valid = 0;
    return;
  }
  tree_delete_digest ^= (unsigned)(key * 4096 + depth * 4 + visit);
  tree_delete_digest *= 1099511628211ULL;
  unsigned bit = 1u << (unsigned)visit;
  if (tree_delete_visits[key] & bit) tree_delete_valid = 0;
  tree_delete_visits[key] |= bit;
  if (visit == preorder || visit == leaf) {
    ++tree_delete_seen;
    tree_delete_nodes[key] = node;
    tree_delete_depth[key] = depth;
    tree_delete_parents[key] = depth ? tree_delete_stack[depth - 1] : NULL;
    tree_delete_stack[depth] = node;
    if (depth) {
      int parent = **(const int* const*)tree_delete_parents[key];
      if (key < parent) tree_delete_left[parent] = key;
      else tree_delete_right[parent] = key;
    }
  } else if (tree_delete_nodes[key] != node || tree_delete_depth[key] != depth) {
    tree_delete_valid = 0;
  }
  if (visit == postorder || visit == leaf) {
    if (key <= tree_delete_previous) tree_delete_valid = 0;
    tree_delete_previous = key;
  }
  if (visit == endorder || visit == leaf) {
    int left = tree_delete_left[key], right = tree_delete_right[key];
    int lh = left < 0 ? 0 : tree_delete_height[left];
    int rh = right < 0 ? 0 : tree_delete_height[right];
    if (TREE_DELETE_CHECK_BALANCE && (lh - rh > 1 || rh - lh > 1))
      tree_delete_valid = 0;
    tree_delete_height[key] = 1 + (lh > rh ? lh : rh);
  }
}

static void tree_delete_snapshot(void* root, int size) {
  tree_delete_valid = 1;
  tree_delete_seen = 0;
  tree_delete_previous = -1;
  for (int i = 0; i < tree_delete_capacity; ++i) {
    tree_delete_nodes[i] = tree_delete_parents[i] = NULL;
    tree_delete_left[i] = tree_delete_right[i] = -1;
    tree_delete_height[i] = 0;
    tree_delete_depth[i] = -1;
    tree_delete_visits[i] = 0;
  }
  twalk(root, tree_delete_visit);
  int count = 0;
  for (int i = 0; i < size; ++i) {
    count += tree_delete_live[i];
    unsigned visits = tree_delete_visits[i];
    tree_delete_check("tree deletion VISIT membership", tree_delete_live[i] ?
          visits == (1u << leaf) || visits == 7u : visits == 0);
    if (tree_delete_live[i]) {
      tree_delete_check("tree deletion retains caller key", tree_delete_nodes[i] != NULL &&
            *(const void* const*)tree_delete_nodes[i] == &tree_delete_keys[i]);
    }
  }
  tree_delete_check("tree deletion ordered visits/depth/balance", tree_delete_valid &&
        count == tree_delete_seen && ((count == 0) == (root == NULL)));
}

static void TREE_CALLBACK_ABI tree_delete_destroy(void* value) {
  int key = *(int*)value;
  tree_delete_check("tree destructor owns original key", key >= 0 && key < tree_delete_capacity &&
        value == &tree_delete_keys[key]);
  if (key >= 0 && key < tree_delete_capacity) ++tree_delete_destroyed[key];
}

static void test_search_delete_contracts(void) {
  static const int sizes[] = {1, 2, 3, 7, 16, 31, 127,
#if TREE_DELETE_EXTENDED
    257, 511,
#endif
  };
  for (unsigned s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
    int size = sizes[s];
    for (int pattern = 0; pattern < 3; ++pattern) {
      int order[tree_delete_capacity];
      void* root = NULL;
      memset(tree_delete_destroyed, 0, sizeof(tree_delete_destroyed));
      for (int i = 0; i < size; ++i) {
        tree_delete_keys[i] = i;
        tree_delete_live[i] = 1;
        order[i] = i;
      }
      if (pattern == 1) {
        for (int i = 0; i < size / 2; ++i) {
          int tmp = order[i]; order[i] = order[size - 1 - i]; order[size - 1 - i] = tmp;
        }
      } else if (pattern == 2) {
        unsigned seed = 0x1948b057u;
        for (int i = size - 1; i > 0; --i) {
          seed = seed * 1664525u + 1013904223u;
          unsigned j = seed % (unsigned)(i + 1);
          int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
        }
      }
      for (int i = 0; i < size; ++i) {
        int key = order[i];
        const int* const* found = (const int* const*)tsearch(&tree_delete_keys[key], &root,
                                                          tree_delete_compare);
        tree_delete_check("tree insert original key", found != NULL && *found == &tree_delete_keys[key]);
      }
      tree_delete_snapshot(root, size);
      for (int step = 0; step < size; ++step) {
        // Root, extreme and shuffled removals cover different successor/rotation shapes.
        int key;
        if (pattern == 0) key = **(const int* const*)root;
        else if (pattern == 1) key = step;
        else key = order[(step * 197) % size];
        // 197 is coprime to every fixed size in this corpus.
        tree_delete_check("tree selected key still live", tree_delete_live[key]);
        int probe = key;
        const void* before = tfind(&probe, &root, tree_delete_compare);
        const void* parent = tree_delete_parents[key];
        int depth = tree_delete_depth[key];
        tree_delete_check("tree duplicate keeps original object", tsearch(&probe, &root,
              tree_delete_compare) == before && *(const void* const*)before == &tree_delete_keys[key]);
        tree_delete_comparisons = 0;
        errno = EDOM;
        void* removed = tdelete(&probe, &root, tree_delete_compare);
        tree_delete_check("tree deletion original parent or nonnull root result", parent ? removed == parent : removed != NULL);
        tree_delete_check("tree deletion comparator errno", errno == ERANGE);
        tree_delete_check("tree deletion bounded stable comparisons", tree_delete_comparisons >= (unsigned)(depth + 1) &&
              tree_delete_comparisons <= 2u * (unsigned)(depth + 1));
        tree_delete_live[key] = 0;
        for (int i = 0; i < size; ++i) {
          const int* const* found = (const int* const*)tfind(&tree_delete_keys[i], &root,
                                                          tree_delete_compare);
          tree_delete_check("tree deletion exact remaining set", tree_delete_live[i] ?
                found != NULL && *found == &tree_delete_keys[i] : found == NULL);
          tree_delete_check("tree deletion does not free caller key", tree_delete_keys[i] == i &&
                tree_delete_destroyed[i] == 0);
        }
        void* unchanged = root;
        tree_delete_check("tree repeated delete misses", tdelete(&probe, &root, tree_delete_compare) == NULL &&
              root == unchanged);
        tree_delete_snapshot(root, size);
      }
      tree_delete_check("tree drain leaves null root", root == NULL);
      tree_delete_comparisons = 0;
      errno = EDOM;
      tree_delete_check("tree null root pointer has no callback", tdelete(&tree_delete_keys[0], NULL,
            tree_delete_compare) == NULL && tree_delete_comparisons == 0 && errno == EDOM);
      tree_delete_check("tree empty root has no callback", tdelete(&tree_delete_keys[0], &root,
            tree_delete_compare) == NULL && tree_delete_comparisons == 0 && errno == EDOM);
      for (int i = 0; i < size; ++i) {
        tree_delete_check("tree rebuild", tsearch(&tree_delete_keys[i], &root, tree_delete_compare) != NULL);
      }
      for (int i = 0; i < size; i += 2) {
        tree_delete_check("tree partial deletion", tdelete(&tree_delete_keys[i], &root, tree_delete_compare) != NULL);
      }
      tdestroy(root, tree_delete_destroy);
      root = NULL;
      for (int i = 0; i < size; ++i) {
        tree_delete_check("tree destructor called only for retained keys", tree_delete_destroyed[i] == (unsigned)(i % 2));
      }
      tdestroy(NULL, tree_delete_destroy);
    }
  }
  check("extended tree deletion contracts", tree_delete_failed == 0);
}
