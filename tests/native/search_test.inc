// Included by libc_test.c so the same search checks run from x86-64 and
// translated arm64 ELF libraries, including callbacks back into the library.
static int tree_visits[128][4];
static int tree_depths[128];
static int tree_last_key;
static int tree_ordered;
static int tree_destroyed;
static int tree_destroy_sum;

#ifndef TREE_CALLBACK_ABI
#define TREE_CALLBACK_ABI
#endif

static void TREE_CALLBACK_ABI tree_visit(const void* node, VISIT visit, int depth) {
  int key = **(const int* const*)node;
  if (key < 0 || key >= 128 || visit < preorder || visit > leaf || depth < 0) {
    tree_ordered = 0;
    return;
  }
  ++tree_visits[key][visit];
  if (tree_depths[key] != -1 && tree_depths[key] != depth) tree_ordered = 0;
  tree_depths[key] = depth;
  if (visit == postorder || visit == leaf) {
    if (key <= tree_last_key) tree_ordered = 0;
    tree_last_key = key;
  }
}

static void TREE_CALLBACK_ABI tree_free_key(void* key) {
  ++tree_destroyed;
  tree_destroy_sum += *(int*)key;
  free(key);
}
#undef TREE_CALLBACK_ABI

static void test_search(void) {
  void* root = NULL;
  int keys[128];
  int inserted = 1;
  // In-order inserts also exercise growth/rotation paths, and leave the
  // original key objects (not copies) in the returned nodes.
  for (int i = 0; i < 128; ++i) {
    keys[i] = i;
    const int* const* node = (const int* const*)tsearch(&keys[i], &root, compare_ints);
    inserted &= node != NULL && *node == &keys[i];
  }
  check("tsearch inserts caller-owned keys", inserted);
  int duplicate = 63;
  const void* original = tfind(&duplicate, &root, compare_ints);
  const int* const* repeated = (const int* const*)tsearch(&duplicate, &root, compare_ints);
  check("tsearch duplicate retains node and key", repeated != NULL &&
        repeated == original && *repeated == &keys[63]);
  int absent = 128;
  check("tfind missing key and null root", tfind(&absent, &root, compare_ints) == NULL &&
        tfind(&absent, NULL, compare_ints) == NULL);
  check("tsearch null root argument", tsearch(&absent, NULL, compare_ints) == NULL);

  memset(tree_visits, 0, sizeof(tree_visits));
  for (int i = 0; i < 128; ++i) tree_depths[i] = -1;
  tree_last_key = -1;
  tree_ordered = 1;
  twalk(root, tree_visit);
  int walked = tree_ordered && tree_last_key == 127;
  for (int i = 0; i < 128; ++i) {
    int* visits = tree_visits[i];
    walked &= (visits[leaf] == 1 && visits[preorder] == 0 &&
               visits[postorder] == 0 && visits[endorder] == 0) ||
              (visits[leaf] == 0 && visits[preorder] == 1 &&
               visits[postorder] == 1 && visits[endorder] == 1);
  }
  check("twalk visits nodes in order with VISIT/depth", walked);
  twalk(NULL, tree_visit);
  twalk(root, NULL);
  check("tdelete missing key leaves tree intact", tdelete(&absent, &root, compare_ints) == NULL &&
        tfind(&keys[63], &root, compare_ints) == original);

  int deleted = 1;
  int remaining[128];
  for (int i = 0; i < 128; ++i) remaining[i] = 1;
  // A permutation removes internal nodes, leaves and ultimately the root.
  for (int i = 0; i < 128; ++i) {
    int index = (i * 37) % 128;
    deleted &= tdelete(&keys[index], &root, compare_ints) != NULL;
    remaining[index] = 0;
    for (int j = 0; j < 128; ++j) {
      const int* const* node = (const int* const*)tfind(&keys[j], &root, compare_ints);
      deleted &= remaining[j] ? node != NULL && *node == &keys[j] : node == NULL;
    }
  }
  check("tdelete preserves other keys; final root null", deleted && root == NULL);
  check("tdelete empty tree and null root", tdelete(&absent, &root, compare_ints) == NULL &&
        tdelete(&absent, NULL, compare_ints) == NULL);

  tree_destroyed = 0;
  tree_destroy_sum = 0;
  int allocated = 1;
  for (int i = 0; i < 128; ++i) {
    int* key = (int*)malloc(sizeof(int));
    if (key == NULL) { allocated = 0; break; }
    *key = i;
    if (tsearch(key, &root, compare_ints) == NULL) { free(key); allocated = 0; break; }
  }
  tdestroy(root, tree_free_key);
  root = NULL;
  tdestroy(NULL, tree_free_key);
  check("tdestroy frees every key exactly once via callback", allocated &&
        tree_destroyed == 128 && tree_destroy_sum == (127 * 128) / 2);
}
